#include "ledger.h"

#include "runmark/error.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSet>

namespace runmark {
namespace {

QJsonValue orNull(const QString& value)
{
    return value.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(value);
}

QString timestamp(const QDateTime& at)
{
    return at.toUTC().toString(Qt::ISODate);
}

QDateTime parseTime(const QJsonObject& object)
{
    return QDateTime::fromString(object.value(QStringLiteral("ts")).toString(), Qt::ISODate);
}

QString text(const QJsonObject& object, const char* key)
{
    return object.value(QLatin1String(key)).toString();
}

void append(const Paths& paths, const QString& executionId, const QJsonObject& event)
{
    QFile file(QDir(paths.ledger).filePath(executionId + QStringLiteral(".jsonl")));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        fail(QStringLiteral("Cannot append ledger %1: %2").arg(file.fileName(), file.errorString()));
    }
    file.write(QJsonDocument(event).toJson(QJsonDocument::Compact));
    file.write("\n");
}

ExecutionStarted parseStarted(const QJsonObject& o)
{
    ExecutionStarted e;
    e.exec = text(o, "exec");
    e.ts = text(o, "ts");
    e.at = parseTime(o);
    e.task = text(o, "task");
    e.agent = text(o, "agent");
    e.repo = text(o, "repo");
    e.worktree = text(o, "worktree");
    e.branch = text(o, "branch");
    e.base = text(o, "base");
    e.workspaceSource = text(o, "workspace_source");
    e.repoDirty = o.value(QStringLiteral("repo_dirty")).toBool();
    e.preservedRef = text(o, "preserved_ref");
    e.baseSha = text(o, "base_sha");
    e.headSha = text(o, "head_sha");
    e.remoteBaseSha = text(o, "remote_base_sha");
    e.sessionId = text(o, "session_id");
    const QJsonValue instructions = o.value(QStringLiteral("instructions"));
    if (instructions.isArray()) {
        QVector<Instruction> list;
        for (const QJsonValue& value : instructions.toArray()) {
            const QJsonObject item = value.toObject();
            list.append({text(item, "name"), text(item, "path"), text(item, "sha1")});
        }
        e.instructions = list;
    }
    e.planRef = text(o, "plan_ref");
    e.planSha1 = text(o, "plan_sha1");
    return e;
}

ExecutionFinished parseFinished(const QJsonObject& o)
{
    ExecutionFinished e;
    e.exec = text(o, "exec");
    e.at = parseTime(o);
    e.outcome = text(o, "outcome");
    e.headSha = text(o, "head_sha");
    const QJsonValue commits = o.value(QStringLiteral("commits"));
    if (commits.isArray()) {
        QStringList list;
        for (const QJsonValue& value : commits.toArray()) list.append(value.toString());
        e.commits = list;
    }
    if (o.value(QStringLiteral("files_changed")).isDouble()) e.filesChanged = o.value(QStringLiteral("files_changed")).toInt();
    e.insertions = o.value(QStringLiteral("insertions")).toInt();
    e.deletions = o.value(QStringLiteral("deletions")).toInt();
    e.filesRef = text(o, "files_ref");
    e.preservedRef = text(o, "preserved_ref");
    e.handoffSha1 = text(o, "handoff_sha1");
    const QJsonObject transcript = o.value(QStringLiteral("transcript")).toObject();
    e.transcript = {text(transcript, "status"), text(transcript, "path"), text(transcript, "error"),
        transcript.value(QStringLiteral("commands")).toInt(), transcript.value(QStringLiteral("test_runs")).toInt()};
    return e;
}

EvidenceRecorded parseEvidence(const QJsonObject& o)
{
    EvidenceRecorded e;
    e.exec = text(o, "exec");
    e.at = parseTime(o);
    e.task = text(o, "task");
    e.kind = text(o, "kind");
    e.fromRuntime = text(o, "source") == QLatin1String("runtime");
    e.runtime = text(o, "runtime");
    if (o.value(QStringLiteral("exit_code")).isDouble()) e.exitCode = o.value(QStringLiteral("exit_code")).toInt();
    e.ref = text(o, "ref");
    e.summary = text(o, "summary");
    return e;
}

NoteRecorded parseNote(const QJsonObject& o)
{
    return {text(o, "exec"), parseTime(o), text(o, "kind"), text(o, "text"), text(o, "source"), text(o, "ref"), text(o, "session")};
}

QString sessionFile(const Paths& paths, const QString& id)
{
    if (!isSafeSessionId(id)) fail(QStringLiteral("Invalid session id: %1").arg(id));
    return QDir(paths.sessions).filePath(id + QStringLiteral(".jsonl"));
}

void appendTo(const QString& path, const QJsonObject& event)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) fail(QStringLiteral("Cannot create %1").arg(QFileInfo(path).absolutePath()));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        fail(QStringLiteral("Cannot append %1: %2").arg(file.fileName(), file.errorString()));
    }
    file.write(QJsonDocument(event).toJson(QJsonDocument::Compact));
    file.write("\n");
}

