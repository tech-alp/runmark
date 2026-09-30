#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
#include <stdexcept>

namespace {

bool writeFile(const QString& path, const QByteArray& contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

bool hasError(const QByteArray& output, const QString& code)
{
    const QJsonDocument document = QJsonDocument::fromJson(output);
    return document.isObject() && document.object().value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toString() == code;
}

bool run(const QString& executable, const QStringList& arguments, int expectedExitCode, QByteArray* standardOutput, QByteArray* standardError)
{
    QProcess process;
    process.start(executable, arguments);
    return process.waitForStarted(5000)
        && process.waitForFinished(5000)
        && process.exitCode() == expectedExitCode
        && ((*standardOutput = process.readAllStandardOutput()), true)
        && ((*standardError = process.readAllStandardError()), true);
}

void check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "read fixture file");
    return file.readAll();
}

bool hasFinding(const QJsonObject& status, const QString& id)
{
    for (const QJsonValue& value : status.value(QStringLiteral("findings")).toArray()) {
        if (value.toObject().value(QStringLiteral("id")) == id) return true;
    }
    return false;
}

bool hasGap(const QJsonObject& package, const QString& id)
{
    for (const QJsonValue& value : package.value(QStringLiteral("gaps")).toArray()) {
        const QJsonObject gap = value.toObject();
        for (const QString& key : {QStringLiteral("id"), QStringLiteral("severity"), QStringLiteral("domain"), QStringLiteral("title"), QStringLiteral("explanation")}) {
            check(gap.value(key).isString(), "gap schema");
        }
        if (gap.value(QStringLiteral("id")) == id) return true;
    }
    return false;
}

// Runs `rmk hook <event>` from `directory` with the runtime's JSON on stdin.
QByteArray hook(const QString& executable, const QString& directory, const QString& event, const QByteArray& input,
    int expectedExit = 0, qint64* elapsedMs = nullptr)
{
    QProcess process;
    process.setWorkingDirectory(directory);
    QElapsedTimer timer;
    timer.start();
    process.start(executable, {QStringLiteral("hook"), event});
    check(process.waitForStarted(5000), "hook starts");
    process.write(input);
    process.closeWriteChannel();
    check(process.waitForFinished(10000), "hook finishes");
    if (elapsedMs) *elapsedMs = timer.elapsed();
    check(process.exitCode() == expectedExit, "hook exit code");
    return process.readAllStandardOutput();
}

