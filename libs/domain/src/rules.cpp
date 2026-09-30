module;

#include <QDateTime>
#include <QMap>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <utility>

module runmark.domain;

namespace runmark {
namespace {

// "No finish event" is a status value. On its own it cannot tell a session that
// is working right now from one that stopped hours ago, and today both looked
// identical while two agents redid each other's work. The age of the newest
// recorded event is the second signal; when the two disagree, this one decides
// what a reader should do.
QString silenceFor(const ExecutionFacts* execution, const QDateTime& now)
{
    if (!execution || !execution->lastActivity.isValid()) {
        return QStringLiteral("last activity unknown");
    }
    const qint64 seconds = execution->lastActivity.secsTo(now);
    if (seconds < 0) return QStringLiteral("last activity is in the future");
    if (seconds < 120) return QStringLiteral("last activity just now");
    if (seconds < 7200) return QStringLiteral("last activity ") + QString::number(seconds / 60) + QStringLiteral(" minutes ago");
    return QStringLiteral("last activity ") + QString::number(seconds / 3600) + QStringLiteral(" hours ago");
}

const ExecutionFacts* executionFor(const StatusFacts& facts, const QString& executionId)
{
    for (const ExecutionFacts& execution : facts.executions) {
        if (execution.exec == executionId) return &execution;
    }
    return nullptr;
}

} // namespace

bool exitCodeCoversTestRun(const QString& command, const QString& testPattern)
{
    qsizetype end = -1;
    QRegularExpressionMatchIterator it = QRegularExpression(testPattern).globalMatch(command);
    while (it.hasNext()) end = it.next().capturedEnd();
    if (end < 0) return false;
    const bool pipefail = command.left(end).contains(QStringLiteral("pipefail"));
    const QString after = command.mid(end);
    for (qsizetype i = 0; i < after.size(); ++i) {
        const QChar c = after[i];
        const QChar next = i + 1 < after.size() ? after[i + 1] : QChar();
        const QChar previous = i > 0 ? after[i - 1] : QChar();
        if (c == QLatin1Char('\n') || c == QLatin1Char(';')) return false;
        if (c == QLatin1Char('|')) {
            if (next == QLatin1Char('|')) return false;
            if (!pipefail) return false;
        } else if (c == QLatin1Char('&')) {
            if (next == QLatin1Char('&')) { ++i; continue; }
            if (previous == QLatin1Char('>') || next == QLatin1Char('>')) continue;   // 2>&1, &>file
            return false;                                                        // background job
        }
    }
    return true;
}

Finding finding(const QString& id, const QString& severity, const QString& domain,
                const QString& title, const QString& explanation, const QString& action)
{
    return {id, severity, domain, title, explanation, action};
}

QVector<Finding> evaluateResume(const ResumeFacts& facts)
{
    QVector<Finding> gaps;
    const auto gap = [&gaps](const QString& id, const QString& domain, const QString& title, const QString& explanation) {
        gaps.append(finding(id, QStringLiteral("warning"), domain, title, explanation));
    };
    if (!facts.ledgerError.isEmpty()) {
        gap(QStringLiteral("context.ledger_unreadable"), QStringLiteral("context"), QStringLiteral("Execution history is unknown"), facts.ledgerError);
        return gaps;
    }
    if (facts.exec.isEmpty()) {
        gap(QStringLiteral("context.no_execution"), QStringLiteral("context"),
            facts.task.isEmpty() ? QStringLiteral("Ledger has no execution") : QStringLiteral("Task has no execution"),
            facts.task.isEmpty() ? QStringLiteral("no execution recorded in this project") : facts.task);
        return gaps;
    }
    if (!facts.started) {
        gap(QStringLiteral("context.missing_start"), QStringLiteral("context"), QStringLiteral("Execution has no start event"), facts.exec);
        return gaps;
    }
    const ExecutionStarted& started = *facts.started;
    if (!started.at.isValid()) {
        gap(QStringLiteral("context.invalid_ledger_timestamp"), QStringLiteral("context"), QStringLiteral("Invalid start timestamp"), facts.exec);
    }
    if (facts.handoff.exists == false) {
        gap(QStringLiteral("context.no_handoff"), QStringLiteral("context"), QStringLiteral("Handoff file is missing"), facts.handoff.path);
    } else if (facts.handoff.sha1.isEmpty()) {
        gap(QStringLiteral("context.handoff_unreadable"), QStringLiteral("context"), QStringLiteral("Handoff cannot be read"), facts.handoff.path + QStringLiteral(": ") + facts.handoff.error);
    } else {
        const QString recorded = facts.finished ? facts.finished->handoffSha1 : QString();
        if (recorded.isEmpty()) {
            gap(QStringLiteral("context.handoff_unverified"), QStringLiteral("context"), QStringLiteral("Original handoff hash is unknown"), facts.exec);
        } else if (recorded != facts.handoff.sha1) {
            gap(QStringLiteral("context.handoff_changed"), QStringLiteral("context"), QStringLiteral("Handoff changed after finish"), facts.handoff.path);
        }
    }
    if (!facts.worktree.exists.has_value()) {
        gap(QStringLiteral("git.worktree_unknown"), QStringLiteral("git"), QStringLiteral("Worktree existence is unknown"), facts.worktree.error);
    } else if (!*facts.worktree.exists) {
        gap(QStringLiteral("git.worktree_missing"), QStringLiteral("git"), QStringLiteral("Worktree is missing"), facts.worktree.path);
    }
    if (!facts.fetchError.isEmpty()) {
        gap(QStringLiteral("git.fetch_failed"), QStringLiteral("git"), QStringLiteral("Cannot fetch remote base"), facts.fetchError);
    }
    if (!facts.baseAdvanced.has_value()) {
        gap(QStringLiteral("git.base_unknown"), QStringLiteral("git"), QStringLiteral("Base advancement is unknown"),
            facts.baseError.isEmpty() ? QStringLiteral("No verified current base comparison is available") : facts.baseError);
    } else if (*facts.baseAdvanced) {
        gap(QStringLiteral("git.base_advanced"), QStringLiteral("git"), QStringLiteral("Base advanced since execution start"), facts.currentBaseSha);
    }
    const QString& recordedPlan = started.planSha1;
    if (recordedPlan.isEmpty() || facts.planSha1.isEmpty()) {
        gap(QStringLiteral("plan.comparison_unknown"), QStringLiteral("plan"), QStringLiteral("Plan change is unknown"), QStringLiteral("Recorded or current plan SHA1 is unavailable"));
    } else if (recordedPlan != facts.planSha1) {
        gap(QStringLiteral("plan.changed_during_execution"), QStringLiteral("plan"), QStringLiteral("Plan changed since execution start"), facts.task);
    }
    if (started.planRef.isEmpty()) {
        gap(QStringLiteral("plan.execution_without_plan_link"), QStringLiteral("plan"), QStringLiteral("Execution has no plan link"), facts.exec);
    }
    if (!facts.measurementError.isEmpty()) {
        gap(QStringLiteral("git.measurement_unavailable"), QStringLiteral("git"), QStringLiteral("Commit or file measurement is unavailable"), facts.measurementError);
    }
    if (!started.instructions) {
        gap(QStringLiteral("context.instructions_unknown"), QStringLiteral("context"), QStringLiteral("Instruction provenance was not recorded"), facts.exec);
    } else {
        for (const Instruction& instruction : *started.instructions) {
            if (instruction.sha1.isEmpty()) {
                gap(QStringLiteral("context.instruction_unreadable"), QStringLiteral("context"), QStringLiteral("Instruction hash was not recorded"), instruction.path);
            }
        }
    }
    for (const NoteRecorded& note : facts.notes) {
        if (note.kind == QLatin1String("unresolved") && note.ref.isEmpty()) {
            gaps.append(finding(QStringLiteral("context.unresolved_without_ref"), QStringLiteral("info"), QStringLiteral("context"),
                QStringLiteral("Unresolved note has no reference"), note.text));
        }
    }
    return gaps;
}

QVector<OpenExecution> openExecutions(const Ledger& ledger)
{
    // Execution IDs carry UTC time: lexical order is the ledger's chronology.
    QMap<QString, ExecutionStarted> newest;
    for (const ExecutionStarted& started : ledger.started) {
        if (!newest.contains(started.task) || started.exec > newest.value(started.task).exec) newest.insert(started.task, started);
    }
    QVector<OpenExecution> open;
    for (const ExecutionStarted& started : std::as_const(newest)) {
        OpenExecution execution{started, {}, started.at};
        const auto touch = [&execution](const QDateTime& at) {
            if (at.isValid() && (!execution.lastActivity.isValid() || at > execution.lastActivity)) execution.lastActivity = at;
        };
        bool closed = false;
        for (const ExecutionFinished& finished : ledger.finished) {
            if (finished.exec != started.exec) continue;
            touch(finished.at);
            if (finished.outcome == QLatin1String("interrupted")) execution.outcome = finished.outcome;
            else closed = true;
        }
        if (closed) continue;
        for (const EvidenceRecorded& evidence : ledger.evidence) {
            if (evidence.exec == started.exec) touch(evidence.at);
        }
        for (const NoteRecorded& note : ledger.notes) {
            if (note.exec == started.exec) touch(note.at);
        }
        open.append(execution);
    }
    std::sort(open.begin(), open.end(), [](const OpenExecution& a, const OpenExecution& b) { return a.started.exec < b.started.exec; });
    return open;
}

QVector<Finding> evaluate(const ProjectConfig& config, const StatusFacts& facts)
{
    QVector<Finding> findings;

    // --- Git ---
    for (const RepoFacts& repository : facts.repos) {
        if (!repository.fetchError.isEmpty()) {
            findings.append(finding(QStringLiteral("git.fetch_failed"), QStringLiteral("warning"), QStringLiteral("git"),
                QStringLiteral("Cannot fetch remote"),
                repository.name + QStringLiteral(": ") + repository.fetchError,
                QStringLiteral("Restore remote access, then run status again.")));
        }
        if (!repository.measured) {
            continue;
        }
        if (repository.dirty) {
            findings.append(finding(QStringLiteral("git.dirty_workspace"), QStringLiteral("warning"), QStringLiteral("git"),
                QStringLiteral("Workspace has uncommitted changes"), repository.name + QStringLiteral(" is dirty")));
        }
        if (repository.behind > 0) {
            findings.append(finding(QStringLiteral("git.remote_ahead"), QStringLiteral("warning"), QStringLiteral("git"),
                QStringLiteral("Branch is behind remote base"),
                repository.name + QStringLiteral(" is ") + QString::number(repository.behind) + QStringLiteral(" commits behind ") + repository.base));
        }
        if (repository.localBaseExists && repository.localBehind > 0) {
            findings.append(finding(QStringLiteral("git.stale_local_base"), QStringLiteral("warning"), QStringLiteral("git"),
                QStringLiteral("Local base is behind remote"),
                repository.localBase + QStringLiteral(" is ") + QString::number(repository.localBehind) + QStringLiteral(" commits behind ") + repository.base));
        }
    }

    // --- Ledger scan ---
    const Ledger& ledger = facts.ledger;
    for (const QString& line : ledger.unrecognised) {
        findings.append(finding(QStringLiteral("context.unrecognised_ledger_event"), QStringLiteral("warning"), QStringLiteral("context"),
            QStringLiteral("Ledger has an event of unknown type"), line,
            QStringLiteral("Upgrade rmk if another version wrote it; otherwise inspect the ledger line.")));
    }
    // Only what Runmark or the runtime measured closes "done": a commit of the
    // execution, or evidence read from the runtime transcript. Anything the
    // agent recorded itself is a claim and never does (ADR-002).
    QSet<QString> evidencedTasks;
    QSet<QString> claimedTasks;
    // A test result counts as measured only when the agent runtime recorded
    // it; the agent's own `rmk evidence --kind test` is a claim (ADR-002).
    QSet<QString> runtimeTestedTasks;
    QSet<QString> claimedTestTasks;
    QMap<QString, EvidenceRecorded> lastRuntimeTest;
    QSet<QString> completedExecutions;
    QSet<QString> executionsWithCommits;
    for (const ExecutionFinished& finished : ledger.finished) {
        completedExecutions.insert(finished.exec);
        if (finished.commits && !finished.commits->isEmpty()) executionsWithCommits.insert(finished.exec);
        // A transcript that could not be read is unknown, not "no tests ran".
        if (finished.transcript.status == QLatin1String("unavailable")) {
            findings.append(finding(QStringLiteral("context.transcript_unavailable"), QStringLiteral("warning"), QStringLiteral("context"),
                QStringLiteral("Agent transcript could not be read"),
                finished.exec + QStringLiteral(": ") + finished.transcript.error,
                QStringLiteral("Test runs of this execution are unknown; check them before trusting its result.")));
        }
    }
    for (const EvidenceRecorded& evidence : ledger.evidence) {
        if (evidence.fromRuntime) evidencedTasks.insert(evidence.task);
        else if (evidence.kind != QLatin1String("manual_note")) claimedTasks.insert(evidence.task);
        if (evidence.kind != QLatin1String("test")) continue;
        if (evidence.fromRuntime) {
            // A run whose result is unknown does not verify a claim either.
            if (evidence.exitCode) runtimeTestedTasks.insert(evidence.task);
            lastRuntimeTest.insert(evidence.exec, evidence);
        } else {
            claimedTestTasks.insert(evidence.task);
        }
    }
    for (const NoteRecorded& note : ledger.notes) {
        if (note.kind == QLatin1String("unresolved") && note.ref.isEmpty()) {
            findings.append(finding(QStringLiteral("context.unresolved_without_ref"), QStringLiteral("info"), QStringLiteral("context"),
                QStringLiteral("Unresolved note has no reference"), note.text));
        }
    }

    for (auto it = lastRuntimeTest.cbegin(); it != lastRuntimeTest.cend(); ++it) {
        if (!it.value().exitCode) {
            findings.append(finding(QStringLiteral("context.test_result_unknown"), QStringLiteral("warning"), QStringLiteral("context"),
                QStringLiteral("Last test run's result is unknown"),
                it.key() + QStringLiteral(": ") + it.value().summary,
                QStringLiteral("Run the tests without piping their output (or with set -o pipefail) so the exit code is the test's own.")));
        } else if (*it.value().exitCode != 0) {
            findings.append(finding(QStringLiteral("context.last_test_failed"), QStringLiteral("warning"), QStringLiteral("context"),
                QStringLiteral("Last recorded test run failed"),
                it.key() + QStringLiteral(": ") + it.value().summary,
                QStringLiteral("Fix the failure before marking the task done, or record why it is expected.")));
        }
    }

    // Interrupted is finished on purpose, yet the work waits for someone; its
    // worktree is the work, not something left behind.
    QSet<QString> interrupted;
    for (const OpenExecution& open : openExecutions(ledger)) {
        if (open.outcome.isEmpty()) continue;
        interrupted.insert(open.started.exec);
        findings.append(finding(QStringLiteral("context.interrupted_execution"), QStringLiteral("info"), QStringLiteral("context"),
            QStringLiteral("Execution waits to be continued"),
            open.started.exec + QStringLiteral(" was interrupted; ") + silenceFor(executionFor(facts, open.started.exec), facts.now),
            QStringLiteral("rmk start ") + open.started.task));
    }

    // --- Per execution ---
    for (const ExecutionStarted& started : ledger.started) {
        const QString& executionId = started.exec;
        const bool completed = completedExecutions.contains(executionId);
        const ExecutionFacts* execution = executionFor(facts, executionId);
        const bool hasHandoff = execution && execution->hasHandoff;
        if (executionsWithCommits.contains(executionId)) {
            evidencedTasks.insert(started.task);
        }
        if (started.planRef.isEmpty()) {
            findings.append(finding(QStringLiteral("plan.execution_without_plan_link"), QStringLiteral("info"), QStringLiteral("plan"),
                QStringLiteral("Execution has no plan link"), executionId + QStringLiteral(" has no matching task in plan")));
        }
        if (completed && !hasHandoff) {
            findings.append(finding(QStringLiteral("context.no_handoff"), QStringLiteral("warning"), QStringLiteral("context"),
                QStringLiteral("Completed execution has no handoff"), executionId));
        }
        // A finished execution whose worktree survives on disk reads as active
        // work. Removal is never automatic (ARCHITECTURE.md "Security"); this
        // only makes it visible.
        if (completed && !interrupted.contains(executionId) && !started.worktree.isEmpty() && execution && execution->worktreeExists) {
            findings.append(finding(QStringLiteral("git.orphaned_worktree"), QStringLiteral("info"), QStringLiteral("git"),
                QStringLiteral("Completed execution still has a worktree"),
                started.worktree + QStringLiteral(" remains on disk after ") + executionId + QStringLiteral(" (")
                    + started.workspaceSource + QStringLiteral(")"),
                QStringLiteral("git worktree remove ") + started.worktree));
        }
        if (!completed && !hasHandoff) {
            if (!started.at.isValid()) {
                findings.append(finding(QStringLiteral("context.invalid_ledger_timestamp"), QStringLiteral("warning"), QStringLiteral("context"),
                    QStringLiteral("Execution has an invalid ledger timestamp"),
                    executionId + QStringLiteral(" has invalid ts: ") + started.ts));
            } else if (started.at.secsTo(facts.now) >= 24 * 60 * 60) {
                const qint64 ageSeconds = started.at.secsTo(facts.now);
                findings.append(finding(QStringLiteral("context.orphaned_execution"), QStringLiteral("warning"), QStringLiteral("context"),
                    QStringLiteral("Execution appears abandoned"),
                    executionId + QStringLiteral(" started ") + QString::number(ageSeconds / 3600)
                        + QStringLiteral(" hours ago without finish or handoff; ") + silenceFor(execution, facts.now)));
            } else {
                const QString agent = execution && !execution->agent.isEmpty()
                    ? QStringLiteral(" (") + execution->agent + QStringLiteral(")") : QString();
                findings.append(finding(QStringLiteral("context.active_execution"), QStringLiteral("info"), QStringLiteral("context"),
                    QStringLiteral("Execution is still active"),
                    executionId + agent + QStringLiteral(" has no finish event or handoff yet; ")
                        + silenceFor(execution, facts.now)));
            }
        }
        const QString currentPlanSha = facts.plan.sha1For(started.planRef);
        if (!completed && !started.planSha1.isEmpty() && !currentPlanSha.isEmpty() && started.planSha1 != currentPlanSha) {
            findings.append(finding(QStringLiteral("plan.changed_during_execution"), QStringLiteral("warning"), QStringLiteral("plan"),
                QStringLiteral("Plan changed during execution"), started.task));
        }
        if (!completed && execution && !execution->baseError.isEmpty()) {
            findings.append(finding(QStringLiteral("git.worktree_base_unknown"), QStringLiteral("warning"), QStringLiteral("git"),
                QStringLiteral("Worktree base comparison is unavailable"),
                executionId + QStringLiteral(": ") + execution->baseError));
        } else if (!completed && execution && execution->includesRemoteBase == false) {
            findings.append(finding(QStringLiteral("git.stale_worktree_base"), QStringLiteral("warning"), QStringLiteral("git"),
                QStringLiteral("Worktree base is stale"),
                executionId + QStringLiteral(" does not include the current ") + started.base));
        }
    }

    // --- Plan ---
    // Each missing plan file is reported; the other files are still read.
    for (const QString& path : facts.plan.unreadable) {
        findings.append(finding(QStringLiteral("plan.unreadable"), QStringLiteral("warning"), QStringLiteral("plan"),
            QStringLiteral("Plan file cannot be read"),
            path + (facts.plan.readable ? QStringLiteral(" could not be opened or matched no file")
                                        : QStringLiteral(" could not be opened; no plan rule was evaluated")),
            QStringLiteral("Fix project.plan.paths in project.json.")));
    }
    if (facts.plan.readable) {
        for (const QString& task : facts.plan.doneTasks) {
            if (!evidencedTasks.contains(task)) {
                findings.append(finding(QStringLiteral("plan.done_without_evidence"), QStringLiteral("warning"), QStringLiteral("plan"),
                    QStringLiteral("Done plan task has no evidence"),
                    claimedTasks.contains(task) ? task + QStringLiteral(": only agent-recorded evidence, nothing measured") : task));
            }
        }
        for (const QString& task : facts.plan.doneTasks) {
            if (claimedTestTasks.contains(task) && !runtimeTestedTasks.contains(task)) {
                findings.append(finding(QStringLiteral("plan.test_claim_unverified"), QStringLiteral("warning"), QStringLiteral("plan"),
                    QStringLiteral("Test result is only claimed by the agent"),
                    task + QStringLiteral(": no test run found in the agent runtime transcript"),
                    QStringLiteral("Run the tests inside the agent session before rmk finish.")));
            }
        }
        // An identifier we could only guess at binds evidence to the wrong
        // task, and a wrong link is worse than a missing one.
        for (const QString& line : facts.plan.ambiguousTasks) {
            findings.append(finding(QStringLiteral("plan.ambiguous_task_id"), QStringLiteral("warning"), QStringLiteral("plan"),
                QStringLiteral("Plan line carries an ambiguous task ID"),
                line + QStringLiteral(" matches ") + config.taskIdPattern
                    + QStringLiteral(" only as part of a longer identifier"),
                QStringLiteral("Widen project.task_id_pattern to cover the whole ID, or write the plain task ID on the line.")));
        }

        // Measuring nothing silently looks exactly like measuring a clean
        // result. Tell the two apart.
        if (facts.plan.taskCount == 0) {
            findings.append(finding(QStringLiteral("plan.no_parsable_tasks"), QStringLiteral("warning"), QStringLiteral("plan"),
                QStringLiteral("Plan yields no task candidates"),
                facts.plan.checklistCount == 0
                    ? config.planPaths.join(QStringLiteral(", ")) + QStringLiteral(" has no \"- [ ]\" / \"- [x]\" checklist item; plan rules evaluated nothing")
                    : QString::number(facts.plan.checklistCount) + QStringLiteral(" checklist items found in ") + config.planPaths.join(QStringLiteral(", ")) + QStringLiteral(" but none matched task_id_pattern ") + config.taskIdPattern,
                QStringLiteral("Write tasks as \"- [x] <TASK-ID> ...\" items, or fix project.task_id_pattern.")));
        }
    }

    // --- Hook ---
    // --- Conflict radar ---
    // Two live sessions on the same task or the same files redo or overwrite
    // each other's work; nobody sees it until the merge. Live: not ended and
    // active within 12 hours.
    // ponytail: a crashed session never sends session.ended; the 12-hour
    // window keeps it from colliding forever. Tune it if that proves short.
    QVector<const SessionFacts*> live;
    for (const SessionFacts& session : facts.sessions) {
        QDateTime last = session.startedAt;
        for (const QDateTime& at : {session.lastWorkingAt, session.lastWaitingAt}) {
            if (at.isValid() && (!last.isValid() || at > last)) last = at;
        }
        if (!session.endedAt && last.isValid() && last.secsTo(facts.now) < 12 * 60 * 60) live.append(&session);
    }
    for (qsizetype i = 0; i < live.size(); ++i) {
        for (qsizetype j = i + 1; j < live.size(); ++j) {
            const SessionFacts& a = *live[i];
            const SessionFacts& b = *live[j];
            QStringList shared;
            for (const QString& task : a.tasks) if (b.tasks.contains(task)) shared.append(QStringLiteral("task ") + task);
            for (const QString& file : a.changedFiles) {
                if (b.changedFiles.contains(file)) shared.append(file.section(QStringLiteral("//"), 1));
            }
            if (shared.isEmpty()) continue;
            const QString pair = a.id + QStringLiteral(" (") + a.runtime + QStringLiteral(") and ") + b.id + QStringLiteral(" (") + b.runtime + QStringLiteral(")");
            const QString list = QStringList(shared.mid(0, 3)).join(QStringLiteral(", "))
                + (shared.size() > 3 ? QStringLiteral(" and %1 more").arg(shared.size() - 3) : QString());
            // In one checkout the changes cannot be attributed to either
            // session; claiming both touched them was wrong (RM-14 #5).
            if (!a.cwd.isEmpty() && a.cwd == b.cwd) {
                findings.append(finding(QStringLiteral("context.session_conflict"), QStringLiteral("warning"), QStringLiteral("context"),
                    QStringLiteral("Two live agent sessions share one checkout"),
                    pair + QStringLiteral(" share the checkout ") + a.cwd + QStringLiteral(", which has changes in ") + list
                        + QStringLiteral("; which session made them cannot be told"),
                    QStringLiteral("Give one of them its own worktree with rmk start, or end the idle one.")));
                continue;
            }
            findings.append(finding(QStringLiteral("context.session_conflict"), QStringLiteral("warning"), QStringLiteral("context"),
                QStringLiteral("Two live agent sessions touch the same work"),
                pair + QStringLiteral(" both touch ") + list,
                QStringLiteral("Stop one of them or split the work before both finish.")));
        }
    }

    // Hooks cannot report their own absence; the runtimes write transcripts
    // regardless. A session seen only there ran blind to Runmark.
    if (config.hooksExpected && !facts.unregisteredSessions.isEmpty()) {
        QStringList names;
        for (const SessionFacts& session : facts.unregisteredSessions.mid(0, 3)) {
            names.append(session.id + QStringLiteral(" (") + session.runtime + QStringLiteral(")"));
        }
        findings.append(finding(QStringLiteral("context.unregistered_session"), QStringLiteral("warning"), QStringLiteral("context"),
            QStringLiteral("Agent sessions ran without Runmark hooks"),
            QString::number(facts.unregisteredSessions.size()) + QStringLiteral(" recent session(s) in this project left no record: ")
                + names.join(QStringLiteral(", ")) + (facts.unregisteredSessions.size() > 3 ? QStringLiteral(", ...") : QString()),
            QStringLiteral("Install or update the runmark-agent plugin; in Codex approve its hooks (/hooks).")));
    }

    // A hook believed to be installed but never run is indistinguishable from
    // a clean project. Once the expectation is declared, absence is a finding.
    if (config.hooksExpected && facts.sessions.isEmpty()) {
        findings.append(finding(QStringLiteral("context.hooks_not_observed"), QStringLiteral("warning"), QStringLiteral("context"),
            QStringLiteral("No agent hook has been observed"),
            facts.sessionsError.isEmpty()
                ? QStringLiteral("project.hooks_expected is true but no agent session has been recorded yet")
                : QStringLiteral("Session records cannot be read: ") + facts.sessionsError,
            QStringLiteral("Install the runmark-agent plugin, then open a new agent session.")));
    }

    return findings;
}

} // namespace runmark