void sessionEvent(const Paths& paths, const QString& id, const QString& type, QJsonObject event = {})
{
    // Milliseconds: a prompt and a reply can land in the same second.
    event.insert(QStringLiteral("ts"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    event.insert(QStringLiteral("type"), type);
    event.insert(QStringLiteral("session"), id);
    appendTo(sessionFile(paths, id), event);
}

// ponytail: lines of an unknown type in a session file are skipped; session
// files are written only by rmk hook, unlike the shared execution ledger.
SessionFacts parseSession(const QString& path)
{
    SessionFacts session;
    session.id = QFileInfo(path).completeBaseName();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) fail(QStringLiteral("Cannot read session %1").arg(path));
    while (!file.atEnd()) {
        const QJsonObject o = QJsonDocument::fromJson(file.readLine().trimmed()).object();
        const QString type = text(o, "type");
        if (type == QLatin1String("session.started")) {
            // A resumed session starts again; its baseline stays the first one.
            if (!session.startedAt.isValid()) {
                session.startHead = text(o, "head");
                session.startedAt = parseTime(o);
            }
            session.runtime = text(o, "runtime");
            session.cwd = text(o, "cwd");
            session.transcriptPath = text(o, "transcript");
            session.source = text(o, "source");
        } else if (type == QLatin1String("session.working")) {
            session.lastWorkingAt = parseTime(o);
            session.turnHead = text(o, "head");
        } else if (type == QLatin1String("session.waiting")) {
            session.lastWaitingAt = parseTime(o);
        } else if (type == QLatin1String("session.reminded")) {
            session.remindedHeads.append(text(o, "head"));
        } else if (type == QLatin1String("session.ended")) {
            session.endedAt = parseTime(o);
            session.endReason = text(o, "reason");
        } else if (type == QLatin1String("note")) {
            session.notes.append(parseNote(o));
        }
    }
    return session;
}

void parseInto(Ledger& ledger, const QJsonObject& event, const QString& file)
{
    const QString type = text(event, "type");
    if (type == QLatin1String("execution.started")) ledger.started.append(parseStarted(event));
    else if (type == QLatin1String("execution.finished")) ledger.finished.append(parseFinished(event));
    else if (type == QLatin1String("evidence.recorded")) ledger.evidence.append(parseEvidence(event));
    else if (type == QLatin1String("note")) ledger.notes.append(parseNote(event));
    else ledger.unrecognised.append(file + QStringLiteral(": ") + (type.isEmpty() ? QStringLiteral("<no type>") : type));
}

} // namespace

void appendEvent(const Paths& paths, const ExecutionStarted& e)
{
    QJsonArray instructions;
    for (const Instruction& instruction : e.instructions.value_or(QVector<Instruction>{})) {
        instructions.append(QJsonObject{{QStringLiteral("name"), instruction.name}, {QStringLiteral("path"), instruction.path},
            {QStringLiteral("sha1"), orNull(instruction.sha1)}});
    }
    append(paths, e.exec, {
        {QStringLiteral("ts"), timestamp(e.at)}, {QStringLiteral("type"), QStringLiteral("execution.started")}, {QStringLiteral("exec"), e.exec},
        {QStringLiteral("task"), e.task}, {QStringLiteral("agent"), e.agent}, {QStringLiteral("repo"), e.repo},
        {QStringLiteral("worktree"), e.worktree}, {QStringLiteral("branch"), e.branch}, {QStringLiteral("base"), e.base},
        {QStringLiteral("workspace_source"), e.workspaceSource}, {QStringLiteral("repo_dirty"), e.repoDirty},
        {QStringLiteral("preserved_ref"), orNull(e.preservedRef)},
        {QStringLiteral("base_sha"), e.baseSha}, {QStringLiteral("head_sha"), e.headSha},
        {QStringLiteral("remote_base_sha"), e.remoteBaseSha},
        {QStringLiteral("session_id"), orNull(e.sessionId)},
        {QStringLiteral("instructions"), instructions},
        {QStringLiteral("plan_ref"), e.planRef},
        {QStringLiteral("plan_sha1"), e.planSha1},
    });
}