void resumeContract(const QString& executable)
{
    QTemporaryDir fixture;
    check(fixture.isValid(), "fixture directory");
    const QString root = fixture.path();
    const QString repo = root + "/repo";
    const QString remote = root + "/remote.git";
    const QString configPath = root + "/.runmark/project.json";
    const auto git = [](const QStringList& args) {
        QByteArray out, err;
        check(run(QStringLiteral("git"), args, 0, &out, &err), "git fixture command");
        return QString::fromUtf8(out).trimmed();
    };
    const auto cli = [&](const QStringList& args) {
        QByteArray out, err;
        check(run(executable, QStringList{"--project", configPath} + args, 0, &out, &err), "CLI exit 0");
        check(err.isEmpty(), "CLI success stderr empty");
        const QJsonDocument doc = QJsonDocument::fromJson(out);
        check(doc.isObject(), "CLI stdout JSON object");
        return doc.object();
    };
    git({"init", "--bare", remote});
    git({"init", "-b", "main", repo});
    git({"-C", repo, "config", "user.name", "Runmark Contract"});
    git({"-C", repo, "config", "user.email", "test@example.invalid"});
    check(writeFile(repo + "/file.txt", "initial\n"), "initial file");
    git({"-C", repo, "add", "."});
    git({"-C", repo, "commit", "-m", "initial"});
    git({"-C", repo, "remote", "add", "origin", remote});
    git({"-C", repo, "push", "-u", "origin", "main"});
    check(QDir().mkpath(root + "/.runmark"), "state directory");
    check(writeFile(root + "/plan.md", "- [ ] MF-1\n- [ ] MF-2\n"), "plan");
    check(writeFile(root + "/AGENTS.md", "Project instructions\n"), "project instructions");
    check(writeFile(root + "/extra.md", "Extra instructions\n"), "extra instructions");
    QJsonObject config = QJsonDocument::fromJson(R"({"version":1,"name":"test","worktree_root":"worktrees","repos":[{"name":"repo","path":"repo","base":{"remote":"origin","branch":"main"}}],"plan":{"paths":["plan.md"]},"task_id_pattern":"MF-\\d+","instructions":["AGENTS.md"]})").object();
    check(writeFile(configPath, QJsonDocument(config).toJson()), "config");

    const QJsonObject none = cli({"resume", "MF-99"});
    check(none.value("exec").isNull() && hasGap(none, "context.no_execution"), "no execution gap");
    check(none.value("workspace").toObject().value("worktree_exists").isNull(), "no execution is unknown workspace");
    check(!QFile::exists(root + "/.runmark/ledger"), "resume creates no ledger");

    const QJsonObject started = cli({"start", "MF-1", "--agent", "claude", "--instruction", "extra.md", "--instruction", "missing.md"});
    check(started.value("warnings").toArray().size() == 1, "unreadable instruction warning");
    check(started.value("warnings").toArray().at(0).toObject().value("id") == "context.instruction_unreadable", "instruction warning id");
    const QString exec = started.value("exec").toString();
    const QString worktree = started.value("worktree").toString();
    const QString ledger = root + "/.runmark/ledger/" + exec + ".jsonl";
    const QJsonObject event = QJsonDocument::fromJson(readFile(ledger).trimmed()).object();
    const QJsonArray instructions = event.value("instructions").toArray();
    check(instructions.size() == 3, "project plus repeatable CLI instructions");
    check(instructions.at(0).toObject().value("name") == "AGENTS.md"
        && instructions.at(1).toObject().value("path") == root + "/extra.md"
        && instructions.at(2).toObject().value("sha1").isNull(), "instruction provenance schema");
    const auto hash = [](const QByteArray& content) { return QString::fromLatin1(QCryptographicHash::hash(content, QCryptographicHash::Sha1).toHex()); };
    check(instructions.at(0).toObject().value("sha1") == hash(readFile(root + "/AGENTS.md")), "instruction hash");
    check(writeFile(worktree + "/file.txt", "changed\n"), "worktree edit");
    git({"-C", worktree, "add", "."});
    git({"-C", worktree, "commit", "-m", "MF-1 implementation"});
    const QJsonObject active = cli({"resume", exec});
    check(active.value("measured").toObject().value("source") == "git"
        && active.value("measured").toObject().value("commits").toArray().size() == 1
        && active.value("measured").toObject().value("files_changed").toInt() == 1
        && hasGap(active, "context.no_handoff"), "active execution measured");
    cli({"evidence", exec, "--kind", "test", "--summary", "1 passed", "--ref", "test.log"});
    cli({"evidence", exec, "--kind", "command", "--summary", "build passed"});
    cli({"evidence", exec, "--kind", "agent_summary", "--summary", "CLAIM ONLY"});
    cli({"note", exec, "--kind", "unresolved", "--text", "No ref"});
    cli({"note", exec, "--kind", "unresolved", "--text", "Has ref", "--ref", "plan.md#L1"});
    check(writeFile(worktree + "/uncommitted.txt", "preserved\n"), "preserved file");
    const QJsonObject finished = cli({"finish", exec});
    // finish reports the worktree at the moment the decision is made, but the
    // command appears only when removing it is actually safe. Here the tree is
    // dirty, so no suggestion: handing one over would offer a way to lose work.
    const QJsonObject finishedWorktree = finished.value("worktree").toObject();
    check(finishedWorktree.value("path") == worktree && finishedWorktree.value("exists") == true
        && finishedWorktree.value("clean") == false
        && !finishedWorktree.contains("suggested_action"), "dirty worktree gets no removal suggestion");
    const QByteArray before = readFile(ledger);
    const QJsonObject package = cli({"resume", "MF-1"});
    check(package.value("exec") == exec && package.value("task") == "MF-1", "task selects execution");
    check(cli({"resume", exec}) == package, "exec selects same package");
    const QJsonObject measured = package.value("measured").toObject();
    check(measured.value("source") == "execution.finished" && measured.value("commits").toArray().size() == 1
        && measured.value("files_changed").toInt() == 1 && measured.value("evidence").toArray().isEmpty(), "finished measurements");
    check(!QJsonDocument(measured).toJson().contains("CLAIM ONLY"), "claims never measured");
    check(package.value("agent_claims").toObject().value("verification") == QStringLiteral("unverified")
        && package.value("agent_claims").toObject().value("evidence").toArray().size() == 3, "claims explicitly unverified");
    check(package.value("unresolved").toObject().value("with_ref").toArray().size() == 1
        && package.value("unresolved").toObject().value("without_ref").toArray().size() == 1, "unresolved refs separated");
    check(package.value("instructions") == instructions, "resume recorded instructions");
    check(package.value("preserved_ref").toString().startsWith("refs/runmark/preserved/"), "preserved ref");
    const QJsonObject handoff = package.value("handoff").toObject();
    const QString handoffPath = handoff.value("path").toString();
    check(handoff.value("sha1") == hash(readFile(handoffPath)) && handoff.value("verified") == true, "handoff production hash verified");
    check(handoff.value("content").toString().toUtf8() == readFile(handoffPath), "handoff content");
    check(package.value("plan_changed") == false && package.value("workspace").toObject().value("base_advanced") == false, "known unchanged");
    QByteArray markdown, stderrOutput;
    check(run(executable, {"--project", configPath, "resume", exec, "--markdown"}, 0, &markdown, &stderrOutput)
        && stderrOutput.isEmpty() && markdown.startsWith("# Runmark resume:")
        && markdown.contains(QStringLiteral("Agent note (weak evidence \u2014 unverified)").toUtf8()) && markdown.contains(handoff.value("sha1").toString().toUtf8()), "markdown contract");
    check(readFile(ledger) == before, "resume never appends delivery event");

    // A session-start hook records the session in its own file, not the
    // ledger. Without any session, a hook that never ran looks like a clean
    // project (regression: this replaced hook-observed.json).
    QJsonObject hooked = config;
    hooked.insert("hooks_expected", true);
    check(writeFile(configPath, QJsonDocument(hooked).toJson()), "hooks_expected config");
    check(hasFinding(cli({"status"}), "context.hooks_not_observed"), "hook blindness reported");
    const QByteArray context = hook(executable, root, "session-start",
        R"({"session_id":"s-start","cwd":")" + root.toUtf8() + R"(","transcript_path":"/x/.claude/projects/p/s-start.jsonl","source":"startup"})");
    check(context.startsWith("# Runmark resume:"), "session start hands the resume context to the agent");
    check(QFile::exists(root + "/.runmark/sessions/s-start.jsonl"), "session start records the session");
    check(readFile(ledger) == before, "session start appends no ledger event");
    check(!hasFinding(cli({"status"}), "context.hooks_not_observed"), "a recorded session clears the finding");
    check(writeFile(configPath, QJsonDocument(config).toJson()), "restore config");

    check(writeFile(handoffPath, "changed handoff\n"), "tamper handoff");
    check(hasGap(cli({"resume", exec}), "context.handoff_changed"), "tampered handoff gap");
    check(QFile::remove(handoffPath), "delete handoff");
    const QJsonObject missing = cli({"resume", exec});
    check(hasGap(missing, "context.no_handoff") && missing.value("handoff").toObject().value("sha1").isNull(), "deleted handoff gap and null hash");
    check(writeFile(root + "/plan.md", "- [ ] MF-1 changed\n"), "change plan");
    git({"-C", repo, "commit", "--allow-empty", "-m", "advance base"});
    git({"-C", repo, "push"});
    const QJsonObject advanced = cli({"resume", exec});
    check(hasGap(advanced, "git.base_advanced") && hasGap(advanced, "plan.changed_during_execution")
        && advanced.value("workspace").toObject().value("base_advanced") == true && advanced.value("plan_changed") == true, "base and plan changes");
    git({"-C", repo, "remote", "set-url", "origin", root + "/missing.git"});
    check(QFile::remove(root + "/plan.md"), "delete plan");
    const QJsonObject offline = cli({"resume", exec});
    check(hasGap(offline, "git.fetch_failed") && offline.value("workspace").toObject().value("base_advanced").isNull()
        && hasGap(offline, "plan.comparison_unknown") && offline.value("plan_changed").isNull(), "unknown differs from false");
    git({"-C", repo, "remote", "set-url", "origin", remote});
    git({"-C", repo, "worktree", "remove", "--force", worktree});
    const QJsonObject removed = cli({"resume", exec});
    check(hasGap(removed, "git.worktree_missing") && removed.value("workspace").toObject().value("worktree_exists") == false
        && removed.value("measured") == measured, "missing worktree keeps recorded measurements");
    check(readFile(ledger) == before, "all resume cases leave ledger unchanged");

    config.remove("instructions");
    check(writeFile(configPath, QJsonDocument(config).toJson()), "no instructions config");
    const QJsonObject second = cli({"start", "MF-2"});
    const QString secondExec = second.value("exec").toString();

    const QJsonValue emptyInstructions = cli({"resume", secondExec}).value("instructions");
    check(emptyInstructions.isArray() && emptyInstructions.toArray().isEmpty(), "empty instruction list");
    git({"-C", repo, "worktree", "remove", second.value("worktree").toString()});
    const QJsonObject missingActive = cli({"resume", secondExec});
    check(hasGap(missingActive, "git.measurement_unavailable") && missingActive.value("measured").toObject().value("commits").isNull(), "missing active measurements unknown");

    const QString adoptedPath = root + "/worktrees/MF-3";
    git({"-C", repo, "worktree", "add", "-b", "external/MF-3", adoptedPath, started.value("base_sha").toString()});
    const QJsonObject adopted = cli({"start", "MF-3"});
    const QJsonObject adoptedResume = cli({"resume", adopted.value("exec").toString()});
    const QJsonObject workspace = adoptedResume.value("workspace").toObject();
    check(workspace.value("base_sha") != workspace.value("remote_base_sha") && workspace.value("base_advanced") == false,
        "adopted merge-base age is not advancement since start");
    git({"-C", remote, "update-ref", "-d", "refs/heads/main"});
    const QJsonObject deletedBase = cli({"resume", adopted.value("exec").toString()});
    check(hasGap(deletedBase, "git.fetch_failed") && deletedBase.value("workspace").toObject().value("base_advanced").isNull(),
        "deleted remote branch is not unchanged stale tracking ref");
    git({"-C", repo, "push", "origin", "main"});
    const QString baseBeforeRewrite = git({"-C", repo, "rev-parse", "main"});
    git({"-C", remote, "update-ref", "refs/heads/main", started.value("base_sha").toString()});
    const QJsonObject rewritten = cli({"resume", adopted.value("exec").toString()});
    check(hasGap(rewritten, "git.base_unknown") && rewritten.value("workspace").toObject().value("base_advanced").isNull(), "rewritten base not forward advancement");
    git({"-C", remote, "update-ref", "refs/heads/main", baseBeforeRewrite});

    {
        // The safe case: an adopted worktree sitting on an ancestor of the base
        // with nothing written to it. Both checks pass, so the command appears.
        const QString idlePath = root + "/worktrees/MF-9";
        git({"-C", repo, "worktree", "add", "-b", "external/MF-9", idlePath, started.value("base_sha").toString()});
        const QJsonObject idleStart = cli({"start", "MF-9"});
        const QJsonObject idle = cli({"finish", idleStart.value("exec").toString(), "--outcome", "abandoned"})
            .value("worktree").toObject();
        check(idle.value("exists") == true && idle.value("clean") == true && idle.value("merged") == true
            && idle.value("suggested_action").toString() == QStringLiteral("git worktree remove ") + idlePath,
            "clean merged worktree is offered for removal");
    }

    // Synthetic legacy entries check selection independently of one-second IDs.
    const QString oldExec = "20000101T000000Z-MF-7";
    const QString newExec = "20000102T000000Z-MF-7";
    for (const QString& id : {oldExec, newExec}) {
        QJsonObject legacy = event;
        legacy.insert("exec", id);
        legacy.insert("task", "MF-7");
        legacy.remove("instructions");
        legacy.remove("remote_base_sha");
        legacy.insert("workspace_source", "adopted");
        check(writeFile(root + "/.runmark/ledger/" + id + ".jsonl", QJsonDocument(legacy).toJson(QJsonDocument::Compact) + '\n'), "legacy ledger");
    }
    check(cli({"resume", "MF-7"}).value("exec") == newExec && cli({"resume", oldExec}).value("exec") == oldExec, "latest task and exact exec priority");
    const QJsonObject legacy = cli({"resume", oldExec});
    check(legacy.value("instructions").isNull() && hasGap(legacy, "context.instructions_unknown")
        && legacy.value("workspace").toObject().value("base_advanced").isNull(), "legacy unknown provenance and adopted baseline");
    check(writeFile(root + "/.runmark/ledger/broken.jsonl", "not json\n"), "corrupt ledger");
    const QJsonObject corrupt = cli({"resume", "MF-99"});
    check(hasGap(corrupt, "context.ledger_unreadable") && !hasGap(corrupt, "context.no_execution"), "unreadable history not absent history");
    QTextStream(stdout) << "resume contract: git chain, no execution, missing/tampered handoff, missing worktree, offline, plan/base changes, provenance, legacy selection, ledger immutability passed\n";
}

