#include "runmark/workflow.h"
#include "runmark/error.h"
#include "config_io.h"
#include "git.h"
#include "paths.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QRegularExpression>

namespace runmark {
namespace {

QString configUnder(QString directory)
{
    while (!directory.isEmpty()) {
        const QString candidate = QDir(directory).filePath(QStringLiteral(".runmark/project.json"));
        if (QFileInfo(candidate).isFile()) return candidate;
        QDir up(directory);
        if (!up.cdUp()) break;
        directory = up.absolutePath();
    }
    return {};
}

// ~/.config/runmark/projects.json: {"projects": ["/abs/.runmark/project.json", ...]}
QString registryPath()
{
    const QString home = qEnvironmentVariable("RUNMARK_CONFIG_HOME", QDir::home().filePath(QStringLiteral(".config/runmark")));
    return QDir(home).filePath(QStringLiteral("projects.json"));
}

QStringList listedProjects()
{
    QFile file(registryPath());
    if (!file.open(QIODevice::ReadOnly)) return {};
    QStringList projects;
    for (const QJsonValue& value : QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("projects")).toArray()) {
        projects.append(value.toString());
    }
    return projects;
}

} // namespace

QString locateProject(const QString& directory)
{
    const QString start = QFileInfo(directory).canonicalFilePath();
    if (start.isEmpty()) return {};
    // 1. The directory or one of its parents.
    if (const QString found = configUnder(start); !found.isEmpty()) return found;
    // 2. A git worktree outside the project root: its main checkout.
    const ProcessResult common = git(start, {QStringLiteral("rev-parse"), QStringLiteral("--path-format=absolute"), QStringLiteral("--git-common-dir")});
    if (common.exitCode == 0) {
        QDir main(common.output.trimmed());
        if (main.cdUp()) {
            if (const QString found = configUnder(main.absolutePath()); !found.isEmpty()) return found;
        }
    }
    // 3. A registered project whose worktree root or repositories contain it.
    for (const QString& configPath : listedProjects()) {
        try {
            const ProjectConfig config = loadProjectConfig(configPath);
            const QString root = pathsFor(configPath).root;
            bool matches = isInsideDirectory(start, expandPath(config.worktreeRoot, root));
            for (const RepositoryConfig& repository : config.repositories) {
                matches = matches || isInsideDirectory(start, expandPath(repository.path, root));
            }
            if (matches) return configPath;
        } catch (const std::exception&) {
            // An unreadable registration is skipped, not fatal for discovery.
        }
    }
    return {};
}

QString initializeProject(const QString& folder, const QString& name, const QString& remote,
    const QString& branch, const QString& plan, const QString& taskPrefix)
{
    const QString root = QFileInfo(folder).canonicalFilePath();
    if (root.isEmpty() || !QFileInfo(root).isDir())
        fail(QStringLiteral("Geçerli bir proje klasörü seçin."));
    const QString gitRoot = gitRequired(root, {"rev-parse", "--show-toplevel"});
    if (QFileInfo(gitRoot).canonicalFilePath() != root)
        fail(QStringLiteral("Git deposunun kök klasörünü seçin."));
    if (!QRegularExpression(QStringLiteral("^[A-Za-z][A-Za-z0-9_]*$")).match(taskPrefix).hasMatch())
        fail(QStringLiteral("Görev öneki harfle başlamalı; harf, rakam ve alt çizgi içerebilir."));
    const QStringList remotes = gitRequired(root, {"remote"}).split('\n', Qt::SkipEmptyParts);
    if (!remotes.contains(remote))
        fail(QStringLiteral("Depoda bu remote tanımlı değil: ") + remote);
    gitRequired(root, {"check-ref-format", "refs/heads/" + branch});
    const QFileInfo planFile(expandPath(plan, root));
    if (!planFile.isFile() || !planFile.isReadable())
        fail(QStringLiteral("Okunabilir bir plan dosyası belirtin."));
    ProjectConfig config;
    config.name = name.trimmed();
    config.worktreeRoot = QDir::home().filePath("worktrees/" + QFileInfo(root).fileName());
    config.taskIdPattern = QRegularExpression::escape(taskPrefix) + QStringLiteral("-\\d+");
    config.planPaths = {QDir(root).relativeFilePath(planFile.absoluteFilePath())};
    config.repositories.append({config.name, QStringLiteral("."), remote, branch});
    if (QFileInfo::exists(QDir(root).filePath("AGENTS.md")))
        config.instructions.append(QStringLiteral("AGENTS.md"));
    const QString path = QDir(root).filePath(".runmark/project.json");
    createProjectConfig(path, config);
    registerProject(path);
    return path;
}

void registerProject(const QString& configPath)
{
    const QString path = QFileInfo(configPath).canonicalFilePath();
    QStringList projects = listedProjects();
    if (path.isEmpty() || projects.contains(path)) return;
    projects.append(path);
    const QString registry = registryPath();
    if (!QDir().mkpath(QFileInfo(registry).absolutePath())) fail(QStringLiteral("Cannot create ") + QFileInfo(registry).absolutePath());
    QSaveFile file(registry);
    const QByteArray data = QJsonDocument(QJsonObject{{QStringLiteral("projects"), QJsonArray::fromStringList(projects)}}).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        fail(QStringLiteral("Cannot write ") + registry + QStringLiteral(": ") + file.errorString());
    }
}
} // namespace runmark
