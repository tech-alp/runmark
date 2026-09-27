// Pure presentation: typed result -> JSON / markdown. Reads no filesystem,
// no git, no clock; its input is already-measured facts. The DISK side of a
// handoff stays in infrastructure (handoff.cpp), because that one writes.
#include "json.h"

#include <QDateTime>
#include <QJsonArray>
#include <QTextStream>

namespace runmark {

static QJsonObject resumePackage(const ResumeFacts& facts, const QJsonArray& gaps);

QJsonObject toJson(const Finding& finding)
{
    QJsonObject value{
        {QStringLiteral("id"), finding.id},
        {QStringLiteral("severity"), finding.severity},
        {QStringLiteral("domain"), finding.domain},
        {QStringLiteral("title"), finding.title},
        {QStringLiteral("explanation"), finding.explanation},
    };
    if (!finding.suggestedAction.isEmpty()) {
        value.insert(QStringLiteral("suggested_action"), finding.suggestedAction);
    }
    return value;
}

QJsonObject toJson(const RepoFacts& facts)
{
    QJsonObject report{
        {QStringLiteral("name"), facts.name},
        {QStringLiteral("path"), facts.path},
        {QStringLiteral("base"), facts.base},
    };
    if (!facts.fetchError.isEmpty()) {
        report.insert(QStringLiteral("fetch_error"), facts.fetchError);
    }
    if (facts.measured) {
        report.insert(QStringLiteral("branch"), facts.branch);
        report.insert(QStringLiteral("head"), facts.head);
        report.insert(QStringLiteral("base_sha"), facts.baseSha);
        report.insert(QStringLiteral("behind_base"), facts.behind);
        report.insert(QStringLiteral("ahead_of_base"), facts.ahead);
        report.insert(QStringLiteral("dirty"), facts.dirty);
    } else {
        report.insert(QStringLiteral("error"), facts.measurementError);
    }
    return report;
}

namespace {

QJsonArray toJsonArray(const QVector<Finding>& findings)
{
    QJsonArray array;
    for (const Finding& finding : findings) array.append(toJson(finding));
    return array;
}

QJsonValue orNull(const QString& value)
{
    return value.isEmpty() ? QJsonValue::Null : QJsonValue(value);
}

} // namespace

QJsonObject toJson(const StatusResult& result)
{
    QJsonArray repositories;
    for (const RepoFacts& repository : result.repositories) repositories.append(toJson(repository));
    QJsonArray sessions;
    for (const SessionFacts& session : result.sessions) sessions.append(toJson(session));
    return {{QStringLiteral("project"), result.project},
            {QStringLiteral("repositories"), repositories},
            {QStringLiteral("findings"), toJsonArray(result.findings)},
            {QStringLiteral("sessions"), sessions},
            {QStringLiteral("plans"), [&] {
                QJsonArray plans;
                for (const PlanFileFacts& file : result.plan.files) {
                    plans.append(QJsonObject{{QStringLiteral("path"), file.path},
                        {QStringLiteral("done"), file.doneCount}, {QStringLiteral("total"), file.checklistCount}});
                }
                return plans;
            }()},
            {QStringLiteral("open_work"), [&] {
                QJsonArray open;
                for (const OpenExecution& execution : result.openWork) {
                    open.append(QJsonObject{{QStringLiteral("task"), execution.started.task},
                        {QStringLiteral("exec"), execution.started.exec},
                        {QStringLiteral("outcome"), execution.outcome.isEmpty() ? QJsonValue::Null : QJsonValue(execution.outcome)},
                        {QStringLiteral("worktree"), execution.started.worktree},
                        {QStringLiteral("branch"), execution.started.branch},
                        {QStringLiteral("last_activity"), execution.lastActivity.isValid()
                            ? QJsonValue(execution.lastActivity.toString(Qt::ISODate)) : QJsonValue::Null}});
                }
                return open;
            }()}};
}

QString statusSummary(const StatusResult& result)
{
    int done = 0;
    int total = 0;
    for (const PlanFileFacts& file : result.plan.files) {
        done += file.doneCount;
        total += file.checklistCount;
    }
    // Plan findings audit past tasks and stay until someone backfills them; in
    // a sidebar that is a number nobody reads. Only git and context warnings
    // ask for attention now.
    int warnings = 0;
    for (const Finding& finding : result.findings) {
        if (finding.severity != QLatin1String("info") && finding.domain != QLatin1String("plan")) ++warnings;
    }
    QStringList parts;
    if (total > 0) parts << QStringLiteral("%1/%2").arg(done).arg(total);
    if (!result.openWork.isEmpty()) parts << QStringLiteral("%1 open").arg(result.openWork.size());
    if (warnings > 0) parts << QStringLiteral("%1 warn").arg(warnings);
    return parts.join(QStringLiteral(" · "));
}

QJsonObject toJson(const SessionFacts& session)
{
    const auto time = [](const QDateTime& at) { return at.isValid() ? QJsonValue(at.toString(Qt::ISODateWithMs)) : QJsonValue::Null; };
    // ended, else whichever of "user sent a prompt" / "agent finished a reply"
    // happened last; a session that has done neither yet is starting.
    const QString state = session.endedAt ? QStringLiteral("ended")
        : session.lastWaitingAt.isValid() && session.lastWaitingAt >= session.lastWorkingAt ? QStringLiteral("waiting")
        : session.lastWorkingAt.isValid() ? QStringLiteral("working") : QStringLiteral("started");
    return {{QStringLiteral("id"), session.id}, {QStringLiteral("runtime"), session.runtime},
        {QStringLiteral("cwd"), session.cwd}, {QStringLiteral("state"), state},
        {QStringLiteral("started_at"), time(session.startedAt)},
        {QStringLiteral("last_waiting_at"), time(session.lastWaitingAt)},
        {QStringLiteral("ended_at"), time(session.endedAt.value_or(QDateTime()))},
        {QStringLiteral("end_reason"), session.endReason.isEmpty() ? QJsonValue::Null : QJsonValue(session.endReason)},
        {QStringLiteral("notes"), session.notes.size()},
        {QStringLiteral("transcript"), session.transcriptPath.isEmpty() ? QJsonValue::Null : QJsonValue(session.transcriptPath)}};
}

QString sessionNotesMarkdown(const SessionFacts& session, const QVector<NoteRecorded>& notes)
{
    QString text = QStringLiteral("\n## Notes from the last session\n\nsession %1 (%2)\n\n").arg(session.id, session.runtime.isEmpty() ? QStringLiteral("unknown") : session.runtime);
    for (const NoteRecorded& note : notes) {
        text += QStringLiteral("- [%1] %2").arg(note.kind, note.text);
        if (!note.ref.isEmpty()) text += QStringLiteral("  (ref: %1)").arg(note.ref);
        text += QLatin1Char('\n');
    }
    return text;
}

QString openWorkMarkdown(const QVector<OpenExecution>& open)
{
    if (open.isEmpty()) return {};
    QString text = QStringLiteral("## Open work\n\nNot done yet; the resume context below covers only the latest execution.\n\n");
    for (const OpenExecution& execution : open) {
        const QString exec = execution.started.exec;
        const QString activity = execution.lastActivity.isValid() ? execution.lastActivity.toString(Qt::ISODate) : QStringLiteral("unknown");
        text += execution.outcome.isEmpty()
            ? QStringLiteral("- %1: %2 never finished, last activity %3. A recent one may be another live session's. "
                "Continue in %4 with `rmk resume %2`\n").arg(execution.started.task, exec, activity, execution.started.worktree)
            : QStringLiteral("- %1: %2 interrupted, last activity %3. Continue with `rmk start %1`\n").arg(execution.started.task, exec, activity);
    }
    return text;
}

QString sessionWithoutNotesMarkdown(const SessionFacts& session)
{
    return QStringLiteral("\n## Unrecorded decisions\n\nAn earlier %1 session (%2) committed work and ended without a "
        "decision or open-item note. What it decided exists only in its transcript: %3\n"
        "Read it if this work continues, and record what still holds with `rmk note`.\n")
        .arg(session.runtime, session.id, session.transcriptPath.isEmpty() ? QStringLiteral("(unknown)") : session.transcriptPath);
}

QJsonObject toJson(const StartResult& result)
{
    return {{QStringLiteral("exec"), result.exec}, {QStringLiteral("worktree"), result.worktree},
        {QStringLiteral("branch"), result.branch}, {QStringLiteral("workspace_source"), result.workspaceSource},
        {QStringLiteral("base_sha"), result.baseSha}, {QStringLiteral("preserved_ref"), orNull(result.preservedRef)},
        {QStringLiteral("warnings"), toJsonArray(result.warnings)}};
}

QJsonObject toJson(const WorktreeCleanupFacts& facts)
{
    QJsonObject value{{QStringLiteral("path"), orNull(facts.path)},
        {QStringLiteral("exists"), facts.exists}};
    if (!facts.error.isEmpty()) {
        // Unknown is not the same as unsafe, and neither is the same as safe.
        value.insert(QStringLiteral("error"), facts.error);
        return value;
    }
    if (!facts.exists) {
        return value;
    }
    value.insert(QStringLiteral("clean"), facts.clean);
    value.insert(QStringLiteral("merged"), facts.merged);
    // The command appears only when both checks passed. Suggesting removal of
    // a dirty or unmerged worktree would hand someone a way to lose work.
    if (facts.clean && facts.merged) {
        value.insert(QStringLiteral("suggested_action"),
            QStringLiteral("git worktree remove ") + facts.path);
    }
    return value;
}

QJsonObject toJson(const FinishResult& result)
{
    return {{QStringLiteral("exec"), result.exec}, {QStringLiteral("outcome"), result.outcome},
        {QStringLiteral("head_sha"), result.headSha}, {QStringLiteral("preserved_ref"), orNull(result.preservedRef)},
        {QStringLiteral("handoff"), result.handoff},
        {QStringLiteral("worktree"), toJson(result.worktree)}};
}

QJsonObject toJson(const ResumeResult& result)
{
    return resumePackage(result.facts, toJsonArray(result.gaps));
}

// The resume package quotes ledger events in their ledger shape, so a reader
// sees the same record the handoff was built from (DATA_MODEL.md §8).
static QJsonObject toJson(const EvidenceRecorded& event)
{
    QJsonObject value{{QStringLiteral("ts"), event.at.toString(Qt::ISODate)}, {QStringLiteral("type"), QStringLiteral("evidence.recorded")},
        {QStringLiteral("exec"), event.exec}, {QStringLiteral("task"), event.task}, {QStringLiteral("kind"), event.kind},
        {QStringLiteral("source"), event.fromRuntime ? QStringLiteral("runtime") : QStringLiteral("agent")},
        {QStringLiteral("ref"), event.ref.isEmpty() ? QJsonValue::Null : QJsonValue(event.ref)}, {QStringLiteral("summary"), event.summary}};
    if (event.fromRuntime) {
        value.insert(QStringLiteral("runtime"), event.runtime);
        value.insert(QStringLiteral("exit_code"), event.exitCode ? QJsonValue(*event.exitCode) : QJsonValue::Null);
    }
    return value;
}

static QJsonObject toJson(const NoteRecorded& note)
{
    return {{QStringLiteral("ts"), note.at.toString(Qt::ISODate)}, {QStringLiteral("type"), QStringLiteral("note")},
        {QStringLiteral("exec"), note.exec}, {QStringLiteral("kind"), note.kind}, {QStringLiteral("text"), note.text},
        {QStringLiteral("source"), note.source}, {QStringLiteral("ref"), note.ref.isEmpty() ? QJsonValue::Null : QJsonValue(note.ref)}};
}

static QJsonObject resumePackage(const ResumeFacts& facts, const QJsonArray& gaps)
{
    const auto nullable = [](const QString& value) -> QJsonValue {
        return value.isEmpty() ? QJsonValue::Null : QJsonValue(value);
    };
    const auto boolean = [](std::optional<bool> value) -> QJsonValue {
        return value.has_value() ? QJsonValue(*value) : QJsonValue::Null;
    };
    const ExecutionStarted started = facts.started.value_or(ExecutionStarted{});
    const QString& recordedPlan = started.planSha1;
    const QString recordedHandoff = facts.finished ? facts.finished->handoffSha1 : QString();
    QJsonArray evidence, claims, withRef, withoutRef;
    for (const EvidenceRecorded& event : facts.evidence) {
        // Only what the runtime recorded is measured; the same kind written
        // by the agent through `rmk evidence` stays a claim.
        if (event.fromRuntime) evidence.append(toJson(event));
        else if (event.kind == QLatin1String("test") || event.kind == QLatin1String("command")
                || event.kind == QLatin1String("agent_summary")) claims.append(toJson(event));
    }
    for (const NoteRecorded& note : facts.notes) {
        if (note.kind == QLatin1String("unresolved")) (note.ref.isEmpty() ? withoutRef : withRef).append(toJson(note));
    }
    QJsonObject measured{{QStringLiteral("source"), nullable(facts.measured.source)},
        {QStringLiteral("commits"), facts.measured.commits ? QJsonValue(QJsonArray::fromStringList(*facts.measured.commits)) : QJsonValue::Null},
        {QStringLiteral("files_changed"), facts.measured.filesChanged ? QJsonValue(*facts.measured.filesChanged) : QJsonValue::Null},
        {QStringLiteral("evidence"), evidence}};
    if (!facts.measured.headSha.isEmpty()) measured.insert(QStringLiteral("head_sha"), facts.measured.headSha);
    QJsonValue instructions = QJsonValue::Null;
    if (started.instructions) {
        QJsonArray list;
        for (const Instruction& instruction : *started.instructions) {
            list.append(QJsonObject{{QStringLiteral("name"), instruction.name}, {QStringLiteral("path"), instruction.path},
                {QStringLiteral("sha1"), nullable(instruction.sha1)}});
        }
        instructions = list;
    }
    return {{QStringLiteral("task"), facts.task}, {QStringLiteral("exec"), nullable(facts.exec)},
        {QStringLiteral("last_activity"), facts.lastActivity.isValid()
            ? QJsonValue(facts.lastActivity.toString(Qt::ISODate)) : QJsonValue::Null},
        {QStringLiteral("plan_ref"), nullable(started.planRef)},
        {QStringLiteral("plan_sha1"), nullable(recordedPlan)},
        {QStringLiteral("current_plan_sha1"), nullable(facts.planSha1)},
        {QStringLiteral("plan_changed"), recordedPlan.isEmpty() || facts.planSha1.isEmpty()
            ? QJsonValue::Null : QJsonValue(recordedPlan != facts.planSha1)},
        {QStringLiteral("workspace"), QJsonObject{
            {QStringLiteral("worktree"), nullable(started.worktree)},
            {QStringLiteral("branch"), nullable(started.branch)},
            {QStringLiteral("base"), nullable(started.base)},
            {QStringLiteral("base_sha"), nullable(started.baseSha)},
            {QStringLiteral("remote_base_sha"), nullable(started.remoteBaseSha)},
            {QStringLiteral("current_base_sha"), nullable(facts.currentBaseSha)},
            {QStringLiteral("worktree_exists"), boolean(facts.worktree.exists)},
            {QStringLiteral("base_advanced"), boolean(facts.baseAdvanced)}}},
        {QStringLiteral("measured"), measured},
        {QStringLiteral("agent_claims"), QJsonObject{{QStringLiteral("verification"), QStringLiteral("unverified")},
            {QStringLiteral("evidence"), claims}}},
        {QStringLiteral("unresolved"), QJsonObject{{QStringLiteral("with_ref"), withRef}, {QStringLiteral("without_ref"), withoutRef}}},
        {QStringLiteral("instructions"), instructions},
        {QStringLiteral("preserved_ref"), nullable(facts.finished ? facts.finished->preservedRef : started.preservedRef)},
        {QStringLiteral("handoff"), QJsonObject{{QStringLiteral("path"), nullable(facts.handoff.path)},
            {QStringLiteral("sha1"), nullable(facts.handoff.sha1)}, {QStringLiteral("recorded_sha1"), nullable(recordedHandoff)},
            {QStringLiteral("verified"), recordedHandoff.isEmpty() || facts.handoff.sha1.isEmpty()
                ? QJsonValue::Null : QJsonValue(recordedHandoff == facts.handoff.sha1)},
            {QStringLiteral("content"), facts.handoff.sha1.isEmpty() ? QJsonValue::Null : QJsonValue(facts.handoffContent)}}},
        {QStringLiteral("gaps"), gaps}};
}

QString resumeMarkdown(const QJsonObject& package)
{
    // This output is meant to be handed to an agent. Whoever wants JSON uses
    // the default; here we use the same headings as handoff.md, otherwise the
    // two formats drift apart.
    QString text;
    QTextStream out(&text);

    const auto str = [](const QJsonValue& value, const QString& fallback = QStringLiteral("—")) {
        return value.isString() ? value.toString() : fallback;
    };
    // Unknown and no are different: null stays "—", false is stated outright.
    const auto tri = [](const QJsonValue& value, const QString& yes, const QString& no) {
        return value.isBool() ? (value.toBool() ? yes : no) : QStringLiteral("unknown");
    };

    const QJsonValue task = package.value(QStringLiteral("task"));
    out << "# Runmark resume";
    if (task.isString() && !task.toString().isEmpty()) out << ": " << task.toString();
    out << "\n\n";

    const QJsonValue exec = package.value(QStringLiteral("exec"));
    if (!exec.isString()) {
        out << (task.isString() && !task.toString().isEmpty()
                ? "No execution recorded for this task.\n\n"
                : "No execution recorded in this project.\n\n");
    } else {
        const QJsonObject workspace = package.value(QStringLiteral("workspace")).toObject();
        out << "exec: " << exec.toString() << '\n'
            << "plan: " << str(package.value(QStringLiteral("plan_ref")))
            << "  (" << tri(package.value(QStringLiteral("plan_changed")),
                            QStringLiteral("CHANGED since the execution"), QStringLiteral("unchanged")) << ")\n"
            << "worktree: " << str(workspace.value(QStringLiteral("worktree")))
            << "  (" << tri(workspace.value(QStringLiteral("worktree_exists")),
                            QStringLiteral("present"), QStringLiteral("MISSING ON DISK")) << ")\n"
            << "branch: " << str(workspace.value(QStringLiteral("branch"))) << '\n'
            << "base: " << str(workspace.value(QStringLiteral("base"))) << '@'
            << str(workspace.value(QStringLiteral("base_sha")))
            << "  (" << tri(workspace.value(QStringLiteral("base_advanced")),
                            QStringLiteral("MOVED since then"), QStringLiteral("up to date")) << ")\n";
        // Whether anyone is still on this execution is not answerable from the
        // absence of a finish event alone; the age of the last write is what a
        // reader can act on.
        const QJsonValue activity = package.value(QStringLiteral("last_activity"));
        if (activity.isString()) {
            const QDateTime stamp = QDateTime::fromString(activity.toString(), Qt::ISODate);
            const qint64 seconds = stamp.secsTo(QDateTime::currentDateTimeUtc());
            out << "last activity: " << activity.toString()
                << (seconds < 120 ? QStringLiteral("  (just now)")
                    : seconds < 7200 ? QStringLiteral("  (%1 minutes ago)").arg(seconds / 60)
                    : QStringLiteral("  (%1 hours ago)").arg(seconds / 3600)) << '\n';
        }
        const QJsonValue preserved = package.value(QStringLiteral("preserved_ref"));
        if (preserved.isString()) out << "preserved work: " << preserved.toString() << '\n';
        out << '\n';
    }

    out << "## Verified (produced by Runmark)\n\n";
    const QJsonObject measured = package.value(QStringLiteral("measured")).toObject();
    const QJsonArray commits = measured.value(QStringLiteral("commits")).toArray();
    if (commits.isEmpty()) {
        out << "No commits.\n";
    } else {
        out << "Commits:\n";
        for (const QJsonValue& commit : commits) out << "- " << commit.toString() << '\n';
    }
    const QJsonValue filesChanged = measured.value(QStringLiteral("files_changed"));
    if (filesChanged.isDouble()) out << "\nFiles changed: " << filesChanged.toInt() << '\n';
    const QJsonArray evidence = measured.value(QStringLiteral("evidence")).toArray();
    out << "\nEvidence:\n";
    if (evidence.isEmpty()) {
        out << "None.\n";
    } else {
        for (const QJsonValue& value : evidence) {
            const QJsonObject item = value.toObject();
            out << "- " << str(item.value(QStringLiteral("kind"))) << ": "
                << str(item.value(QStringLiteral("summary"))) << '\n';
        }
    }

    out << "\n## Agent note (weak evidence \u2014 unverified)\n\n";
    const QJsonArray claims = package.value(QStringLiteral("agent_claims")).toObject()
        .value(QStringLiteral("evidence")).toArray();
    if (claims.isEmpty()) {
        out << "None.\n";
    } else {
        for (const QJsonValue& value : claims) {
            const QString kind = str(value.toObject().value(QStringLiteral("kind")));
            out << "- " << (kind == QLatin1String("agent_summary") ? QString() : QLatin1Char('[') + kind + QStringLiteral("] "))
                << str(value.toObject().value(QStringLiteral("summary"))) << '\n';
        }
    }

    out << "\n## Open items\n\n";
    const QJsonObject unresolved = package.value(QStringLiteral("unresolved")).toObject();
    bool anyUnresolved = false;
    for (const QString& key : {QStringLiteral("with_ref"), QStringLiteral("without_ref")}) {
        for (const QJsonValue& value : unresolved.value(key).toArray()) {
            const QJsonObject note = value.toObject();
            const QJsonValue reference = note.value(QStringLiteral("ref"));
            out << "- [ ] " << str(note.value(QStringLiteral("text")))
                << (reference.isString() ? QStringLiteral("  (ref: ") + reference.toString() + QLatin1Char(')')
                                         : QStringLiteral("  (no ref)")) << '\n';
            anyUnresolved = true;
        }
    }
    if (!anyUnresolved) out << "None.\n";

    out << "\n## Instructions\n\n";
    const QJsonValue instructions = package.value(QStringLiteral("instructions"));
    const QJsonArray instructionList = instructions.toArray();
    if (instructionList.isEmpty()) {
        out << (instructions.isNull() ? "Not recorded.\n" : "None.\n");
    } else {
        for (const QJsonValue& value : instructionList) {
            const QJsonObject item = value.toObject();
            const QJsonValue sha1 = item.value(QStringLiteral("sha1"));
            out << "- " << str(item.value(QStringLiteral("name"))) << "  "
                << (sha1.isString() ? QStringLiteral("sha1 ") + sha1.toString().left(12)
                                    : QStringLiteral("UNREADABLE")) << '\n';
        }
    }

    out << "\n## Handoff\n\n";
    const QJsonObject handoff = package.value(QStringLiteral("handoff")).toObject();
    const QJsonValue verified = handoff.value(QStringLiteral("verified"));
    if (!handoff.value(QStringLiteral("path")).isString()) {
        out << "None.\n";
    } else {
        out << "path: " << str(handoff.value(QStringLiteral("path"))) << '\n'
            << "sha1: " << str(handoff.value(QStringLiteral("sha1"))) << '\n';
        if (!handoff.value(QStringLiteral("sha1")).isString()) {
            out << "status: file could not be read\n";
        } else if (!verified.isBool()) {
            out << "status: unverifiable (no sha1 recorded in the ledger)\n";
        } else if (verified.toBool()) {
            out << "status: matches the recorded sha1\n";
        } else {
            out << "status: CHANGED \u2014 recorded sha1 "
                << str(handoff.value(QStringLiteral("recorded_sha1"))) << '\n';
        }
    }

    out << "\n## Gaps\n\n";
    const QJsonArray gaps = package.value(QStringLiteral("gaps")).toArray();
    if (gaps.isEmpty()) {
        out << "None.\n";
    } else {
        for (const QJsonValue& value : gaps) {
            const QJsonObject gap = value.toObject();
            out << "- " << str(gap.value(QStringLiteral("id"))) << " — "
                << str(gap.value(QStringLiteral("explanation"))) << '\n';
        }
    }
    return text;
}

} // namespace runmark
