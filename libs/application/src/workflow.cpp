#include "runmark/workflow.h"

#include "runmark/project_config.h"
#include "runmark/rules.h"
#include "runmark/error.h"
#include "config_io.h"
#include "git.h"
#include "handoff.h"
#include "ledger.h"
#include "paths.h"
#include "plan.h"
#include "transcript.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>

namespace runmark {
namespace {

// Orchestration joins two separate duties: creating directories belongs to
// paths, writing excludes to git. Calling both together is the command's job.
void prepareState(const Paths& paths)
{
    ensureDirectories(paths);
    ensureGitExcludes(paths);
}

const RepositoryConfig& repositoryFor(const ProjectConfig& config, const QString& name)
{
    if (name.isEmpty() && config.repositories.size() == 1) {
        return config.repositories.constFirst();
    }
    for (const RepositoryConfig& repository : config.repositories) {
        if (repository.name == name) {
            return repository;
        }
    }
    fail(QStringLiteral("Unknown repository: %1").arg(name.isEmpty() ? QStringLiteral("(select --repo)") : name));
}

// What a live session is working on, for the conflict radar: tasks of the
// executions it started and files changed since each baseline (its own
// directory from its start HEAD, each worktree from the execution's base),
// uncommitted changes included. Local git only.
void observeSessionWork(SessionFacts& session, const Ledger& ledger)
{
    QVector<QPair<QString, QString>> places;
    if (!session.startHead.isEmpty()) places.append({session.cwd, session.startHead});
    for (const ExecutionStarted& started : ledger.started) {
        if (started.sessionId != session.id) continue;
        session.tasks.append(started.task);
        places.append({started.worktree, started.baseSha});
    }
    for (const auto& [place, baseline] : places) {
        if (place.isEmpty() || !QFileInfo(place).isDir()) continue;
        const ProcessResult diff = git(place, {QStringLiteral("diff"), QStringLiteral("--name-only"), baseline});
        const ProcessResult common = git(place, {QStringLiteral("rev-parse"), QStringLiteral("--path-format=absolute"), QStringLiteral("--git-common-dir")});
        if (diff.exitCode != 0 || common.exitCode != 0) continue;
        for (const QString& file : diff.output.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
            session.changedFiles.append(common.output.trimmed() + QStringLiteral("//") + file);
        }
    }
    session.changedFiles.removeDuplicates();
}

// Sessions of the last three days that ran in this project (its root, its
// worktree root or a repository) but were never recorded by a hook.
// ponytail: three days bounds the scan; widen it if status proves fast enough.
QVector<SessionFacts> unregisteredSessions(const ProjectConfig& config, const Paths& paths,
    const QVector<SessionFacts>& registered, const QDateTime& now)
{
    QStringList directories{paths.root, expandPath(config.worktreeRoot, paths.root)};
    for (const RepositoryConfig& repository : config.repositories) directories.append(expandPath(repository.path, paths.root));
    QSet<QString> known;
    for (const SessionFacts& session : registered) known.insert(session.id);
    QVector<SessionFacts> result;
    for (const TranscriptSession& found : recentTranscriptSessions(now.addDays(-3))) {
        if (known.contains(found.id)) continue;
        // A removed worktree still counts: fall back to the path as written.
        const QString canonical = QFileInfo(found.cwd).canonicalFilePath();
        const QString cwd = canonical.isEmpty() ? QDir::cleanPath(found.cwd) : canonical;
        for (const QString& directory : directories) {
            if (!isInsideDirectory(cwd, directory)) continue;
            SessionFacts session;
            session.id = found.id;
            session.runtime = found.runtime;
            session.cwd = found.cwd;
            session.transcriptPath = found.path;
            result.append(session);
            break;
        }
    }
    return result;
}

// OBSERVE: run git, read the plan, read the ledger, measure what exists on
// disk. No evaluation happens here.
StatusFacts observe(const ProjectConfig& config, const Paths& paths, bool fetch)
{
    StatusFacts facts;
    facts.now = QDateTime::currentDateTimeUtc();
    facts.ledger = readLedger(paths);
    facts.plan = observePlan(config, paths.root);

    for (const RepositoryConfig& repository : config.repositories) {
        facts.repos.append(observeRepo(repository, expandPath(repository.path, paths.root), fetch));
    }

    facts.sessions = readSessions(paths, &facts.sessionsError);
    for (SessionFacts& session : facts.sessions) {
        if (session.endedAt) continue;
        observeSessionWork(session, facts.ledger);
    }
    facts.unregisteredSessions = unregisteredSessions(config, paths, facts.sessions, facts.now);

    for (const ExecutionStarted& started : facts.ledger.started) {
        ExecutionFacts execution;
        execution.exec = started.exec;
        execution.hasHandoff = QFileInfo::exists(QDir(paths.handoffs).filePath(execution.exec + QStringLiteral(".md")));
        execution.worktreeExists = !started.worktree.isEmpty() && QFileInfo::exists(started.worktree);
        execution.agent = started.agent;
        // The newest timestamp across this execution's events. Scanning is
        // cheap and honest: no separate heartbeat to fall out of sync with the
        // record it claims to describe.
        const auto touch = [&execution](const QString& exec, const QDateTime& at) {
            if (exec == execution.exec && at.isValid() && (!execution.lastActivity.isValid() || at > execution.lastActivity)) {
                execution.lastActivity = at;
            }
        };
        for (const ExecutionStarted& e : facts.ledger.started) touch(e.exec, e.at);
        for (const ExecutionFinished& e : facts.ledger.finished) touch(e.exec, e.at);
        for (const EvidenceRecorded& e : facts.ledger.evidence) touch(e.exec, e.at);
        for (const NoteRecorded& e : facts.ledger.notes) touch(e.exec, e.at);
        facts.executions.append(execution);
    }
    return facts;
}

// Test runs the agent runtime recorded between start and now become one
// runtime evidence event. Returns what happened to the transcript, so an
// unreadable one surfaces as a finding instead of "no tests ran".
// ponytail: attribution is by session and time window; two executions driven
// from one session at the same time share their test runs.
TranscriptRecord recordRuntimeTests(const ProjectConfig& config, const Paths& paths, const ExecutionStarted& started, Ledger& ledger)
{
    if (started.sessionId.isEmpty()) return {QStringLiteral("no_session")};
    const TranscriptFacts transcript = readTranscript(started.agent, started.sessionId, started.at);
    if (!transcript.error.isEmpty()) return {QStringLiteral("unavailable"), {}, transcript.error};

    const QRegularExpression testPattern(config.testCommandPattern);
    QJsonArray runs;
    const TranscriptCommand* last = nullptr;
    for (const TranscriptCommand& command : transcript.commands) {
        if (!testPattern.match(command.command).hasMatch()) continue;
        const bool covered = exitCodeCoversTestRun(command.command, config.testCommandPattern);
        runs.append(QJsonObject{{QStringLiteral("at"), command.at.toString(Qt::ISODateWithMs)},
            {QStringLiteral("command"), command.command},
            {QStringLiteral("exit_code"), covered ? QJsonValue(command.exitCode) : QJsonValue::Null}});
        last = &command;
    }
    const TranscriptRecord record{QStringLiteral("read"), transcript.path, {}, int(transcript.commands.size()), int(runs.size())};
    if (!last) return record;

    EvidenceRecorded evidence;
    evidence.exec = started.exec;
    evidence.at = QDateTime::currentDateTimeUtc();
    evidence.task = started.task;
    evidence.kind = QStringLiteral("test");
    evidence.fromRuntime = true;
    evidence.runtime = started.agent;
    // `ctest | tail` exits with tail's status: record that as unknown, not 0.
    if (exitCodeCoversTestRun(last->command, config.testCommandPattern)) evidence.exitCode = last->exitCode;
    evidence.ref = writeEvidence(paths, {{QStringLiteral("transcript"), transcript.path}, {QStringLiteral("runs"), runs}});
    evidence.summary = QStringLiteral("%1 test run(s); last: %2 (%3)")
        .arg(QString::number(runs.size()), last->command.left(200),
            evidence.exitCode ? QStringLiteral("exit ") + QString::number(*evidence.exitCode)
                              : QStringLiteral("exit code unknown: a later command owns it"));
    appendEvent(paths, evidence);
    ledger.evidence.append(evidence);
    return record;
}

// The agent session rmk runs inside, if any. Codex exports CODEX_THREAD_ID,
// Claude Code CLAUDE_CODE_SESSION_ID.
// ponytail: Codex launched from Claude inherits both and Codex wins, which is
// the innermost session; the reverse nesting would pick the wrong one.
QString currentSession()
{
    for (const char* name : {"CODEX_THREAD_ID", "CLAUDE_CODE_SESSION_ID"}) {
        const QString id = qEnvironmentVariable(name);
        if (isSafeSessionId(id)) return id;
    }
    return {};
}

// Measured transcript locations: ~/.codex/sessions/..., ~/.claude/projects/...
QString runtimeOf(const QString& transcriptPath)
{
    if (transcriptPath.contains(QStringLiteral("/.codex/"))) return QStringLiteral("codex");
    if (transcriptPath.contains(QStringLiteral("/.claude/"))) return QStringLiteral("claude");
    return QStringLiteral("unknown");
}

QString headOf(const QString& directory)
{
    if (directory.isEmpty()) return {};
    const ProcessResult result = git(directory, {QStringLiteral("rev-parse"), QStringLiteral("HEAD")});
    return result.exitCode == 0 ? result.output.trimmed() : QString();
}

// Notes this session wrote: in its own file and in any execution ledger.
QVector<NoteRecorded> notesOf(const Paths& paths, const SessionFacts& session, const Ledger* ledger = nullptr)
{
    QVector<NoteRecorded> notes = session.notes;
    const Ledger loaded = ledger ? Ledger{} : readLedger(paths);
    for (const NoteRecorded& note : (ledger ? *ledger : loaded).notes) {
        if (note.session == session.id) notes.append(note);
    }
    return notes;
}

} // namespace

ProjectConfig inspectProject(const QString& configPath)
{
    return loadProjectConfig(configPath);
}

StatusResult projectStatus(const QString& configPath, bool fetch)
{
    const ProjectConfig config = loadProjectConfig(configPath);
    const Paths paths = pathsFor(configPath);
    prepareState(paths);

    const StatusFacts facts = observe(config, paths, fetch);

    return {config.name, facts.repos, evaluate(config, facts), facts.sessions, facts.plan, openExecutions(facts.ledger)};
}

ResumeResult resumeExecution(const QString& configPath, const QString& taskOrExecution)
{
    const ProjectConfig config = loadProjectConfig(configPath);
    const Paths paths = pathsFor(configPath);
    ResumeFacts facts = observeResumeLedger(paths, taskOrExecution);
    if (facts.started) {
        facts.planSha1 = observePlan(config, paths.root).sha1For(facts.started->planRef);
        readHandoff(paths, facts);
        observeResumeGit(config, paths, facts);
    }
    return {facts, evaluateResume(facts)};
}

StartResult startExecution(const QString& configPath, const QString& task, const QString& agent, const QString& repositoryName, const QStringList& instructions)
{
    const ProjectConfig config = loadProjectConfig(configPath);
    const Paths paths = pathsFor(configPath);
    prepareState(paths);

    const QRegularExpression taskPattern(config.taskIdPattern);
    const QRegularExpressionMatch taskMatch = taskPattern.match(task);
    if (!taskMatch.hasMatch() || taskMatch.capturedLength() != task.size() || task.contains(QLatin1Char('/')) || task.contains(QStringLiteral(".."))) {
        fail(QStringLiteral("Task does not match project.task_id_pattern: %1").arg(task));
    }
    if (agent != QLatin1String("codex") && agent != QLatin1String("claude")) {
        fail(QStringLiteral("Agent must be codex or claude"));
    }
    const Ledger ledger = readLedger(paths);
    std::optional<OpenExecution> open;
    for (const OpenExecution& execution : openExecutions(ledger)) {
        if (execution.started.task == task) open = execution;
    }
    if (open && open->outcome.isEmpty()) {
        fail(QStringLiteral("Refusing start: %1 is still open in %2. Continue it with `rmk resume %1`, "
            "or close it with `rmk finish %1 --outcome interrupted`").arg(open->started.exec, open->started.worktree));
    }

    const RepositoryConfig& repository = repositoryFor(config, repositoryName);
    const QString repositoryPath = expandPath(repository.path, paths.root);
    const QString remoteBase = baseRef(repository);
    gitRequired(repositoryPath, {QStringLiteral("fetch"), QStringLiteral("--quiet"), repository.remote});
    // ADR-014: a dirty main repo does not block start. The base resolves from
    // a remote ref, so there is no technical obstacle; the state is recorded
    // and warned about. The worktree's own dirt is captured into a preserved
    // ref -- otherwise it would be evidence invisible to the merge-base..HEAD
    // diff accounting.
    const bool repositoryDirty = !gitRequired(repositoryPath, {QStringLiteral("status"), QStringLiteral("--porcelain")}).isEmpty();
    QVector<Finding> warnings;
    if (repositoryDirty) {
        warnings.append(finding(QStringLiteral("git.dirty_workspace"), QStringLiteral("warning"), QStringLiteral("git"),
            QStringLiteral("Repository has uncommitted changes"),
            repository.name + QStringLiteral(" was dirty when this execution started"),
            QStringLiteral("Commit or stash before the next start if this was unintended.")));
    }
    const QString remoteBaseSha = gitRequired(repositoryPath, {QStringLiteral("rev-parse"), remoteBase});
    const QString worktreeRoot = expandPath(config.worktreeRoot, paths.root);
    const QString worktree = QDir(worktreeRoot).filePath(task);
    const QString executionId = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'")) + QLatin1Char('-') + task;
    if (QFileInfo::exists(QDir(paths.ledger).filePath(executionId + QStringLiteral(".jsonl")))) {
        fail(QStringLiteral("Refusing start: execution already exists"));
    }
    // An interrupted execution is continued: the new one takes over its branch
    // and worktree, recreating the worktree when it was removed meanwhile.
    if (open && !QFileInfo::exists(worktree)) {
        gitRequired(repositoryPath, {QStringLiteral("worktree"), QStringLiteral("add"), worktree, open->started.branch});
    }
    QString branch;
    QString baseSha;
    QString headSha;
    QString workspaceSource;
    QString preservedRef;
    if (QFileInfo::exists(worktree)) {
        if (gitCommonDir(worktree) != gitCommonDir(repositoryPath)) {
            fail(QStringLiteral("Refusing start: existing worktree belongs to another repository"));
        }
        for (const ExecutionStarted& other : ledger.started) {
            if (!open && other.worktree == worktree) {
                fail(QStringLiteral("Refusing start: worktree already belongs to execution %1").arg(other.exec));
            }
        }
        preservedRef = preserveWorktree(worktree, executionId);
        branch = gitRequired(worktree, {QStringLiteral("branch"), QStringLiteral("--show-current")});
        headSha = gitRequired(worktree, {QStringLiteral("rev-parse"), QStringLiteral("HEAD")});
        baseSha = gitRequired(worktree, {QStringLiteral("merge-base"), QStringLiteral("HEAD"), remoteBase});
        workspaceSource = QStringLiteral("adopted");
    } else {
        if (!QDir().mkpath(worktreeRoot)) {
            fail(QStringLiteral("Cannot create worktree root: %1").arg(worktreeRoot));
        }
        branch = QStringLiteral("task/") + task;
        gitRequired(repositoryPath, {QStringLiteral("worktree"), QStringLiteral("add"), QStringLiteral("-b"), branch, worktree, remoteBase});
        baseSha = remoteBaseSha;
        headSha = baseSha;
        workspaceSource = QStringLiteral("created");
    }

    const QVector<Instruction> recordedInstructions = observeInstructions(config.instructions + instructions, paths.root);
    for (const Instruction& instruction : recordedInstructions) {
        if (instruction.sha1.isEmpty()) {
            warnings.append(finding(QStringLiteral("context.instruction_unreadable"), QStringLiteral("warning"), QStringLiteral("context"),
                QStringLiteral("Instruction file cannot be read"), instruction.path));
        }
    }

    ExecutionStarted started;
    started.exec = executionId;
    started.at = QDateTime::currentDateTimeUtc();
    started.task = task;
    started.agent = agent;
    started.repo = repository.name;
    started.worktree = worktree;
    started.branch = branch;
    started.base = remoteBase;
    started.workspaceSource = workspaceSource;
    started.repoDirty = repositoryDirty;
    started.preservedRef = preservedRef;
    started.baseSha = baseSha;
    started.headSha = headSha;
    started.remoteBaseSha = remoteBaseSha;
    // Which runtime transcript finish reads for test runs (DATA_MODEL.md §3.3).
    started.sessionId = sessionIdFromEnvironment(agent);
    started.instructions = recordedInstructions;
    started.planRef = planReference(config, paths.root, task);
    // The fingerprint of the file the task lives in, not of every plan: a line
    // added to another tool's plan must not flag this execution (eng review D6).
    started.planSha1 = observePlan(config, paths.root).sha1For(started.planRef);
    appendEvent(paths, started);
    return {executionId, worktree, branch, workspaceSource, baseSha, preservedRef, warnings};
}

FinishResult finishExecution(const QString& configPath, const QString& executionId, const QString& outcome)
{
    if (outcome != QLatin1String("finished") && outcome != QLatin1String("interrupted") && outcome != QLatin1String("abandoned")) {
        fail(QStringLiteral("Outcome must be finished, interrupted, or abandoned"));
    }
    const Paths paths = pathsFor(configPath);
    const ProjectConfig config = loadProjectConfig(configPath);
    prepareState(paths);
    Ledger ledger = readLedger(paths);
    const ExecutionStarted started = startedEvent(ledger, executionId);
    for (const ExecutionFinished& finished : ledger.finished) {
        if (finished.exec == executionId) fail(QStringLiteral("Execution already finished: %1").arg(executionId));
    }

    HandoffInput input;
    input.executionId = executionId;
    input.outcome = outcome;
    input.started = started;
    input.worktree = started.worktree;
    input.baseSha = started.baseSha;
    input.preservedRef = started.preservedRef;

    const QString finishPreservedRef = preserveWorktree(input.worktree, executionId, input.preservedRef);
    if (!finishPreservedRef.isEmpty()) input.preservedRef = finishPreservedRef;

    input.headSha = gitRequired(input.worktree, {QStringLiteral("rev-parse"), QStringLiteral("HEAD")});
    const QString range = input.baseSha + QStringLiteral("..HEAD");
    input.commitLines = gitRequired(input.worktree, {QStringLiteral("log"), QStringLiteral("--format=%h%x09%s"), range}).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    input.files = gitRequired(input.worktree, {QStringLiteral("diff"), QStringLiteral("--name-only"), range}).split(QLatin1Char('\n'), Qt::SkipEmptyParts);

    const QString shortstat = gitRequired(input.worktree, {QStringLiteral("diff"), QStringLiteral("--shortstat"), range});
    const QRegularExpression shortstatPattern(QStringLiteral("^(\\d+) files? changed(?:, (\\d+) insertions?\\(\\+\\))?(?:, (\\d+) deletions?\\(-\\))?$"));
    const QRegularExpressionMatch shortstatMatch = shortstatPattern.match(shortstat);
    if (!shortstat.isEmpty() && !shortstatMatch.hasMatch()) {
        fail(QStringLiteral("Unexpected git diff --shortstat output: %1").arg(shortstat));
    }
    if (shortstatMatch.hasMatch()) {
        input.filesChanged = shortstatMatch.captured(1).toInt();
        input.insertions = shortstatMatch.captured(2).toInt();
        input.deletions = shortstatMatch.captured(3).toInt();
    }

    QStringList commits;
    for (const QString& commit : input.commitLines) commits.append(commit.section(QLatin1Char('\t'), 0, 0));
    input.filesRef = writeEvidence(paths, {{QStringLiteral("files"), QJsonArray::fromStringList(input.files)}});

    const TranscriptRecord transcript = recordRuntimeTests(config, paths, started, ledger);

    writeHandoff(paths, input, ledger);
    ResumeFacts handoffFacts;
    handoffFacts.exec = executionId;
    readHandoff(paths, handoffFacts);
    if (handoffFacts.handoff.sha1.isEmpty()) fail(QStringLiteral("Cannot hash generated handoff: ") + handoffFacts.handoff.error);

    ExecutionFinished finished;
    finished.exec = executionId;
    finished.at = QDateTime::currentDateTimeUtc();
    finished.outcome = outcome;
    finished.headSha = input.headSha;
    finished.commits = commits;
    finished.filesChanged = input.filesChanged;
    finished.insertions = input.insertions;
    finished.deletions = input.deletions;
    finished.filesRef = input.filesRef;
    finished.preservedRef = input.preservedRef;
    finished.handoffSha1 = handoffFacts.handoff.sha1;
    finished.transcript = transcript;
    appendEvent(paths, finished);
    return {executionId, outcome, input.headSha, input.preservedRef,
        QStringLiteral("handoffs/") + executionId + QStringLiteral(".md"),
        observeWorktreeCleanup(input.worktree, started.branch, started.base)};
}

void recordEvidence(const QString& configPath, const QString& executionId, const QString& kind, const QString& summary, const QString& reference)
{
    const QStringList kinds{QStringLiteral("commit"), QStringLiteral("diff"), QStringLiteral("test"), QStringLiteral("files"), QStringLiteral("command"), QStringLiteral("agent_summary"), QStringLiteral("manual_note")};
    if (!kinds.contains(kind) || summary.trimmed().isEmpty()) fail(QStringLiteral("Invalid evidence kind or empty summary"));
    const Paths paths = pathsFor(configPath);
    prepareState(paths);
    EvidenceRecorded evidence;
    evidence.exec = executionId;
    evidence.at = QDateTime::currentDateTimeUtc();
    evidence.task = startedEvent(readLedger(paths), executionId).task;
    evidence.kind = kind;
    evidence.ref = reference;
    evidence.summary = summary;
    appendEvent(paths, evidence);
}

void recordNote(const QString& configPath, const QString& executionId, const QString& kind, const QString& text, const QString& reference)
{
    const QStringList kinds{QStringLiteral("decision"), QStringLiteral("unresolved"), QStringLiteral("blocker")};
    if (!kinds.contains(kind) || text.trimmed().isEmpty()) fail(QStringLiteral("Invalid note kind or empty text"));
    const Paths paths = pathsFor(configPath);
    prepareState(paths);
    const QString session = currentSession();
    if (executionId.isEmpty()) {
        if (session.isEmpty()) fail(QStringLiteral("rmk note needs an execution ID outside an agent session"));
    } else {
        startedEvent(readLedger(paths), executionId);
    }
    appendEvent(paths, NoteRecorded{executionId, QDateTime::currentDateTimeUtc(), kind, text, QStringLiteral("human"), reference, session});
}

bool isSessionId(const QString& id)
{
    return isSafeSessionId(id);
}

SessionStartResult sessionStarted(const QString& configPath, const HookInput& input)
{
    const Paths paths = pathsFor(configPath);
    // Once per session: keeps sessions/ out of `git status`, so recording a
    // session never shows up as a dirty workspace.
    prepareState(paths);
    SessionStartResult result;
    // Before recording this one, so it is never its own predecessor.
    const Ledger ledger = readLedger(paths);
    result.openWork = openExecutions(ledger);
    QDateTime newestNote;
    for (const SessionFacts& earlier : readSessions(paths, nullptr)) {
        if (earlier.id == input.sessionId) continue;
        const QVector<NoteRecorded> notes = notesOf(paths, earlier, &ledger);
        for (const NoteRecorded& note : notes) {
            if (!newestNote.isValid() || note.at > newestNote) {
                newestNote = note.at;
                result.lastWithNotes = earlier;
                result.lastNotes = notes;
            }
        }
        if (!earlier.endedAt || earlier.remindedHeads.isEmpty() || !notes.isEmpty()) continue;
        if (!result.previousWithoutNotes || *earlier.endedAt > *result.previousWithoutNotes->endedAt) {
            result.previousWithoutNotes = earlier;
        }
    }
    SessionFacts session;
    session.id = input.sessionId;
    session.runtime = runtimeOf(input.transcriptPath);
    session.cwd = input.cwd;
    session.transcriptPath = input.transcriptPath;
    session.source = input.source;
    session.startHead = headOf(input.cwd);
    recordSessionStarted(paths, session);
    return result;
}

void sessionWorking(const QString& configPath, const HookInput& input)
{
    recordSessionWorking(pathsFor(configPath), input.sessionId, headOf(input.cwd));
}

std::optional<QString> sessionStopped(const QString& configPath, const HookInput& input)
{
    const Paths paths = pathsFor(configPath);
    recordSessionWaiting(paths, input.sessionId);
    // Budget < 100 ms (eng review D9): local git and this project's files only,
    // no fetch, no transcript. Never block twice in a row (no loop).
    if (input.stopHookActive) return std::nullopt;
    const std::optional<SessionFacts> session = readSession(paths, input.sessionId);
    if (!session) return std::nullopt;

    // Where this session's commits land: its own directory and the worktrees
    // of executions it started, each against its baseline.
    QVector<QPair<QString, QString>> places{{session->cwd, session->turnHead.isEmpty() ? session->startHead : session->turnHead}};
    const Ledger ledger = readLedger(paths);
    for (const ExecutionStarted& started : ledger.started) {
        if (started.sessionId == session->id) places.append({started.worktree, started.headSha});
    }
    const QVector<NoteRecorded> notes = notesOf(paths, *session, &ledger);
    for (const auto& [place, baseline] : places) {
        const QString head = headOf(place);
        if (head.isEmpty() || baseline.isEmpty() || head == baseline || session->remindedHeads.contains(head)) continue;
        const QDateTime committed = QDateTime::fromString(
            git(place, {QStringLiteral("log"), QStringLiteral("-1"), QStringLiteral("--format=%cI"), head}).output.trimmed(), Qt::ISODate);
        bool noted = false;
        for (const NoteRecorded& note : notes) noted = noted || (committed.isValid() && note.at >= committed.addSecs(-1));
        if (noted) continue;
        recordSessionReminded(paths, session->id, head);
        return QStringLiteral("Runmark: commit %1 landed in this session and no decision or open item has been "
            "recorded since. Record what was decided with `rmk note --kind decision --text \"...\"` "
            "(or --kind unresolved / blocker), then stop.").arg(head.left(12));
    }
    return std::nullopt;
}

void sessionEnded(const QString& configPath, const HookInput& input)
{
    recordSessionEnded(pathsFor(configPath), input.sessionId, input.reason);
}

} // namespace runmark
