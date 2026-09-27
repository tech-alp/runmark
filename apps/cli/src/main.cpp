#include "json.h"

#include "runmark/project_config.h"
#include "runmark/version.h"
#include "runmark/workflow.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

#include <exception>

namespace {

// stdout = result, stderr = error. Both JSON; the signal is the exit code.
int emitError(const QString& code, const QString& message, int exitCode)
{
    const QJsonObject payload{{QStringLiteral("error"), QJsonObject{
        {QStringLiteral("code"), code},
        {QStringLiteral("message"), message},
    }}};
    QTextStream(stderr) << QJsonDocument(payload).toJson(QJsonDocument::Indented);
    return exitCode;
}

// `rmk hook <event>`: the agent runtime's hook JSON arrives on stdin. The
// plugin script only checks the rmk version and forwards; every decision is
// made here so it is covered by the contract tests (eng review D7).
int runHook(const QString& configPath, const QString& event)
{
    QFile input;
    if (!input.open(stdin, QIODevice::ReadOnly)) return emitError(QStringLiteral("runtime"), QStringLiteral("Cannot read hook input"), 1);
    const QJsonObject json = QJsonDocument::fromJson(input.readAll()).object();
    runmark::HookInput hook;
    hook.sessionId = json.value(QStringLiteral("session_id")).toString();
    hook.cwd = json.value(QStringLiteral("cwd")).toString();
    hook.transcriptPath = json.value(QStringLiteral("transcript_path")).toString();
    hook.source = json.value(QStringLiteral("source")).toString();
    hook.reason = json.value(QStringLiteral("reason")).toString();
    hook.stopHookActive = json.value(QStringLiteral("stop_hook_active")).toBool();
    if (!runmark::isSessionId(hook.sessionId)) {
        return emitError(QStringLiteral("runtime"), QStringLiteral("Hook input has no usable session_id"), 1);
    }
    QTextStream out(stdout);
    if (event == QLatin1String("session-start")) {
        const runmark::SessionStartResult started = runmark::sessionStarted(configPath, hook);
        // Plain stdout is taken as context by both runtimes.
        const runmark::ResumeResult resume = runmark::resumeExecution(configPath, QString());
        QVector<runmark::OpenExecution> otherOpen = started.openWork;
        otherOpen.removeIf([&resume](const runmark::OpenExecution& open) { return open.started.exec == resume.facts.exec; });
        QStringList sections{openWorkMarkdown(otherOpen), runmark::resumeMarkdown(toJson(resume))};
        if (started.lastWithNotes) {
            // Notes newer than the latest execution are the current state; a
            // days-old execution on top buried them (RM-14).
            bool notesFirst = false;
            for (const runmark::NoteRecorded& note : started.lastNotes) {
                notesFirst = notesFirst || !resume.facts.lastActivity.isValid() || note.at > resume.facts.lastActivity;
            }
            sections.insert(notesFirst ? 1 : 2, sessionNotesMarkdown(*started.lastWithNotes, started.lastNotes));
        }
        if (started.previousWithoutNotes) sections << sessionWithoutNotesMarkdown(*started.previousWithoutNotes);
        sections.removeAll(QString());
        for (QString& section : sections) section = section.trimmed();
        out << sections.join(QStringLiteral("\n\n")) << '\n';
    } else if (event == QLatin1String("prompt-submit")) {
        runmark::sessionWorking(configPath, hook);
    } else if (event == QLatin1String("stop")) {
        if (const std::optional<QString> reason = runmark::sessionStopped(configPath, hook)) {
            out << QJsonDocument(QJsonObject{{QStringLiteral("decision"), QStringLiteral("block")},
                {QStringLiteral("reason"), *reason}}).toJson(QJsonDocument::Compact) << '\n';
        }
    } else if (event == QLatin1String("session-end")) {
        runmark::sessionEnded(configPath, hook);
    } else {
        return emitError(QStringLiteral("usage"), QStringLiteral("Unknown hook event: ") + event, 2);
    }
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("rmk"));
    app.setApplicationVersion(QStringLiteral(RUNMARK_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Local-first execution continuity CLI"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption projectOption(
        {QStringLiteral("p"), QStringLiteral("project")},
        QStringLiteral("Path to project.json."),
        QStringLiteral("path"));
    parser.addOption(projectOption);
    const QCommandLineOption agentOption(QStringLiteral("agent"), QStringLiteral("Agent: codex or claude."), QStringLiteral("agent"), QStringLiteral("codex"));
    const QCommandLineOption repositoryOption(QStringLiteral("repo"), QStringLiteral("Repository name."), QStringLiteral("name"));
    const QCommandLineOption outcomeOption(QStringLiteral("outcome"), QStringLiteral("Finish outcome."), QStringLiteral("outcome"), QStringLiteral("finished"));
    const QCommandLineOption kindOption(QStringLiteral("kind"), QStringLiteral("Evidence or note kind."), QStringLiteral("kind"));
    const QCommandLineOption summaryOption(QStringLiteral("summary"), QStringLiteral("Evidence summary."), QStringLiteral("text"));
    const QCommandLineOption textOption(QStringLiteral("text"), QStringLiteral("Note text."), QStringLiteral("text"));
    const QCommandLineOption referenceOption(QStringLiteral("ref"), QStringLiteral("Durable source reference."), QStringLiteral("reference"));
    const QCommandLineOption instructionOption(QStringLiteral("instruction"), QStringLiteral("Instruction path, added to project instructions; repeatable."), QStringLiteral("path"));
    const QCommandLineOption markdownOption(QStringLiteral("markdown"), QStringLiteral("Render resume as Markdown."));
    const QCommandLineOption lineOption(QStringLiteral("line"), QStringLiteral("Render status as one short line."));
    const QCommandLineOption nameOption(QStringLiteral("name"), QStringLiteral("init: project name (default: folder name)."), QStringLiteral("name"));
    const QCommandLineOption remoteOption(QStringLiteral("remote"), QStringLiteral("init: git remote."), QStringLiteral("remote"), QStringLiteral("origin"));
    const QCommandLineOption branchOption(QStringLiteral("branch"), QStringLiteral("init: base branch."), QStringLiteral("branch"), QStringLiteral("main"));
    const QCommandLineOption planOption(QStringLiteral("plan"), QStringLiteral("init: plan file, created when missing."), QStringLiteral("path"), QStringLiteral("PLAN.md"));
    const QCommandLineOption prefixOption(QStringLiteral("prefix"), QStringLiteral("init: task prefix, e.g. NA for NA-1."), QStringLiteral("prefix"));
    parser.addOptions({agentOption, repositoryOption, outcomeOption, kindOption, summaryOption, textOption, referenceOption, instructionOption, markdownOption,
        lineOption, nameOption, remoteOption, branchOption, planOption, prefixOption});
    parser.addPositionalArgument(QStringLiteral("command"), QStringLiteral("init, inspect, status, start, finish, resume, evidence, note, or hook."));
    parser.addPositionalArgument(QStringLiteral("argument"), QStringLiteral("Task or execution ID, depending on command."), QStringLiteral("[argument]"));
    parser.process(app);

    const QStringList arguments = parser.positionalArguments();
    try {
        QString configPath = parser.value(projectOption);
        if (configPath.isEmpty()) configPath = runmark::locateProject(QDir::currentPath());
        // Nothing found: keep the old default so the error names a concrete path.
        if (configPath.isEmpty()) configPath = QDir::current().filePath(QStringLiteral(".runmark/project.json"));
        QJsonObject result;
        if (arguments == QStringList{QStringLiteral("init")}) {
            // Run in the repository root. An existing project is only registered.
            const QString existing = QDir::current().filePath(QStringLiteral(".runmark/project.json"));
            if (QFileInfo::exists(existing)) {
                runmark::registerProject(existing);
                result = {{QStringLiteral("project"), QFileInfo(existing).canonicalFilePath()}, {QStringLiteral("created"), false}};
            } else {
                if (!parser.isSet(prefixOption)) return emitError(QStringLiteral("usage"), QStringLiteral("rmk init needs --prefix for a new project"), 2);
                const QString plan = QDir::current().filePath(parser.value(planOption));
                if (!QFileInfo::exists(plan)) {
                    QDir().mkpath(QFileInfo(plan).absolutePath());
                    QFile file(plan);
                    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) || file.write("# Plan\n\n") < 0) {
                        return emitError(QStringLiteral("runtime"), QStringLiteral("Cannot create plan file ") + plan, 1);
                    }
                }
                const QString name = parser.isSet(nameOption) ? parser.value(nameOption) : QDir::current().dirName();
                result = {{QStringLiteral("project"), runmark::initializeProject(QDir::currentPath(), name, parser.value(remoteOption),
                              parser.value(branchOption), parser.value(planOption), parser.value(prefixOption))},
                          {QStringLiteral("created"), true}};
            }
        } else if (arguments == QStringList{QStringLiteral("inspect")}) {
            result = runmark::inspectProject(configPath).toJson();
        } else if (arguments == QStringList{QStringLiteral("status")}) {
            const runmark::StatusResult status = runmark::projectStatus(configPath);
            if (parser.isSet(lineOption)) {
                QTextStream(stdout) << runmark::statusSummary(status) << '\n';
                return 0;
            }
            result = toJson(status);
        } else if (arguments.size() == 2 && arguments.constFirst() == QLatin1String("start")) {
            result = toJson(runmark::startExecution(configPath, arguments.constLast(), parser.value(agentOption), parser.value(repositoryOption), parser.values(instructionOption)));
        } else if ((arguments.size() == 1 || arguments.size() == 2) && arguments.constFirst() == QLatin1String("resume")) {
            // resume without an argument means the latest execution. The
            // SessionStart hook does not know which task it is on, so it must
            // be able to call this without a selector.
            result = toJson(runmark::resumeExecution(configPath, arguments.size() == 2 ? arguments.constLast() : QString()));
            if (parser.isSet(markdownOption)) {
                QTextStream(stdout) << runmark::resumeMarkdown(result);
                return 0;
            }
        } else if (arguments.size() == 2 && arguments.constFirst() == QLatin1String("finish")) {
            result = toJson(runmark::finishExecution(configPath, arguments.constLast(), parser.value(outcomeOption)));
        } else if (arguments.size() == 2 && arguments.constFirst() == QLatin1String("evidence") && parser.isSet(kindOption) && parser.isSet(summaryOption)) {
            runmark::recordEvidence(configPath, arguments.constLast(), parser.value(kindOption), parser.value(summaryOption), parser.value(referenceOption));
            result = {{QStringLiteral("recorded"), QStringLiteral("evidence")}};
        } else if ((arguments.size() == 1 || arguments.size() == 2) && arguments.constFirst() == QLatin1String("note") && parser.isSet(kindOption) && parser.isSet(textOption)) {
            runmark::recordNote(configPath, arguments.size() == 2 ? arguments.constLast() : QString(), parser.value(kindOption), parser.value(textOption), parser.value(referenceOption));
            result = {{QStringLiteral("recorded"), QStringLiteral("note")}};
        } else if (arguments.size() == 2 && arguments.constFirst() == QLatin1String("hook")) {
            return runHook(configPath, arguments.constLast());
        } else {
            return emitError(QStringLiteral("usage"),
                QStringLiteral("Usage: rmk <init|inspect|status|start|finish|resume|evidence|note|hook> [argument] [options]"), 2);
        }
        QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Indented);
        return 0;
    } catch (const std::exception& error) {
        return emitError(QStringLiteral("runtime"), QString::fromUtf8(error.what()), 1);
    }
}