QJsonObject lastEvent(const QString& ledger)
{
    const QList<QByteArray> lines = readFile(ledger).trimmed().split('\n');
    return QJsonDocument::fromJson(lines.last()).object();
}

QJsonObject runtimeEvidence(const QString& ledger)
{
    for (const QByteArray& line : readFile(ledger).trimmed().split('\n')) {
        const QJsonObject event = QJsonDocument::fromJson(line).object();
        if (event.value("source") == "runtime") return event;
    }
    return {};
}

// Test runs come from the agent runtime's own transcript, never from the
// agent's word: rmk evidence --kind test stays a claim (ADR-002).
void transcriptContract(const QString& executable)
{
    QTemporaryDir fixture;
    check(fixture.isValid(), "transcript fixture directory");
    const QString root = fixture.path();
    const QString repo = root + "/repo";
    const QString configPath = root + "/.runmark/project.json";
    const auto git = [](const QStringList& args) {
        QByteArray out, err;
        check(run(QStringLiteral("git"), args, 0, &out, &err), "transcript git fixture command");
    };
    const auto cli = [&](const QStringList& args) {
        QByteArray out, err;
        check(run(executable, QStringList{"--project", configPath} + args, 0, &out, &err), "transcript CLI exit 0");
        return QJsonDocument::fromJson(out).object();
    };
    git({"init", "--bare", root + "/remote.git"});
    git({"init", "-b", "main", repo});
    git({"-C", repo, "-c", "user.name=R", "-c", "user.email=r@example.invalid", "commit", "--allow-empty", "-m", "initial"});
    git({"-C", repo, "remote", "add", "origin", root + "/remote.git"});
    git({"-C", repo, "push", "-u", "origin", "main"});
    check(QDir().mkpath(root + "/.runmark"), "transcript state directory");
    check(writeFile(root + "/plan.md", "- [x] MF-1\n- [x] MF-2\n"), "transcript plan");
    check(writeFile(configPath, R"({"version":1,"name":"t","worktree_root":"worktrees","repos":[{"name":"repo","path":"repo","base":{"remote":"origin","branch":"main"}}],"plan":{"paths":["plan.md"]},"task_id_pattern":"MF-\\d+"})"), "transcript config");
    const QByteArray claudeHome = qgetenv("CLAUDE_CONFIG_DIR");
    const QByteArray codexHome = qgetenv("CODEX_HOME");

    // Claude: a failing ctest after start; a passing one before start must not count.
    qputenv("CLAUDE_CONFIG_DIR", (root + "/claude").toUtf8());
    qputenv("CLAUDE_CODE_SESSION_ID", "session-1");
    const QString exec = cli({"start", "MF-1", "--agent", "claude"}).value("exec").toString();
    const QString ledger = root + "/.runmark/ledger/" + exec + ".jsonl";
    check(lastEvent(ledger).value("session_id") == "session-1", "start records the runtime session");
    // One wall-clock snapshot can predate later executions on a slow runner.
    const QByteArray claudeAt = lastEvent(ledger).value("ts").toString().toUtf8();
    check(QDir().mkpath(root + "/claude/projects/-encoded"), "claude projects directory");
    check(writeFile(root + "/claude/projects/-encoded/session-1.jsonl",
        "{\"timestamp\":\"" + claudeAt + "\",\"message\":{\"content\":[{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":\"Bash\",\"input\":{\"command\":\"cd w && ctest --preset dev\"}}]}}\n"
        "{\"timestamp\":\"" + claudeAt + "\",\"message\":{\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"t1\",\"is_error\":true,\"content\":\"Exit code 8\\nfailed\"}]}}\n"
        "{\"timestamp\":\"" + claudeAt + "\",\"message\":{\"content\":[{\"type\":\"tool_use\",\"id\":\"t2\",\"name\":\"Bash\",\"input\":{\"command\":\"ls\"}}]}}\n"
        "{\"timestamp\":\"" + claudeAt + "\",\"message\":{\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"t2\",\"content\":\"file\"}]}}\n"
        "{\"timestamp\":\"2000-01-01T00:00:00.000Z\",\"message\":{\"content\":[{\"type\":\"tool_use\",\"id\":\"t0\",\"name\":\"Bash\",\"input\":{\"command\":\"ctest\"}}]}}\n"
        "{\"timestamp\":\"2000-01-01T00:00:00.000Z\",\"message\":{\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"t0\",\"content\":\"ok\"}]}}\n"),
        "claude transcript");
    cli({"evidence", exec, "--kind", "test", "--summary", "all green"});
    cli({"finish", exec});
    const QJsonObject tested = runtimeEvidence(ledger);
    check(tested.value("kind") == "test" && tested.value("exit_code") == 8
        && tested.value("summary").toString().contains("ctest --preset dev"), "failing runtime test recorded");
    check(lastEvent(ledger).value("transcript").toObject().value("test_runs") == 1, "only runs after start count");
    QJsonObject status = cli({"status"});
    check(hasFinding(status, "context.last_test_failed"), "failing last test reported");
    const QJsonObject package = cli({"resume", exec});
    const QJsonArray measured = package.value("measured").toObject().value("evidence").toArray();
    check(measured.size() == 1 && measured.at(0).toObject().value("source") == "runtime", "only runtime evidence is measured");
    check(QJsonDocument(package.value("agent_claims").toObject()).toJson().contains("all green"), "agent test result stays a claim");
    check(readFile(root + "/.runmark/handoffs/" + exec + ".md").contains("Tests (from the claude transcript)"), "handoff shows runtime tests");

    // A session whose transcript cannot be found is unknown, not clean.
    qputenv("CLAUDE_CODE_SESSION_ID", "session-missing");
    const QString missing = cli({"start", "MF-2", "--agent", "claude"}).value("exec").toString();
    cli({"evidence", missing, "--kind", "test", "--summary", "trust me"});
    cli({"finish", missing});
    status = cli({"status"});
    check(hasFinding(status, "context.transcript_unavailable"), "unreadable transcript reported");
    check(hasFinding(status, "plan.test_claim_unverified"), "claim-only test on a done task reported");

    // Codex: the rollout records the exit code as a field.
    qputenv("CODEX_HOME", (root + "/codex").toUtf8());
    qputenv("CODEX_THREAD_ID", "thread-1");
    const QString codex = cli({"start", "MF-3", "--agent", "codex"}).value("exec").toString();
    const QByteArray codexAt = lastEvent(root + "/.runmark/ledger/" + codex + ".jsonl").value("ts").toString().toUtf8();
    check(QDir().mkpath(root + "/codex/sessions/2026/09/25"), "codex sessions directory");
    check(writeFile(root + "/codex/sessions/2026/09/25/rollout-2026-09-25T00-00-00-thread-1.jsonl",
        "{\"timestamp\":\"" + codexAt + "\",\"type\":\"event_msg\",\"payload\":{\"type\":\"item_completed\",\"item\":{\"type\":\"CommandExecution\",\"command\":[\"/bin/zsh\",\"-lc\",\"pytest -q\"],\"exit_code\":0}}}\n"),
        "codex transcript");
    cli({"finish", codex});
    const QJsonObject codexTested = runtimeEvidence(root + "/.runmark/ledger/" + codex + ".jsonl");
    check(codexTested.value("exit_code") == 0 && codexTested.value("runtime") == "codex"
        && codexTested.value("summary").toString().contains("pytest -q"), "codex runtime test recorded");

    // Piped into tail, a failing suite exits 0: the result is unknown, never a pass.
    qputenv("CODEX_THREAD_ID", "thread-2");
    const QString piped = cli({"start", "MF-4", "--agent", "codex"}).value("exec").toString();
    const QByteArray pipedAt = lastEvent(root + "/.runmark/ledger/" + piped + ".jsonl").value("ts").toString().toUtf8();
    check(writeFile(root + "/codex/sessions/2026/09/25/rollout-2026-09-25T00-00-01-thread-2.jsonl",
        "{\"timestamp\":\"" + pipedAt + "\",\"type\":\"event_msg\",\"payload\":{\"type\":\"item_completed\",\"item\":{\"type\":\"CommandExecution\",\"command\":[\"/bin/zsh\",\"-lc\",\"ctest --preset dev 2>&1 | tail -5\"],\"exit_code\":0}}}\n"),
        "piped transcript");
    cli({"finish", piped});
    check(runtimeEvidence(root + "/.runmark/ledger/" + piped + ".jsonl").value("exit_code").isNull(), "piped exit code recorded as unknown");
    check(hasFinding(cli({"status"}), "context.test_result_unknown"), "unknown test result reported");

    for (const char* name : {"CLAUDE_CODE_SESSION_ID", "CODEX_THREAD_ID"}) qunsetenv(name);
    qputenv("CLAUDE_CONFIG_DIR", claudeHome);
    qputenv("CODEX_HOME", codexHome);
}

