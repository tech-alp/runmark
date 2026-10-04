module;

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <optional>

export module runmark.domain;

export namespace runmark {

struct RepositoryConfig {
    QString name;
    QString path;
    QString remote;
    QString branch;
};

struct ProjectConfig {
    int version = 1;
    QString name;
    QString worktreeRoot;
    QVector<RepositoryConfig> repositories;
    // Plan files, in order; a file-name glob ("docs/superpowers/plans/*.md")
    // is allowed. Workflow tools all track progress with "- [ ]" (measured:
    // Superpowers, planning-with-files, GSD), so one reader serves them all.
    QStringList planPaths;
    QString taskIdPattern;
    bool hooksExpected = false;   // proje bir ajan hook'u bekliyor mu
    QStringList instructions;
    // Which transcript commands count as a test run (runtime evidence).
    QString testCommandPattern = defaultTestCommandPattern();

    // ponytail: a keyword heuristic over the command line; a project whose
    // runner is not listed sets project.test_command_pattern.
    static QString defaultTestCommandPattern()
    {
        return QStringLiteral("\\b(ctest|pytest|go test|cargo test|(npm|pnpm|yarn)( run)? test|make (test|check)|swift test|dotnet test)\\b");
    }

    // Pure validation. To read from disk, use loadProjectConfig in infrastructure.
    static ProjectConfig parse(const QJsonObject& root);
    QJsonObject toJson() const;
};

} // namespace runmark

// Plain data produced by the observation phase. No field here carries a
// judgement -- only what was measured. Rule evaluation (rules.h) reads these
// and nothing else: it never touches git or the filesystem.