void appendEvent(const Paths& paths, const ExecutionFinished& e)
{
    QJsonObject transcript{{QStringLiteral("status"), e.transcript.status}};
    if (e.transcript.status == QLatin1String("read")) {
        transcript.insert(QStringLiteral("path"), e.transcript.path);
        transcript.insert(QStringLiteral("commands"), e.transcript.commands);
        transcript.insert(QStringLiteral("test_runs"), e.transcript.testRuns);
    }
    if (!e.transcript.error.isEmpty()) transcript.insert(QStringLiteral("error"), e.transcript.error);
    append(paths, e.exec, {{QStringLiteral("ts"), timestamp(e.at)}, {QStringLiteral("type"), QStringLiteral("execution.finished")},
        {QStringLiteral("exec"), e.exec}, {QStringLiteral("outcome"), e.outcome}, {QStringLiteral("head_sha"), e.headSha},
        {QStringLiteral("commits"), QJsonArray::fromStringList(e.commits.value_or(QStringList{}))},
        {QStringLiteral("files_changed"), e.filesChanged.value_or(0)},
        {QStringLiteral("insertions"), e.insertions}, {QStringLiteral("deletions"), e.deletions},
        {QStringLiteral("files_ref"), e.filesRef}, {QStringLiteral("preserved_ref"), orNull(e.preservedRef)},
        {QStringLiteral("handoff_sha1"), e.handoffSha1}, {QStringLiteral("transcript"), transcript}});
}

void appendEvent(const Paths& paths, const EvidenceRecorded& e)
{
    QJsonObject event{{QStringLiteral("ts"), timestamp(e.at)}, {QStringLiteral("type"), QStringLiteral("evidence.recorded")},
        {QStringLiteral("exec"), e.exec}, {QStringLiteral("task"), e.task}, {QStringLiteral("kind"), e.kind},
        {QStringLiteral("source"), e.fromRuntime ? QStringLiteral("runtime") : QStringLiteral("agent")},
        {QStringLiteral("ref"), orNull(e.ref)}, {QStringLiteral("summary"), e.summary}};
    if (e.fromRuntime) {
        event.insert(QStringLiteral("runtime"), e.runtime);
        event.insert(QStringLiteral("exit_code"), e.exitCode ? QJsonValue(*e.exitCode) : QJsonValue::Null);
    }
    append(paths, e.exec, event);
}

void appendEvent(const Paths& paths, const NoteRecorded& e)
{
    const QJsonObject event{{QStringLiteral("ts"), timestamp(e.at)}, {QStringLiteral("type"), QStringLiteral("note")},
        {QStringLiteral("exec"), orNull(e.exec)}, {QStringLiteral("kind"), e.kind}, {QStringLiteral("text"), e.text},
        {QStringLiteral("source"), e.source}, {QStringLiteral("ref"), orNull(e.ref)}, {QStringLiteral("session"), orNull(e.session)}};
    if (e.exec.isEmpty()) appendTo(sessionFile(paths, e.session), event);
    else append(paths, e.exec, event);
}

bool isSafeSessionId(const QString& id)
{
    static const QRegularExpression safe(QStringLiteral("^[A-Za-z0-9-]+$"));
    return safe.match(id).hasMatch();
}

void recordSessionStarted(const Paths& paths, const SessionFacts& session)
{
    sessionEvent(paths, session.id, QStringLiteral("session.started"), {
        {QStringLiteral("runtime"), session.runtime}, {QStringLiteral("cwd"), session.cwd},
        {QStringLiteral("transcript"), orNull(session.transcriptPath)}, {QStringLiteral("source"), orNull(session.source)},
        {QStringLiteral("head"), orNull(session.startHead)}});
}

void recordSessionWorking(const Paths& paths, const QString& id, const QString& head)
{
    sessionEvent(paths, id, QStringLiteral("session.working"), {{QStringLiteral("head"), orNull(head)}});
}

void recordSessionWaiting(const Paths& paths, const QString& id)
{
    sessionEvent(paths, id, QStringLiteral("session.waiting"));
}

void recordSessionReminded(const Paths& paths, const QString& id, const QString& head)
{
    sessionEvent(paths, id, QStringLiteral("session.reminded"), {{QStringLiteral("head"), head}});
}

void recordSessionEnded(const Paths& paths, const QString& id, const QString& reason)
{
    sessionEvent(paths, id, QStringLiteral("session.ended"), {{QStringLiteral("reason"), orNull(reason)}});
}

std::optional<SessionFacts> readSession(const Paths& paths, const QString& id)
{
    const QString path = sessionFile(paths, id);
    if (!QFileInfo::exists(path)) return std::nullopt;
    return parseSession(path);
}