// Runs rmk without --project from `directory`; returns the exit code.
int runIn(const QString& executable, const QStringList& arguments, const QString& directory, QByteArray* output)
{
    QProcess process;
    process.setWorkingDirectory(directory);
    process.start(executable, arguments);
    check(process.waitForStarted(5000) && process.waitForFinished(10000), "rmk runs");
    *output = process.readAllStandardOutput();
    return process.exitCode();
}

// Agents work in worktrees outside the project root and in subdirectories;
// every command must still find the project without --project.
void discoveryContract(const QString& executable)
{
    QTemporaryDir fixture, outside, elsewhere, configHome;
    check(fixture.isValid() && outside.isValid() && elsewhere.isValid() && configHome.isValid(), "discovery fixtures");
    const QString root = QFileInfo(fixture.path()).canonicalFilePath();
    const QString worktrees = QFileInfo(outside.path()).canonicalFilePath();
    const auto git = [](const QStringList& args) {
        QByteArray out, err;
        check(run(QStringLiteral("git"), args, 0, &out, &err), "discovery git fixture command");
    };
    git({"init", "--bare", root + "/remote.git"});
    git({"init", "-b", "main", root + "/repo"});
    git({"-C", root + "/repo", "-c", "user.name=R", "-c", "user.email=r@example.invalid", "commit", "--allow-empty", "-m", "initial"});
    git({"-C", root + "/repo", "remote", "add", "origin", root + "/remote.git"});
    git({"-C", root + "/repo", "push", "-u", "origin", "main"});
    const QString project = root + "/repo";
    check(QDir().mkpath(project + "/.runmark") && QDir().mkpath(project + "/src/deep"), "discovery layout");
    check(writeFile(project + "/plan.md", "- [ ] MF-1\n"), "discovery plan");
    check(writeFile(project + "/.runmark/project.json", QJsonDocument(QJsonObject{
        {"version", 1}, {"name", "found"}, {"worktree_root", worktrees},
        {"repos", QJsonArray{QJsonObject{{"name", "repo"}, {"path", "."}, {"base", QJsonObject{{"remote", "origin"}, {"branch", "main"}}}}}},
        {"plan", QJsonObject{{"paths", QJsonArray{"plan.md"}}}}, {"task_id_pattern", "MF-\\d+"}}).toJson()), "discovery config");
    qputenv("RUNMARK_CONFIG_HOME", configHome.path().toUtf8());

    QByteArray output;
    check(runIn(executable, {"status"}, project + "/src/deep", &output) == 0
        && QJsonDocument::fromJson(output).object().value("project") == "found", "found from a subdirectory");
    check(runIn(executable, {"start", "MF-1", "--agent", "claude"}, project, &output) == 0, "start creates an outside worktree");
    const QString worktree = QJsonDocument::fromJson(output).object().value("worktree").toString();
    check(worktree.startsWith(worktrees), "worktree lies outside the project root");
    check(runIn(executable, {"status"}, worktree, &output) == 0
        && QJsonDocument::fromJson(output).object().value("project") == "found", "found from a worktree via its main checkout");
    check(runIn(executable, {"status"}, elsewhere.path(), &output) == 1, "an unrelated directory finds nothing");

    // A registered project claims directories under its worktree root even
    // when git cannot lead there (the worktree was moved or is not a checkout).
    const QString loose = worktrees + "/loose";
    check(QDir().mkpath(loose), "loose directory");
    check(runIn(executable, {"status"}, loose, &output) == 1, "unregistered loose directory finds nothing");
    check(writeFile(configHome.path() + "/projects.json",
        QJsonDocument(QJsonObject{{"projects", QJsonArray{project + "/.runmark/project.json"}}}).toJson()), "project list");
    check(runIn(executable, {"status"}, loose, &output) == 0
        && QJsonDocument::fromJson(output).object().value("project") == "found", "found through the project list");
    qunsetenv("RUNMARK_CONFIG_HOME");

    // Several plan files, one of them a glob: progress per file, and a glob
    // matching nothing is reported rather than read as an empty plan.
    check(QDir().mkpath(project + "/plans"), "plans directory");
    check(writeFile(project + "/plans/a.md", "- [x] step one\n- [ ] step two\n"), "plan a");
    check(writeFile(project + "/plans/b.md", "- [x] only step\n"), "plan b");
    QJsonObject config = QJsonDocument::fromJson(readFile(project + "/.runmark/project.json")).object();
    config.insert("plan", QJsonObject{{"paths", QJsonArray{"plan.md", "plans/*.md", "none/*.md"}}});
    check(writeFile(project + "/.runmark/project.json", QJsonDocument(config).toJson()), "multi plan config");
    check(runIn(executable, {"status"}, project, &output) == 0, "status with several plans");
    const QJsonObject status = QJsonDocument::fromJson(output).object();
    QStringList progress;
    for (const QJsonValue& plan : status.value("plans").toArray()) {
        progress.append(plan.toObject().value("path").toString() + "=" + QString::number(plan.toObject().value("done").toInt())
            + "/" + QString::number(plan.toObject().value("total").toInt()));
    }
    check(progress == QStringList{"plan.md=0/1", "plans/a.md=1/2", "plans/b.md=1/1"}, "per-file progress in path order");
    check(hasFinding(status, "plan.unreadable"), "a glob matching nothing is reported");
}