export namespace runmark {

struct RepoFacts {
    QString name;
    QString path;
    QString base;              // remote/branch, for display only
    QString branch;
    QString head;
    QString baseSha;
    int behind = 0;
    int ahead = 0;
    bool dirty = false;
    QString localBase;         // name of the local base branch
    bool localBaseExists = false;
    int localBehind = 0;
    QString fetchError;        // non-empty means fetch failed: a diagnosis, not evidence
    QString measurementError;  // non-empty means measurement stopped halfway
    bool measured = false;
};

struct ExecutionFacts {
    QString exec;
    bool hasHandoff = false;
    bool worktreeExists = false;
    QString agent;
    // Timestamp of the newest event recorded for this execution. An execution
    // with no finish event is a status value; this is the second signal. A
    // session that has been silent for hours is not the same as one that wrote
    // a minute ago, and only the second one is worth staying away from.
    // Invalid when the ledger carried no usable timestamp.
    QDateTime lastActivity;
    // Whether the observed remote base is an ancestor of the current worktree HEAD.
    // Unknown when Git cannot inspect the worktree; never inferred from its start SHA.
    std::optional<bool> includesRemoteBase = std::nullopt;
    QString baseError = {};
};

// Ledger events, typed. Only infrastructure's ledger.cpp knows the JSONL keys
// (DATA_MODEL.md §3); everything above it reads these, so a schema change
// touches one file instead of every rule.
struct Instruction {
    QString name;
    QString path;
    QString sha1;              // empty: the file could not be read at start
};

struct ExecutionStarted {
    QString exec;
    QString ts;                // as written, for messages when `at` is invalid
    QDateTime at;
    QString task;
    QString agent;
    QString repo;
    QString worktree;
    QString branch;
    QString base;
    QString workspaceSource;   // created | adopted
    bool repoDirty = false;
    QString preservedRef;
    QString baseSha;
    QString headSha;
    QString remoteBaseSha;     // empty in records older than the field
    QString sessionId;
    // nullopt: a record older than the field, which proves nothing about
    // whether instructions existed.
    std::optional<QVector<Instruction>> instructions;
    QString planRef;
    QString planSha1;
};

// What finish could learn from the agent runtime's transcript (ADR-021).
struct TranscriptRecord {
    QString status;            // read | unavailable | no_session; empty in older records
    QString path;
    QString error;
    int commands = 0;
    int testRuns = 0;
};

struct ExecutionFinished {
    QString exec;
    QDateTime at;
    QString outcome;
    QString headSha;
    std::optional<QStringList> commits;
    std::optional<int> filesChanged;
    int insertions = 0;
    int deletions = 0;
    QString filesRef;
    QString preservedRef;
    QString handoffSha1;       // empty in records older than the field
    TranscriptRecord transcript;
};

struct EvidenceRecorded {
    QString exec;
    QDateTime at;
    QString task;
    QString kind;
    // runtime: read from the agent runtime's transcript by finish. Anything
    // else, including records older than the field, is the agent's claim.
    bool fromRuntime = false;
    QString runtime;
    std::optional<int> exitCode;
    QString ref;
    QString summary;
};

struct NoteRecorded {
    QString exec;
    QDateTime at;
    QString kind;              // decision | unresolved | blocker
    QString text;
    QString source;
    QString ref;
    // The agent session that wrote it, when rmk ran inside one. A note without
    // an execution lives in that session's own file.
    QString session;
};

// One agent session as its hooks reported it (.runmark/sessions/<id>.jsonl).
// A session exists whether or not anyone ran `rmk start` in it.
struct SessionFacts {
    QString id;
    QString runtime;           // claude | codex | unknown
    QString cwd;
    QString transcriptPath;
    QString source;            // startup | resume | ...
    QDateTime startedAt;
    QString startHead;         // HEAD of cwd when it started; empty outside git
    // HEAD of cwd when the latest prompt arrived; empty in older records.
    // Commits before it landed while this session waited, so they are
    // another session's in a shared checkout.
    QString turnHead;
    QDateTime lastWorkingAt;   // the user sent a prompt; the agent is working
    QDateTime lastWaitingAt;   // the agent finished a reply and waits for the user
    std::optional<QDateTime> endedAt;
    QString endReason;
    QStringList remindedHeads; // commits after which a note was already asked for
    QVector<NoteRecorded> notes;
    // Observed by status, not stored: tasks of executions this session
    // started, and files it changed against its baselines, keyed
    // "<git common dir>//<path>" so equal names in different repos differ.
    QStringList tasks;
    QStringList changedFiles;
};

struct Ledger {
    QVector<ExecutionStarted> started;
    QVector<ExecutionFinished> finished;
    QVector<EvidenceRecorded> evidence;
    QVector<NoteRecorded> notes;
    // "<file>: <type>" for lines of an unknown type. Skipping them silently
    // would make a newer or corrupted ledger read as a quieter one.
    QStringList unrecognised;
};

// Measured state of a finished execution's worktree. Runmark never removes a
// worktree on its own -- an uncommitted change or an unmerged branch would be
// gone with no record of it -- so it reports what it measured and leaves the
// decision outside.
struct WorktreeCleanupFacts {
    QString path;
    bool exists = false;
    bool clean = false;        // no uncommitted change
    bool merged = false;       // branch is an ancestor of the base
    QString error;             // non-empty when a check could not run at all
};

// One plan file's progress: "how many planned, how many done".
struct PlanFileFacts {
    QString path;              // as the task's plan_ref names it: relative to the project root when inside it
    int checklistCount = 0;
    int doneCount = 0;
    QString sha1;
};

struct PlanFacts {
    bool readable = false;     // at least one plan file could be read
    QVector<PlanFileFacts> files;
    QStringList unreadable;    // listed paths that could not be read, or globs that matched nothing
    int checklistCount = 0;    // number of "- [ ]" / "- [x]" lines
    int taskCount = 0;         // of those, the ones matching task_id_pattern
    QStringList doneTasks;     // task IDs marked "- [x]", in file order
    // Checklist lines where the pattern matched only part of a longer
    // identifier, e.g. "SCMS-42" inside "SCMS-42-W1". Guessing which task
    // they mean would silently bind evidence to the wrong one.
    QStringList ambiguousTasks;

