// evaluate() is pure, so this test sets up no git repo, no filesystem and no
// clock. Facts are built by hand and findings are checked directly.

import runmark.domain;

#include <QJsonArray>
#include <QJsonObject>
#include <QTextStream>

namespace {

bool has(const QVector<runmark::Finding>& findings, const QString& id)
{
    for (const runmark::Finding& finding : findings) {
        if (finding.id == id) return true;
    }
    return false;
}

runmark::ProjectConfig config()
{
    runmark::ProjectConfig c;
    c.name = QStringLiteral("t");
    c.planPaths = {QStringLiteral("plan.md")};
    c.taskIdPattern = QStringLiteral("MF-\\d+");
    return c;
}

runmark::ExecutionStarted startedEvent(const QString& exec, const QString& task, const QString& ts)
{
    runmark::ExecutionStarted e;
    e.exec = exec;
    e.task = task;
    e.ts = ts;
    e.at = QDateTime::fromString(ts, Qt::ISODate);
    e.repo = QStringLiteral("r");
    e.planRef = QStringLiteral("plan.md#L1");
    e.baseSha = QStringLiteral("aaa");
    e.worktree = QStringLiteral("/w/MF-1");
    return e;
}

runmark::EvidenceRecorded agentEvidence(const QString& task, const QString& kind)
{
    runmark::EvidenceRecorded e;
    e.task = task;
    e.kind = kind;
    return e;
}

} // namespace