// Session hooks (Phase 0a): one file per session, a one-time note reminder
// after each new commit, never a block loop, notes without an execution, and
// the next session told when an earlier one left decisions unrecorded.
void sessionContract(const QString& executable)
{
    QTemporaryDir fixture;
    check(fixture.isValid(), "session fixture");
    const QString root = QFileInfo(fixture.path()).canonicalFilePath();
    const auto git = [&](const QStringList& args) {
        QByteArray out, err;
        check(run(QStringLiteral("git"), QStringList{"-C", root} + args, 0, &out, &err), "session git command");
    };
    QByteArray out, err;
    check(run(QStringLiteral("git"), {"init", "-b", "main", root}, 0, &out, &err), "session repo");
    const auto commit = [&](const QString& message) {
        git({"-c", "user.name=R", "-c", "user.email=r@example.invalid", "commit", "--allow-empty", "-m", message});
    };
    commit("initial");
    check(QDir().mkpath(root + "/.runmark"), "session state");
    check(writeFile(root + "/plan.md", "- [ ] MF-1\n"), "session plan");
    check(writeFile(root + "/.runmark/project.json", R"({"version":1,"name":"s","worktree_root":"wt","repos":[{"name":"r","path":".","base":{"remote":"origin","branch":"main"}}],"plan":{"paths":["plan.md"]},"task_id_pattern":"MF-\\d+"})"), "session config");
    const auto input = [&](const QString& id, const QByteArray& extra = {}) {
        return R"({"session_id":")" + id.toUtf8() + R"(","cwd":")" + root.toUtf8()
            + R"(","transcript_path":"/x/.codex/sessions/2026/09/26/rollout-)" + id.toUtf8() + R"(.jsonl")" + extra + "}";
    };
    const auto session = [&](const QString& id) {
        for (const QJsonValue& value : QJsonDocument::fromJson(
                [&] { QByteArray o, e; run(executable, {"--project", root + "/.runmark/project.json", "status"}, 0, &o, &e); return o; }())
                .object().value("sessions").toArray()) {
            if (value.toObject().value("id") == id) return value.toObject();
        }
        return QJsonObject{};
    };

    hook(executable, root, "session-start", input("s1", R"(,"source":"startup")"));
    // Checked before any other command runs: status would add the exclude
    // itself and hide a hook that does not.
    {
        QByteArray porcelain, error;
        check(run(QStringLiteral("git"), {"-C", root, "status", "--porcelain", "--", ".runmark/sessions"}, 0, &porcelain, &error)
            && porcelain.isEmpty(), "session files stay out of git status");
    }
    check(session("s1").value("runtime") == "codex" && session("s1").value("state") == "started", "runtime from the transcript path");
    hook(executable, root, "prompt-submit", input("s1"));
    check(session("s1").value("state") == "working", "a prompt means the agent works");
    check(hook(executable, root, "stop", input("s1")).isEmpty(), "no commit, no reminder");
    check(session("s1").value("state") == "waiting", "a finished reply means the agent waits");

    commit("work one");
    // /compact re-runs session start in the same session (measured, Claude
    // Code 2.1.282): context again, and the baseline stays the first start's,
    // so the commit above is still asked about.
    check(!hook(executable, root, "session-start", input("s1", R"(,"source":"compact")")).isEmpty(), "compact hands the context again");
    qint64 elapsed = 0;
    const QByteArray block = hook(executable, root, "stop", input("s1"), 0, &elapsed);
    const QJsonObject decision = QJsonDocument::fromJson(block).object();
    check(decision.value("decision") == "block" && decision.value("reason").toString().contains("rmk note"), "a new commit without a note asks once");
    QTextStream(stdout) << "stop hook with reminder: " << elapsed << " ms\n";
    check(elapsed < 100, "stop hook stays within the 100 ms budget (eng review D9)");
    check(hook(executable, root, "stop", input("s1")).isEmpty(), "the same commit is not asked twice");

    commit("work two");
    check(hook(executable, root, "stop", input("s1", R"(,"stop_hook_active":true)")).isEmpty(), "never blocks while a stop hook is active");
    qputenv("CLAUDE_CODE_SESSION_ID", "s1");
    check(run(executable, {"--project", root + "/.runmark/project.json", "note", "--kind", "decision", "--text", "kept the old API"}, 0, &out, &err),
        "a note without an execution goes to the session");
    qunsetenv("CLAUDE_CODE_SESSION_ID");
    check(hook(executable, root, "stop", input("s1")).isEmpty(), "a note after the commit satisfies the reminder");
    check(session("s1").value("notes") == 1, "the session holds its note");
    check(run(executable, {"--project", root + "/.runmark/project.json", "note", "--kind", "decision", "--text", "x"}, 1, &out, &err),
        "outside a session a note still needs an execution");
    hook(executable, root, "session-end", input("s1", R"(,"reason":"other")"));
    check(session("s1").value("state") == "ended" && session("s1").value("end_reason") == "other", "session end recorded");

    // s2 commits, is reminded, never notes, ends: the next session hears of it.
    hook(executable, root, "session-start", input("s2"));
    commit("work three");
    check(!hook(executable, root, "stop", input("s2")).isEmpty(), "s2 is reminded");
    hook(executable, root, "session-end", input("s2", R"(,"reason":"other")"));
    const QByteArray next = hook(executable, root, "session-start", input("s3"));
    check(next.contains("## Unrecorded decisions") && next.contains("s2"), "the next session is told what went unrecorded");
    check(!next.contains("(s1)"), "a session that noted its decision is not reported");
    check(next.contains("## Notes from the last session") && next.contains("session s1") && next.contains("[decision] kept the old API"),
        "the next session sees the last session's notes");
    check(next.startsWith("## Notes from the last session"), "notes newer than any execution come first");

    hook(executable, root, "session-end", input("s3", R"(,"reason":"other")"));

    // Conflict radar: two live sessions in one checkout change the same file.
    check(writeFile(root + "/shared.txt", "v1\n"), "shared file");
    git({"add", "shared.txt"});
    commit("add shared");
    hook(executable, root, "session-start", input("r1"));
    hook(executable, root, "session-start", input("r2"));
    const auto status = [&] { QByteArray o, e; run(executable, {"--project", root + "/.runmark/project.json", "status"}, 0, &o, &e); return QJsonDocument::fromJson(o).object(); };
    check(!hasFinding(status(), "context.session_conflict"), "no shared change, no conflict");
    check(writeFile(root + "/shared.txt", "v2\n"), "edit shared file");
    check(hasFinding(status(), "context.session_conflict"), "two live sessions changing one file conflict");
    {
        QString explanation;
        for (const QJsonValue& value : status().value("findings").toArray()) {
            if (value.toObject().value("id") == "context.session_conflict") explanation = value.toObject().value("explanation").toString();
        }
        check(explanation.contains("share the checkout") && !explanation.contains("both touch"),
            "one checkout cannot tell whose change it is (RM-14 #5)");
    }
    // A commit that lands while a session waits is another session's: r1's
    // next turn starts after it, so r1 is not asked about it (RM-14 #5).
    hook(executable, root, "prompt-submit", input("r1"));
    hook(executable, root, "stop", input("r1"));
    hook(executable, root, "prompt-submit", input("r2"));
    check(writeFile(root + "/shared.txt", "v2\n"), "r2 edits");
    git({"add", "shared.txt"});
    commit("r2 work");
    check(!hook(executable, root, "stop", input("r2")).isEmpty(), "the committing session is asked for a note");
    hook(executable, root, "prompt-submit", input("r1"));
    check(hook(executable, root, "stop", input("r1")).isEmpty(), "a session that waited through another's commit is not");
    hook(executable, root, "session-end", input("r2", R"(,"reason":"other")"));
    check(!hasFinding(status(), "context.session_conflict"), "an ended session no longer conflicts");

    // Sessions whose hooks never ran are found through their transcripts.
    QTemporaryDir runtimes;
    const QByteArray claudeHome = qgetenv("CLAUDE_CONFIG_DIR");
    const QByteArray codexHome = qgetenv("CODEX_HOME");
    qputenv("CLAUDE_CONFIG_DIR", (runtimes.path() + "/claude").toUtf8());
    qputenv("CODEX_HOME", (runtimes.path() + "/codex").toUtf8());
    check(QDir().mkpath(runtimes.path() + "/claude/projects/-p") && QDir().mkpath(runtimes.path() + "/codex/sessions/2026/09/26"), "runtime homes");
    const auto claudeTranscript = [&](const QString& id, const QString& cwd) {
        check(writeFile(runtimes.path() + "/claude/projects/-p/" + id + ".jsonl",
            "{\"type\":\"queue-operation\"}\n{\"type\":\"attachment\",\"cwd\":\"" + cwd.toUtf8() + "\",\"sessionId\":\"" + id.toUtf8() + "\"}\n"), "claude transcript");
    };
    const auto codexRollout = [&](const QString& id, const QByteArray& extra) {
        check(writeFile(runtimes.path() + "/codex/sessions/2026/09/26/rollout-2026-09-26T00-00-00-" + id + ".jsonl",
            R"({"type":"session_meta","payload":{"id":")" + id.toUtf8() + R"(","cwd":")" + root.toUtf8() + "\"" + extra + "}}\n"), "codex rollout");
    };
    claudeTranscript("ghost-claude", root + "/src");
    claudeTranscript("s1", root);                     // recorded by its hooks
    claudeTranscript("elsewhere", "/");                // not this project
    codexRollout("ghost-codex", "");
    codexRollout("sub-thread", R"(,"parent_thread_id":"ghost-codex")");
    QJsonObject expecting = QJsonDocument::fromJson(readFile(root + "/.runmark/project.json")).object();
    check(!hasFinding(status(), "context.unregistered_session"), "without hooks_expected it stays quiet");
    expecting.insert("hooks_expected", true);
    check(writeFile(root + "/.runmark/project.json", QJsonDocument(expecting).toJson()), "hooks expected");
    QString unregistered;
    for (const QJsonValue& value : status().value("findings").toArray()) {
        if (value.toObject().value("id") == "context.unregistered_session") unregistered = value.toObject().value("explanation").toString();
    }
    check(unregistered.contains("ghost-claude") && unregistered.contains("ghost-codex") && unregistered.startsWith("2 "),
        "sessions seen only in transcripts are reported");
    check(!unregistered.contains("(claude), s1") && !unregistered.contains("sub-thread") && !unregistered.contains("elsewhere"),
        "recorded sessions, sub-threads and other projects are not");
    qputenv("CLAUDE_CONFIG_DIR", claudeHome);
    qputenv("CODEX_HOME", codexHome);

    hook(executable, root, "session-start", R"({"session_id":"../escape","cwd":"/"})", 1);
    check(!QFile::exists(root + "/.runmark/escape.jsonl") && !QFile::exists(root + "/.runmark/sessions/../escape.jsonl"), "unsafe session id rejected");
}

} // namespace