    // Fingerprint of the file a plan_ref ("<path>#L<n>") points into; empty
    // when that file is not among the plan files.
    QString sha1For(const QString& planRef) const
    {
        const QString path = planRef.section(QLatin1Char('#'), 0, 0);
        for (const PlanFileFacts& file : files) {
            if (file.path == path) return file.sha1;
        }
        return {};
    }
};

struct StatusFacts {
    // "Now" is an observation too. Carrying it as a fact keeps the rules pure
    // and lets the 24-hour threshold be tested without rewinding ledger dates.
    QDateTime now;
    QVector<RepoFacts> repos;
    Ledger ledger;
    QVector<ExecutionFacts> executions;
    PlanFacts plan;
    // Sessions the agent hooks recorded. None at all, with hooks expected, is
    // indistinguishable from a clean project, so it is a finding.
    QVector<SessionFacts> sessions;
    QString sessionsError;     // non-empty: the session directory could not be read
    // Recent sessions in this project known only from their transcripts: no
    // hook recorded them (plugin missing, Codex hooks not approved). Hooks
    // cannot report their own absence; transcripts are written regardless.
    QVector<SessionFacts> unregisteredSessions;
};

struct FileFacts {
    QString path;
    std::optional<bool> exists;
    QString sha1;
    QString error;
};

// Commits and files of the selected execution: from its finish event, or
// measured live from its worktree while it is still running.
struct MeasuredWork {
    QString source;            // execution.finished | git; empty when unmeasured
    QString headSha;           // git only
    std::optional<QStringList> commits;
    std::optional<int> filesChanged;
};

struct ResumeFacts {
    QString task;
    QString exec;
    // Newest event timestamp for this execution; the second signal a reader
    // needs before assuming an unfinished execution is nobody's.
    QDateTime lastActivity;
    QString ledgerError;
    // The selected execution's events; `exec` may be known from an evidence
    // or note event while `started` is missing.
    std::optional<ExecutionStarted> started;
    std::optional<ExecutionFinished> finished;
    QVector<EvidenceRecorded> evidence;
    QVector<NoteRecorded> notes;
    FileFacts handoff;
    QString handoffContent;
    FileFacts worktree;
    QString planSha1;
    QString currentBaseSha;
    std::optional<bool> baseAdvanced;
    QString baseError;
    QString fetchError;
    MeasuredWork measured;
    QString measurementError;
};

} // namespace runmark

// The product of evaluation. Derived from measured facts and carrying a
// judgement -- which is why it does not live in facts.h. Turning it into JSON
// is the CLI's job (TC-007), so there is no serialisation here.


export namespace runmark {

struct Finding {
    QString id;
    QString severity;          // info | warning | blocking
    QString domain;            // git | plan | context
    QString title;
    QString explanation;
    QString suggestedAction;   // may be empty
    // The same action as a program and its arguments, when it is one command
    // that resolves the finding; empty when the action is advice. A client
    // runs it without a shell, so a path is never split or interpreted.
    QStringList command;
};

} // namespace runmark

export namespace runmark {

// Produces findings. PURE: reads no filesystem, no git, no clock.
// The same facts always yield the same findings, so a test is a plain call.
QVector<Finding> evaluate(const ProjectConfig& config, const StatusFacts& facts);
QVector<Finding> evaluateResume(const ResumeFacts& facts);

// Work still waiting for someone: the newest execution of each task, when it
// never finished or finished as interrupted. A later execution of the task
// takes over, so an older one never shows. Oldest first.
struct OpenExecution {
    ExecutionStarted started;
    QString outcome;           // interrupted; empty when it never finished
    QDateTime lastActivity;
};
QVector<OpenExecution> openExecutions(const Ledger& ledger);

// Whether a shell command's exit status is the test run's own. After
// `ctest | tail`, `ctest; echo` or `ctest || true` the status belongs to a
// later command, so a failing suite reads as exit 0. `&&` keeps a failure, and
// so does a pipe once `pipefail` is set.
// ponytail: scans characters, not shell grammar; a separator inside quotes
// after the test counts too, which errs towards "unknown".
bool exitCodeCoversTestRun(const QString& command, const QString& testPattern);

// One place owns the finding shape (TRUST_MODEL.md); start warnings use it too.
Finding finding(const QString& id, const QString& severity, const QString& domain,
                const QString& title, const QString& explanation, const QString& action = {});

} // namespace runmark