int main()
{
    const QDateTime now = QDateTime::fromString(QStringLiteral("2026-09-20T12:00:00Z"), Qt::ISODate);

    // 1. Git: even when fetch fails, what can be computed offline still speaks.
    {
        runmark::StatusFacts f;
        f.now = now;
        f.plan.readable = true;
        f.plan.taskCount = 1;
        runmark::RepoFacts repo;
        repo.name = QStringLiteral("r");
        repo.base = QStringLiteral("origin/main");
        repo.fetchError = QStringLiteral("network down");
        repo.measured = true;
        repo.dirty = true;
        repo.behind = 3;
        f.repos.append(repo);
        const QVector<runmark::Finding> findings = runmark::evaluate(config(), f);
        if (!has(findings, QStringLiteral("git.fetch_failed"))
                || !has(findings, QStringLiteral("git.dirty_workspace"))
                || !has(findings, QStringLiteral("git.remote_ahead"))) return 1;
    }

    // 2. If measurement did not complete, no measurement-based rule fires.
    {
        runmark::StatusFacts f;
        f.now = now;
        f.plan.readable = true;
        f.plan.taskCount = 1;
        runmark::RepoFacts repo;
        repo.name = QStringLiteral("r");
        repo.measurementError = QStringLiteral("boom");
        repo.dirty = true;   // must be ignored: it was never measured
        f.repos.append(repo);
        if (has(runmark::evaluate(config(), f), QStringLiteral("git.dirty_workspace"))) return 1;
    }

    // 3. The 24-hour threshold -- "now" is a fact, so no ledger date is rewound.
    {
        runmark::StatusFacts f;
        f.now = now;
        f.plan.readable = true;
        f.plan.taskCount = 1;
        f.ledger.started.append(startedEvent(QStringLiteral("E1"), QStringLiteral("MF-1"), QStringLiteral("2026-09-20T11:00:00Z")));
        f.executions.append({QStringLiteral("E1"), false, false});
        const QVector<runmark::Finding> fresh = runmark::evaluate(config(), f);
        if (!has(fresh, QStringLiteral("context.active_execution"))
                || has(fresh, QStringLiteral("context.orphaned_execution"))) return 1;

        f.ledger.started[0] = startedEvent(QStringLiteral("E1"), QStringLiteral("MF-1"), QStringLiteral("2026-09-19T11:00:00Z"));
        const QVector<runmark::Finding> stale = runmark::evaluate(config(), f);
        if (!has(stale, QStringLiteral("context.orphaned_execution"))
                || has(stale, QStringLiteral("context.active_execution"))) return 1;

        f.ledger.started[0] = startedEvent(QStringLiteral("E1"), QStringLiteral("MF-1"), QStringLiteral("bozuk"));
        if (!has(runmark::evaluate(config(), f), QStringLiteral("context.invalid_ledger_timestamp"))) return 1;
    }

    // 4. Plan blindness: readable but no tasks -> must warn.
    {
        runmark::StatusFacts f;
        f.now = now;
        f.plan.readable = true;
        f.plan.checklistCount = 3;
        f.plan.taskCount = 0;
        if (!has(runmark::evaluate(config(), f), QStringLiteral("plan.no_parsable_tasks"))) return 1;

        f.plan.readable = false;
        f.plan.unreadable = {QStringLiteral("plan.md")};
        if (!has(runmark::evaluate(config(), f), QStringLiteral("plan.unreadable"))) return 1;
    }

    // 4b. Several plan files: a missing one is reported, the rest still count.
    {
        runmark::StatusFacts f;
        f.now = now;
        f.plan.readable = true;
        f.plan.taskCount = 1;
        f.plan.doneTasks = {QStringLiteral("MF-1")};
        f.plan.unreadable = {QStringLiteral("docs/superpowers/plans/*.md")};
        const QVector<runmark::Finding> findings = runmark::evaluate(config(), f);
        if (!has(findings, QStringLiteral("plan.unreadable")) || !has(findings, QStringLiteral("plan.done_without_evidence"))) return 1;
        // plan_ref points into one file; only that file's fingerprint counts.
        f.plan.files = {{QStringLiteral("a.md"), 1, 0, QStringLiteral("sha-a")}, {QStringLiteral("b.md"), 1, 0, QStringLiteral("sha-b2")}};
        if (f.plan.sha1For(QStringLiteral("a.md#L3")) != QStringLiteral("sha-a") || !f.plan.sha1For(QStringLiteral("c.md#L1")).isEmpty()) return 1;
        runmark::ExecutionStarted e = startedEvent(QStringLiteral("E1"), QStringLiteral("MF-1"), QStringLiteral("2026-09-20T11:00:00Z"));
        e.planRef = QStringLiteral("a.md#L3");
        e.planSha1 = QStringLiteral("sha-a");
        f.ledger.started = {e};
        if (has(runmark::evaluate(config(), f), QStringLiteral("plan.changed_during_execution"))) return 1;
        f.plan.files[0].sha1 = QStringLiteral("sha-a2");
        if (!has(runmark::evaluate(config(), f), QStringLiteral("plan.changed_during_execution"))) return 1;
    }

    // 5b. An ambiguous ID is reported per line and never guessed into a task.
    {
        runmark::StatusFacts f;
        f.now = now;
        f.plan.readable = true;
        f.plan.checklistCount = 2;
        f.plan.taskCount = 0;
        f.plan.ambiguousTasks = {QStringLiteral("- [x] MF-1-W1 first"), QStringLiteral("- [x] MF-1-W2 second")};
        const QVector<runmark::Finding> findings = runmark::evaluate(config(), f);
        int count = 0;
        for (const runmark::Finding& finding : findings) {
            if (finding.id != QLatin1String("plan.ambiguous_task_id")) continue;
            ++count;
            if (!finding.explanation.contains(QStringLiteral("MF-\\d+"))
                    || finding.suggestedAction.isEmpty()) return 1;
        }
        if (count != 2 || has(findings, QStringLiteral("plan.done_without_evidence"))) return 1;
        // The blindness rule still speaks: nothing parsed is not a clean plan.
        if (!has(findings, QStringLiteral("plan.no_parsable_tasks"))) return 1;
    }

    // 4b. Two signals: "no finish event" alone cannot separate a session that is
    //     working from one that stopped hours ago. The age of the newest event
    //     decides, and it is reported where a reader will act on it.
    {
        runmark::StatusFacts f;
        f.now = now;
        f.plan.readable = true;
        f.plan.taskCount = 1;
        f.ledger.started.append(startedEvent(QStringLiteral("E1"), QStringLiteral("MF-1"), QStringLiteral("2026-09-20T11:00:00Z")));
        runmark::ExecutionFacts busy{QStringLiteral("E1"), false, false, QStringLiteral("codex"),
            QDateTime::fromString(QStringLiteral("2026-09-20T11:59:30Z"), Qt::ISODate)};
        f.executions.append(busy);
        QString explanation;
        for (const runmark::Finding& finding : runmark::evaluate(config(), f)) {
            if (finding.id == QLatin1String("context.active_execution")) explanation = finding.explanation;
        }
        if (!explanation.contains(QStringLiteral("(codex)")) || !explanation.contains(QStringLiteral("just now"))) return 1;

        f.executions[0].lastActivity = QDateTime::fromString(QStringLiteral("2026-09-20T07:00:00Z"), Qt::ISODate);
        explanation.clear();
        for (const runmark::Finding& finding : runmark::evaluate(config(), f)) {
            if (finding.id == QLatin1String("context.active_execution")) explanation = finding.explanation;
        }
        if (!explanation.contains(QStringLiteral("5 hours ago"))) return 1;

        // No usable timestamp must read as unknown, never as fresh.
        f.executions[0].lastActivity = {};
        explanation.clear();
        for (const runmark::Finding& finding : runmark::evaluate(config(), f)) {
            if (finding.id == QLatin1String("context.active_execution")) explanation = finding.explanation;
        }
        if (!explanation.contains(QStringLiteral("last activity unknown"))) return 1;
    }

    // 5. Only measured evidence closes "done" (ADR-002): the agent's own
    //    records, even of kind test or commit, are claims.
    {
        runmark::StatusFacts f;
        f.now = now;
        f.plan.readable = true;
        f.plan.taskCount = 1;
        f.plan.doneTasks = {QStringLiteral("MF-1")};
        if (!has(runmark::evaluate(config(), f), QStringLiteral("plan.done_without_evidence"))) return 1;

        for (const QString& kind : {QStringLiteral("test"), QStringLiteral("commit"), QStringLiteral("agent_summary")}) {
            f.ledger.evidence = {agentEvidence(QStringLiteral("MF-1"), kind)};
            if (!has(runmark::evaluate(config(), f), QStringLiteral("plan.done_without_evidence"))) return 1;
        }

        runmark::EvidenceRecorded measured = agentEvidence(QStringLiteral("MF-1"), QStringLiteral("test"));
        measured.exec = QStringLiteral("E1");
        measured.fromRuntime = true;
        measured.exitCode = 0;
        f.ledger.evidence = {measured};
        QVector<runmark::Finding> findings = runmark::evaluate(config(), f);
        if (has(findings, QStringLiteral("plan.done_without_evidence")) || has(findings, QStringLiteral("context.last_test_failed"))
                || has(findings, QStringLiteral("context.test_result_unknown"))) return 1;

        // Piped into tail: a run happened, its result did not reach us.
        f.ledger.evidence[0].exitCode.reset();
        findings = runmark::evaluate(config(), f);
        if (!has(findings, QStringLiteral("context.test_result_unknown")) || has(findings, QStringLiteral("context.last_test_failed"))) return 1;
        f.ledger.evidence[0].exitCode = 2;
        if (!has(runmark::evaluate(config(), f), QStringLiteral("context.last_test_failed"))) return 1;
    }

    // 5d. Whose exit code is it? Measured in this repository's own sessions:
    //     135 of 142 agent test commands were piped.
    {
        const QString pattern = runmark::ProjectConfig::defaultTestCommandPattern();
        const struct { const char* command; bool covered; } cases[] = {
            {"ctest --preset dev", true},
            {"cd build && ctest --output-on-failure", true},
            {"ctest --preset dev && echo done", true},
            {"ctest --preset dev 2>&1", true},
            {"ctest --preset dev &> log.txt", true},
            {"set -o pipefail; ctest --preset dev | tail -5", true},
            {"ctest --preset dev 2>&1 | tail -20", false},
            {"ctest --preset dev; echo done", false},
            {"ctest --preset dev || true", false},
            {"ctest --preset dev &", false},
            {"ctest -R one\necho next", false},
            {"echo build only", false},
        };
        for (const auto& c : cases) {
            const QString command = QString::fromLatin1(c.command);
            if (runmark::exitCodeCoversTestRun(command, pattern) != c.covered) {
                QTextStream(stderr) << "exitCodeCoversTestRun wrong for: " << c.command << '\n';
                return 1;
            }
        }
    }

    // Resume: deterministic facts, no git/filesystem/clock access.
    {
        runmark::ResumeFacts f;
        f.task = QStringLiteral("MF-1");
        if (!has(runmark::evaluateResume(f), QStringLiteral("context.no_execution"))) return 1;
        f.ledgerError = QStringLiteral("permission denied");
        if (!has(runmark::evaluateResume(f), QStringLiteral("context.ledger_unreadable"))
                || has(runmark::evaluateResume(f), QStringLiteral("context.no_execution"))) return 1;
        f.ledgerError.clear();
        f.exec = QStringLiteral("E1");
        f.started = startedEvent(f.exec, f.task, QStringLiteral("2026-09-20T11:00:00Z"));
        f.started->planSha1 = QStringLiteral("plan");
        f.started->instructions = QVector<runmark::Instruction>{};
        f.planSha1 = QStringLiteral("plan");
        f.finished = runmark::ExecutionFinished{};
        f.finished->handoffSha1 = QStringLiteral("handoff");
        f.handoff.sha1 = QStringLiteral("handoff");
        // These paths are always populated in practice; leaving them empty
        // would produce an empty explanation.
        f.handoff.path = QStringLiteral("/p/.runmark/handoffs/E1.md");
        f.worktree.path = QStringLiteral("/w/MF-1");
        f.handoff.exists = true;
        f.worktree.exists = true;
        f.baseAdvanced = false;
        if (!runmark::evaluateResume(f).isEmpty()) return 1;

        f.handoff.exists = false;
        f.handoff.sha1.clear();
        f.worktree.exists = false;
        f.baseAdvanced = true;
        f.currentBaseSha = QStringLiteral("bbb");
        f.planSha1 = QStringLiteral("changed");
        const QVector<runmark::Finding> missing = runmark::evaluateResume(f);
        for (const QString& id : {QStringLiteral("context.no_handoff"), QStringLiteral("git.worktree_missing"),
                QStringLiteral("git.base_advanced"), QStringLiteral("plan.changed_during_execution")}) {
            if (!has(missing, id)) return 1;
        }
        // The type now guarantees the fields exist; the remaining risk is
        // leaving one empty.
        for (const runmark::Finding& gap : missing) {
            if (gap.id.isEmpty() || gap.severity.isEmpty() || gap.domain.isEmpty()
                    || gap.title.isEmpty() || gap.explanation.isEmpty()) return 1;
        }

        f.handoff.exists.reset();
        f.worktree.exists.reset();
        f.baseAdvanced.reset();
        f.planSha1.clear();
        f.fetchError = QStringLiteral("offline");
        f.measurementError = QStringLiteral("missing git objects");
        f.started->instructions.reset();
        const QVector<runmark::Finding> unknown = runmark::evaluateResume(f);
        for (const QString& id : {QStringLiteral("context.handoff_unreadable"), QStringLiteral("git.worktree_unknown"),
                QStringLiteral("git.base_unknown"), QStringLiteral("git.fetch_failed"), QStringLiteral("plan.comparison_unknown"),
                QStringLiteral("git.measurement_unavailable"), QStringLiteral("context.instructions_unknown")}) {
            if (!has(unknown, id)) return 1;
        }
        if (has(unknown, QStringLiteral("git.worktree_missing")) || has(unknown, QStringLiteral("context.no_handoff"))
                || has(unknown, QStringLiteral("plan.changed_during_execution")) || has(unknown, QStringLiteral("git.base_advanced"))) return 1;
        f.handoff.exists = true;
        f.handoff.sha1 = QStringLiteral("edited");
        if (!has(runmark::evaluateResume(f), QStringLiteral("context.handoff_changed"))) return 1;
        f.finished->handoffSha1.clear();
        if (!has(runmark::evaluateResume(f), QStringLiteral("context.handoff_unverified"))) return 1;
        f.started->instructions = QVector<runmark::Instruction>{{QStringLiteral("missing.md"), QStringLiteral("missing.md"), QString()}};
        if (!has(runmark::evaluateResume(f), QStringLiteral("context.instruction_unreadable"))) return 1;
    }

    // 5c. An event of unknown type is reported, not skipped: a newer or
    //     corrupted ledger must not read as a quieter one.
    {
        runmark::StatusFacts f;
        f.now = now;
        f.plan.readable = true;
        f.plan.taskCount = 1;
        if (has(runmark::evaluate(config(), f), QStringLiteral("context.unrecognised_ledger_event"))) return 1;
        f.ledger.unrecognised = {QStringLiteral("E1.jsonl: execution.paused")};
        if (!has(runmark::evaluate(config(), f), QStringLiteral("context.unrecognised_ledger_event"))) return 1;
    }

    // 5e. Conflict radar: two live sessions on the same task or files.
    {
        runmark::StatusFacts f;
        f.now = now;
        f.plan.readable = true;
        f.plan.taskCount = 1;
        const auto live = [&](const QString& id, const QStringList& files, const QStringList& tasks = {}) {
            runmark::SessionFacts s;
            s.id = id;
            s.runtime = QStringLiteral("claude");
            s.startedAt = now.addSecs(-600);
            s.changedFiles = files;
            s.tasks = tasks;
            return s;
        };
        const QString rules = QStringLiteral("/repo/.git//libs/domain/src/rules.cpp");
        f.sessions = {live("a", {rules}), live("b", {QStringLiteral("/repo/.git//README.md")})};
        if (has(runmark::evaluate(config(), f), QStringLiteral("context.session_conflict"))) return 1;
        f.sessions[1].changedFiles.append(rules);
        QVector<runmark::Finding> findings = runmark::evaluate(config(), f);
        if (!has(findings, QStringLiteral("context.session_conflict"))) return 1;
        for (const runmark::Finding& finding : findings) {
            if (finding.id == QLatin1String("context.session_conflict") && !finding.explanation.contains(QStringLiteral("libs/domain/src/rules.cpp"))) return 1;
        }
        // One checkout: the changes cannot be attributed to either session.
        f.sessions[0].cwd = f.sessions[1].cwd = QStringLiteral("/repo");
        for (const runmark::Finding& finding : runmark::evaluate(config(), f)) {
            if (finding.id == QLatin1String("context.session_conflict") && !finding.explanation.contains(QStringLiteral("share the checkout /repo"))) return 1;
        }
        f.sessions[0].cwd.clear();
        f.sessions[1].cwd.clear();
        // Same name in another repository is not the same file.
        f.sessions[1].changedFiles = {QStringLiteral("/other/.git//libs/domain/src/rules.cpp")};
        if (has(runmark::evaluate(config(), f), QStringLiteral("context.session_conflict"))) return 1;
        // Same task, no shared file yet.
        f.sessions[0].tasks = {QStringLiteral("MF-1")};
        f.sessions[1].tasks = {QStringLiteral("MF-1")};
        if (!has(runmark::evaluate(config(), f), QStringLiteral("context.session_conflict"))) return 1;
        // An ended session, or one silent for over 12 hours, is not live.
        f.sessions[1].endedAt = now;
        if (has(runmark::evaluate(config(), f), QStringLiteral("context.session_conflict"))) return 1;
        f.sessions[1].endedAt.reset();
        f.sessions[1].startedAt = now.addSecs(-13 * 60 * 60);
        if (has(runmark::evaluate(config(), f), QStringLiteral("context.session_conflict"))) return 1;
    }

    // 5f. A session seen only in a transcript ran without hooks; reported
    //     only once hooks are expected (ADR-015 opt-in).
    {
        runmark::StatusFacts f;
        f.now = now;
        f.plan.readable = true;
        f.plan.taskCount = 1;
        runmark::SessionFacts ghost;
        ghost.id = QStringLiteral("ghost");
        ghost.runtime = QStringLiteral("codex");
        f.unregisteredSessions = {ghost};
        f.sessions = {ghost};
        runmark::ProjectConfig expects = config();
        if (has(runmark::evaluate(config(), f), QStringLiteral("context.unregistered_session"))) return 1;
        expects.hooksExpected = true;
        if (!has(runmark::evaluate(expects, f), QStringLiteral("context.unregistered_session"))) return 1;
        f.unregisteredSessions.clear();
        if (has(runmark::evaluate(expects, f), QStringLiteral("context.unregistered_session"))) return 1;
    }

    // 6. Hook blindness: once the expectation is declared, absence of an
    //    observation is a finding; without it the rule stays quiet, or a
    //    CLI-only project would see a warning it cannot turn off.
    {
        runmark::StatusFacts f;
        f.now = now;
        f.plan.readable = true;
        f.plan.taskCount = 1;
        runmark::ProjectConfig expects = config();
        expects.hooksExpected = true;
        if (has(runmark::evaluate(config(), f), QStringLiteral("context.hooks_not_observed"))) return 1;
        if (!has(runmark::evaluate(expects, f), QStringLiteral("context.hooks_not_observed"))) return 1;
        // Any recorded agent session proves a hook ran (sessions replaced
        // hook-observed.json; regression from eng review D5).
        runmark::SessionFacts seen;
        seen.id = QStringLiteral("s-1");
        f.sessions = {seen};
        if (has(runmark::evaluate(expects, f), QStringLiteral("context.hooks_not_observed"))) return 1;
        // An unreadable session directory must not count as "seen"; the
        // reason belongs in the explanation.
        f.sessions.clear();
        f.sessionsError = QStringLiteral("Invalid ts: soon");
        const QVector<runmark::Finding> broken = runmark::evaluate(expects, f);
        if (!has(broken, QStringLiteral("context.hooks_not_observed"))) return 1;
        bool explained = false;
        for (const runmark::Finding& f2 : broken) {
            if (f2.id == QLatin1String("context.hooks_not_observed")) {
                explained = f2.explanation.contains(QStringLiteral("Invalid ts: soon"));
            }
        }
        if (!explained) return 1;
    }

    // A one-command action also comes as argv, so a client can run it without
    // a shell; advice stays text only.
    {
        const auto command = [](const QVector<runmark::Finding>& findings, const QString& id) {
            for (const runmark::Finding& finding : findings) {
                if (finding.id == id) return finding.command;
            }
            return QStringList{};
        };
        runmark::StatusFacts f;
        f.now = now;
        f.ledger.started = {startedEvent(QStringLiteral("E1"), QStringLiteral("MF-1"), QStringLiteral("2026-09-20T11:00:00Z"))};
        runmark::ExecutionFinished finished;
        finished.exec = QStringLiteral("E1");
        finished.outcome = QStringLiteral("finished");
        f.ledger.finished = {finished};
        runmark::ExecutionFacts execution;
        execution.exec = QStringLiteral("E1");
        execution.hasHandoff = true;
        execution.worktreeExists = true;
        f.executions = {execution};
        if (command(runmark::evaluate(config(), f), QStringLiteral("git.orphaned_worktree"))
                != QStringList{QStringLiteral("git"), QStringLiteral("worktree"), QStringLiteral("remove"), QStringLiteral("/w/MF-1")}) return 1;
        f.ledger.finished[0].outcome = QStringLiteral("interrupted");
        if (command(runmark::evaluate(config(), f), QStringLiteral("context.interrupted_execution"))
                != QStringList{QStringLiteral("rmk"), QStringLiteral("start"), QStringLiteral("MF-1")}) return 1;
        f.repos = {{}};
        f.repos[0].measured = true;
        f.repos[0].dirty = true;
        if (!command(runmark::evaluate(config(), f), QStringLiteral("git.dirty_workspace")).isEmpty()) return 1;
    }

    return 0;
}