// Parallel and paused work (RM-14): the next start of an interrupted task
// continues it, a start of a still-open task points at it, and session start
// lists every open execution, not only the latest one.
void continuationContract(const QString& executable)
{
    QTemporaryDir fixture;
    check(fixture.isValid(), "continuation fixture");
    const QString root = QFileInfo(fixture.path()).canonicalFilePath();
    const QString project = root + "/repo";
    const auto git = [](const QStringList& args) {
        QByteArray out, err;
        check(run(QStringLiteral("git"), args, 0, &out, &err), "continuation git command");
        return QString::fromUtf8(out);
    };
    git({"init", "--bare", root + "/remote.git"});
    git({"init", "-b", "main", project});
    git({"-C", project, "-c", "user.name=R", "-c", "user.email=r@example.invalid", "commit", "--allow-empty", "-m", "initial"});
    git({"-C", project, "remote", "add", "origin", root + "/remote.git"});
    git({"-C", project, "push", "-u", "origin", "main"});
    check(QDir().mkpath(project + "/.runmark") && writeFile(project + "/plan.md", "- [ ] MF-1\n- [ ] MF-2\n"), "continuation layout");
    check(writeFile(project + "/.runmark/project.json", R"({"version":1,"name":"c","worktree_root":")" + root.toUtf8()
        + R"(/wt","repos":[{"name":"r","path":".","base":{"remote":"origin","branch":"main"}}],"plan":{"paths":["plan.md"]},"task_id_pattern":"MF-\\d+"})"), "continuation config");
    const auto cli = [&](const QStringList& args, int exitCode = 0, QByteArray* error = nullptr) {
        QByteArray out, err;
        check(run(executable, QStringList{"--project", project + "/.runmark/project.json"} + args, exitCode, &out, &err), "continuation CLI exit");
        if (error) *error = err;
        return QJsonDocument::fromJson(out).object();
    };

    const QJsonObject first = cli({"start", "MF-1"});
    const QString worktree = first.value("worktree").toString();
    git({"-C", worktree, "-c", "user.name=R", "-c", "user.email=r@example.invalid", "commit", "--allow-empty", "-m", "half done"});
    cli({"finish", first.value("exec").toString(), "--outcome", "interrupted"});
    const QJsonObject paused = cli({"status"});
    check(hasFinding(paused, "context.interrupted_execution"), "an interrupted execution waits to be continued");
    const QJsonArray openWork = paused.value("open_work").toArray();
    check(openWork.size() == 1 && openWork.at(0).toObject().value("task") == "MF-1"
        && openWork.at(0).toObject().value("outcome") == "interrupted"
        && openWork.at(0).toObject().value("worktree") == worktree, "status lists open work for the plugin");
    check(!hasFinding(paused, "git.orphaned_worktree"), "its worktree is the work, not left behind");

    QThread::sleep(1);  // execution IDs have one-second resolution
    const QString second = cli({"start", "MF-2"}).value("exec").toString();
    const QByteArray context = hook(executable, project, "session-start", R"({"session_id":"c1","cwd":")" + project.toUtf8() + R"("})");
    check(context.startsWith("## Open work") && context.contains("- MF-1: " + first.value("exec").toString().toUtf8() + " interrupted")
        && !context.contains("- MF-2:"), "session start lists open work besides the resumed execution");
    {
        QByteArray line, error;
        check(run(executable, {"--project", project + "/.runmark/project.json", "status", "--line"}, 0, &line, &error)
            && line == "0/2 · 2 open · 1 warn\n", "one short line for a sidebar (the untracked plan leaves the checkout dirty)");
        // It runs on every agent turn: an unreachable remote must not show.
        git({"-C", project, "remote", "set-url", "origin", root + "/missing.git"});
        check(run(executable, {"--project", project + "/.runmark/project.json", "status", "--line"}, 0, &line, &error)
            && line == "0/2 · 2 open · 1 warn\n", "the line never fetches");
        check(hasFinding(cli({"status"}), "git.fetch_failed"), "full status still fetches");
        git({"-C", project, "remote", "set-url", "origin", root + "/remote.git"});
    }
    QByteArray refusal;
    cli({"start", "MF-2"}, 1, &refusal);
    check(refusal.contains("rmk resume " + second.toUtf8()), "a still-open task is pointed at, not restarted");

    const QJsonObject continued = cli({"start", "MF-1"});
    check(continued.value("workspace_source") == "adopted" && continued.value("worktree") == worktree
        && continued.value("branch") == "task/MF-1" && git({"-C", worktree, "log", "--format=%s"}).contains("half done"),
        "the next start continues the interrupted work");
    check(!hasFinding(cli({"status"}), "context.interrupted_execution"), "a continued execution no longer waits");

    cli({"finish", continued.value("exec").toString(), "--outcome", "interrupted"});
    git({"-C", project, "worktree", "remove", worktree});
    QThread::sleep(1);
    const QJsonObject recreated = cli({"start", "MF-1"});
    check(recreated.value("workspace_source") == "adopted" && git({"-C", worktree, "log", "--format=%s"}).contains("half done"),
        "a removed worktree is recreated from the interrupted branch");
    QTextStream(stdout) << "continuation contract: interrupted continue, open refusal, open work listing passed\n";
}