QVector<SessionFacts> readSessions(const Paths& paths, QString* error)
{
    QVector<SessionFacts> sessions;
    const QDir directory(paths.sessions);
    if (!directory.exists()) return sessions;
    try {
        for (const QString& name : directory.entryList({QStringLiteral("*.jsonl")}, QDir::Files, QDir::Name)) {
            sessions.append(parseSession(directory.filePath(name)));
        }
    } catch (const std::exception& failure) {
        if (error) *error = QString::fromUtf8(failure.what());
    }
    return sessions;
}

Ledger readLedger(const Paths& paths)
{
    Ledger ledger;
    const QStringList files = QDir(paths.ledger).entryList({QStringLiteral("*.jsonl")}, QDir::Files, QDir::Name);
    for (const QString& name : files) {
        QFile file(QDir(paths.ledger).filePath(name));
        if (!file.open(QIODevice::ReadOnly)) {
            fail(QStringLiteral("Cannot read ledger %1").arg(file.fileName()));
        }
        while (!file.atEnd()) {
            const QByteArray line = file.readLine().trimmed();
            if (line.isEmpty()) {
                continue;
            }
            QJsonParseError error;
            const QJsonDocument document = QJsonDocument::fromJson(line, &error);
            if (error.error != QJsonParseError::NoError || !document.isObject()) {
                fail(QStringLiteral("Invalid ledger event in %1").arg(file.fileName()));
            }
            parseInto(ledger, document.object(), name);
        }
    }
    return ledger;
}

const ExecutionStarted& startedEvent(const Ledger& ledger, const QString& executionId)
{
    for (const ExecutionStarted& event : ledger.started) {
        if (event.exec == executionId) return event;
    }
    fail(QStringLiteral("Unknown execution: %1").arg(executionId));
}

ResumeFacts observeResumeLedger(const Paths& paths, const QString& taskOrExecution)
{
    ResumeFacts facts;
    facts.task = taskOrExecution;
    Ledger ledger;
    try {
        const FileFacts directory = observePath(paths.ledger);
        if (!directory.exists.has_value() || (directory.exists == true
                && (!QFileInfo(paths.ledger).isDir() || !QFileInfo(paths.ledger).isReadable()
                    || !QFileInfo(paths.ledger).isExecutable()))) {
            fail(QStringLiteral("Cannot list ledger directory: ") + paths.ledger);
        }
        ledger = readLedger(paths);
    } catch (const std::exception& error) {
        facts.ledgerError = QString::fromUtf8(error.what());
        return facts;
    }
    // An execution ID selects itself, even when only its evidence or notes
    // survived. Otherwise IDs carry UTC time and lexical order is the ledger's
    // documented chronology. An empty selector means "the latest execution":
    // for a caller that does not know which task it is on at session start,
    // that is the only meaningful default.
    QSet<QString> known;
    for (const ExecutionStarted& e : ledger.started) known.insert(e.exec);
    for (const ExecutionFinished& e : ledger.finished) known.insert(e.exec);
    for (const EvidenceRecorded& e : ledger.evidence) known.insert(e.exec);
    for (const NoteRecorded& e : ledger.notes) known.insert(e.exec);
    if (!taskOrExecution.isEmpty() && known.contains(taskOrExecution)) {
        facts.exec = taskOrExecution;
    } else {
        for (const ExecutionStarted& e : ledger.started) {
            if ((taskOrExecution.isEmpty() || e.task == taskOrExecution) && e.exec > facts.exec) facts.exec = e.exec;
        }
    }
    if (facts.exec.isEmpty()) return facts;

    const auto touch = [&facts](const QDateTime& at) {
        if (at.isValid() && (!facts.lastActivity.isValid() || at > facts.lastActivity)) facts.lastActivity = at;
    };
    for (const ExecutionStarted& e : ledger.started) {
        if (e.exec == facts.exec) { facts.started = e; touch(e.at); }
    }
    for (const ExecutionFinished& e : ledger.finished) {
        if (e.exec == facts.exec) { facts.finished = e; touch(e.at); }
    }
    for (const EvidenceRecorded& e : ledger.evidence) {
        if (e.exec == facts.exec) { facts.evidence.append(e); touch(e.at); }
    }
    for (const NoteRecorded& e : ledger.notes) {
        if (e.exec == facts.exec) { facts.notes.append(e); touch(e.at); }
    }
    if (facts.started) facts.task = facts.started->task;
    return facts;
}

QString writeEvidence(const Paths& paths, const QJsonObject& value)
{
    const QByteArray data = QJsonDocument(value).toJson(QJsonDocument::Compact);
    const QString name = QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha1).toHex()) + QStringLiteral(".json");
    QFile file(QDir(paths.evidence).filePath(name));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail(QStringLiteral("Cannot write evidence %1").arg(file.fileName()));
    }
    file.write(data);
    return QStringLiteral("evidence/") + name;
}

} // namespace runmark