// rmk init: a new project from the terminal, registered for discovery; an
// existing one is only registered, never rewritten.
void initContract(const QString& executable)
{
    QTemporaryDir fixture;
    check(fixture.isValid(), "init fixture");
    const QString root = QFileInfo(fixture.path()).canonicalFilePath();
    const QString project = root + "/notes-api";
    const auto git = [](const QStringList& args) {
        QByteArray out, err;
        check(run(QStringLiteral("git"), args, 0, &out, &err), "init git command");
    };
    git({"init", "--bare", root + "/remote.git"});
    git({"init", "-b", "main", project});
    git({"-C", project, "remote", "add", "origin", root + "/remote.git"});
    qputenv("RUNMARK_CONFIG_HOME", (root + "/config").toUtf8());
    const auto registered = [&] {
        return QJsonDocument::fromJson(readFile(root + "/config/projects.json")).object().value("projects").toArray();
    };

    QByteArray output;
    check(runIn(executable, {"init"}, project, &output) == 2, "a new project needs a task prefix");
    check(runIn(executable, {"init", "--prefix", "NA"}, project, &output) == 0
        && QJsonDocument::fromJson(output).object().value("created") == true, "init creates a project");
    check(QFile::exists(project + "/PLAN.md") && QFile::exists(project + "/.runmark/project.json"), "init creates the plan and config");
    check(registered() == QJsonArray{project + "/.runmark/project.json"}, "init registers the project");
    const QByteArray config = readFile(project + "/.runmark/project.json");
    check(runIn(executable, {"init", "--prefix", "XX"}, project, &output) == 0
        && QJsonDocument::fromJson(output).object().value("created") == false, "init on an existing project only registers");
    check(readFile(project + "/.runmark/project.json") == config && registered().size() == 1, "an existing project is never rewritten or listed twice");
    qunsetenv("RUNMARK_CONFIG_HOME");
    QTextStream(stdout) << "init contract: create, register, idempotent passed\n";
}

int main(int argc, char* argv[])
{
    if (argc != 2) return 1;
    const QString executable = QString::fromLocal8Bit(argv[1]);
    QByteArray standardOutput;
    QByteArray standardError;

    if (!run(executable, {QStringLiteral("nonsense")}, 2, &standardOutput, &standardError)
            || !standardOutput.isEmpty()
            || !hasError(standardError, QStringLiteral("usage"))) return 1;

    if (!run(executable, {QStringLiteral("--project"), QStringLiteral("/does/not/exist/project.json"), QStringLiteral("status")}, 1, &standardOutput, &standardError)
            || !standardOutput.isEmpty()
            || !hasError(standardError, QStringLiteral("runtime"))) return 1;

    QTemporaryDir directory;
    if (!directory.isValid()
            || !writeFile(directory.filePath(QStringLiteral("project.json")), R"({"version":1,"name":"test","worktree_root":"worktrees","repos":[{"name":"repo","path":".","base":{"remote":"origin","branch":"main"}}],"plan":{"paths":["plan.md"]},"task_id_pattern":"MF-\\d+"})")) return 1;
    if (!run(executable, {QStringLiteral("--project"), directory.filePath(QStringLiteral("project.json")), QStringLiteral("inspect")}, 0, &standardOutput, &standardError)
            || !standardError.isEmpty()
            || !QJsonDocument::fromJson(standardOutput).isObject()) return 1;

    // The suite may itself run inside an agent session; its ids must not
    // point finish at a real transcript, and status must not scan the real
    // runtime homes for unregistered sessions.
    qunsetenv("CLAUDE_CODE_SESSION_ID");
    qunsetenv("CODEX_THREAD_ID");
    QTemporaryDir isolated;
    qputenv("CLAUDE_CONFIG_DIR", (isolated.path() + "/claude").toUtf8());
    qputenv("CODEX_HOME", (isolated.path() + "/codex").toUtf8());
    try {
        resumeContract(executable);
        transcriptContract(executable);
        discoveryContract(executable);
        sessionContract(executable);
        continuationContract(executable);
        initContract(executable);
    } catch (const std::exception& error) {
        QTextStream(stderr) << error.what() << '\n';
        return 1;
    }
    return 0;
}
