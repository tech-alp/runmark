#include "git.h"

#include "runmark/error.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryFile>

#include <stdexcept>

namespace runmark {
namespace {

ProcessResult run(const QString& program, const QStringList& arguments, const QProcessEnvironment& environment = QProcessEnvironment::systemEnvironment())
{
    QProcess process;
    process.setProgram(program);
    process.setArguments(arguments);
    process.setProcessEnvironment(environment);
    process.start();
    if (!process.waitForStarted(5000)) {
        fail(QStringLiteral("Cannot start %1: %2").arg(program, process.errorString()));
    }
    if (!process.waitForFinished(60000)) {
        process.kill();
        fail(QStringLiteral("Timed out: %1 %2").arg(program, arguments.join(QLatin1Char(' '))));
    }
    return {process.exitCode(), QString::fromUtf8(process.readAllStandardOutput()).trimmed(), QString::fromUtf8(process.readAllStandardError()).trimmed()};
}

QString gitWithIndexRequired(const QString& repository, const QStringList& arguments, const QString& indexPath)
{
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("GIT_INDEX_FILE"), indexPath);
    QStringList gitArguments{QStringLiteral("-C"), repository};
    gitArguments.append(arguments);
    const ProcessResult result = run(QStringLiteral("git"), gitArguments, environment);
    if (result.exitCode != 0) {
        fail(QStringLiteral("git -C %1 %2: %3").arg(repository, arguments.join(QLatin1Char(' ')), result.error));
    }
    return result.output;
}

int leftRightCount(const QString& output, int index)
{
    return output.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts).value(index).toInt();
}

} // namespace

ProcessResult git(const QString& repository, const QStringList& arguments)
{
    QStringList gitArguments{QStringLiteral("-C"), repository};
    gitArguments.append(arguments);
    return run(QStringLiteral("git"), gitArguments);
}

QString gitRequired(const QString& repository, const QStringList& arguments)
{
    const ProcessResult result = git(repository, arguments);
    if (result.exitCode != 0) {
        fail(QStringLiteral("git -C %1 %2: %3").arg(repository, arguments.join(QLatin1Char(' ')), result.error));
    }
    return result.output;
}

QString gitCommonDir(const QString& repository)
{
    const QString commonDir = gitRequired(repository, {QStringLiteral("rev-parse"), QStringLiteral("--git-common-dir")});
    const QString absolutePath = QDir::isAbsolutePath(commonDir) ? QDir::cleanPath(commonDir) : QDir::cleanPath(QDir(repository).absoluteFilePath(commonDir));
    const QString canonicalPath = QFileInfo(absolutePath).canonicalFilePath();
    return canonicalPath.isEmpty() ? absolutePath : canonicalPath;
}

QString baseRef(const RepositoryConfig& repository)
{
    return repository.remote + QLatin1Char('/') + repository.branch;
}

RepoFacts observeRepo(const RepositoryConfig& repository, const QString& repositoryPath, bool fetch)
{
    RepoFacts facts;
    facts.name = repository.name;
    facts.path = repositoryPath;
    facts.base = baseRef(repository);

    // Fetch is wrapped separately: its failure must not cancel the other
    // measurements. Dirty state and local base lag need no network.
    if (fetch) {
        const ProcessResult fetchResult = git(repositoryPath, {QStringLiteral("fetch"), QStringLiteral("--quiet"), repository.remote});
        if (fetchResult.exitCode != 0) facts.fetchError = fetchResult.error;
    }

    try {
        const QString dirty = gitRequired(repositoryPath, {QStringLiteral("status"), QStringLiteral("--porcelain")});
        facts.dirty = !dirty.isEmpty();
        facts.head = gitRequired(repositoryPath, {QStringLiteral("rev-parse"), QStringLiteral("HEAD")});
        facts.branch = gitRequired(repositoryPath, {QStringLiteral("branch"), QStringLiteral("--show-current")});
        facts.baseSha = gitRequired(repositoryPath, {QStringLiteral("rev-parse"), facts.base});
        const QString counts = gitRequired(repositoryPath, {QStringLiteral("rev-list"), QStringLiteral("--left-right"), QStringLiteral("--count"), facts.base + QStringLiteral("...HEAD")});
        facts.behind = leftRightCount(counts, 0);
        facts.ahead = leftRightCount(counts, 1);

        facts.localBase = repository.branch;
        const ProcessResult localBaseExists = git(repositoryPath, {QStringLiteral("rev-parse"), QStringLiteral("--verify"), facts.localBase});
        if (localBaseExists.exitCode == 0) {
            facts.localBaseExists = true;
            facts.localBehind = leftRightCount(
                gitRequired(repositoryPath, {QStringLiteral("rev-list"), QStringLiteral("--left-right"), QStringLiteral("--count"), facts.base + QStringLiteral("...") + facts.localBase}), 0);
        }
        facts.measured = true;
    } catch (const std::exception& error) {
        facts.measurementError = QString::fromUtf8(error.what());
    }
    return facts;
}


void observeResumeGit(const ProjectConfig& config, const Paths& paths, ResumeFacts& facts)
{
    // Called only for an execution with a start event.
    const ExecutionStarted& started = *facts.started;
    facts.worktree = observePath(started.worktree);
    const QString baseSha = started.baseSha;
    if (facts.finished) {
        facts.measured = {QStringLiteral("execution.finished"), {}, facts.finished->commits, facts.finished->filesChanged};
        if (!facts.measured.commits || !facts.measured.filesChanged) {
            facts.measurementError = QStringLiteral("Finish event lacks commit or file measurements");
        }
    } else {
        try {
            if (facts.worktree.exists != true) fail(QStringLiteral("Worktree cannot be inspected"));
            bool sameRepository = false;
            for (const RepositoryConfig& repository : config.repositories) {
                if (repository.name == started.repo) {
                    sameRepository = gitCommonDir(facts.worktree.path) == gitCommonDir(expandPath(repository.path, paths.root));
                }
            }
            if (!sameRepository) fail(QStringLiteral("Worktree does not belong to the recorded repository"));
            if (baseSha.isEmpty()) fail(QStringLiteral("No base_sha was recorded"));
            const QString head = gitRequired(facts.worktree.path, {QStringLiteral("rev-parse"), QStringLiteral("HEAD")});
            const QString range = baseSha + QStringLiteral("..") + head;
            const QStringList commits = gitRequired(facts.worktree.path,
                {QStringLiteral("log"), QStringLiteral("--format=%H"), range}).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            const QString files = gitRequired(facts.worktree.path,
                {QStringLiteral("diff"), QStringLiteral("--name-only"), QStringLiteral("-z"), range});
            facts.measured = {QStringLiteral("git"), head, commits, int(files.count(QChar('\0')))};
        } catch (const std::exception& error) {
            facts.measurementError = QString::fromUtf8(error.what());
        }
    }

    try {
        const RepositoryConfig* repository = nullptr;
        for (const RepositoryConfig& candidate : config.repositories) {
            if (candidate.name == started.repo) repository = &candidate;
        }
        if (!repository) fail(QStringLiteral("Recorded repository is absent from project config"));
        // The recorded display string is never parsed (DATA_MODEL invariant).
        if (baseRef(*repository) != started.base) {
            fail(QStringLiteral("Configured base differs from the recorded base"));
        }
        QString baseline = started.remoteBaseSha;
        if (baseline.isEmpty() && started.workspaceSource == QLatin1String("created")) baseline = baseSha;
        if (baseline.isEmpty()) fail(QStringLiteral("Remote base SHA at start is unknown"));
        const QString repositoryPath = expandPath(repository->path, paths.root);
        // Fetch this branch explicitly: a deleted remote branch must not leave
        // a stale tracking ref looking like a verified, unchanged base.
        const ProcessResult fetch = git(repositoryPath, {QStringLiteral("fetch"), QStringLiteral("--quiet"),
            repository->remote, QStringLiteral("refs/heads/") + repository->branch});
        if (fetch.exitCode != 0) {
            facts.fetchError = fetch.error.isEmpty() ? QStringLiteral("git fetch failed") : fetch.error;
            return;
        }
        facts.currentBaseSha = gitRequired(repositoryPath, {QStringLiteral("rev-parse"), QStringLiteral("FETCH_HEAD")});
        if (baseline == facts.currentBaseSha) {
            facts.baseAdvanced = false;
        } else {
            const ProcessResult ancestor = git(repositoryPath,
                {QStringLiteral("merge-base"), QStringLiteral("--is-ancestor"), baseline, facts.currentBaseSha});
            if (ancestor.exitCode != 0) fail(QStringLiteral("Base history diverged or ancestry cannot be established: ") + ancestor.error);
            facts.baseAdvanced = true;
        }
    } catch (const std::exception& error) {
        facts.baseError = QString::fromUtf8(error.what());
    }
}

WorktreeCleanupFacts observeWorktreeCleanup(const QString& worktree, const QString& branch, const QString& base)
{
    WorktreeCleanupFacts facts;
    facts.path = worktree;
    if (worktree.isEmpty()) {
        facts.error = QStringLiteral("No worktree was recorded for this execution");
        return facts;
    }
    if (!QFileInfo::exists(worktree)) {
        return facts;   // already gone; nothing to suggest
    }
    facts.exists = true;

    const ProcessResult dirty = git(worktree, {QStringLiteral("status"), QStringLiteral("--porcelain")});
    if (dirty.exitCode != 0) {
        facts.error = QStringLiteral("Cannot read worktree status: ") + dirty.error;
        return facts;
    }
    facts.clean = dirty.output.trimmed().isEmpty();

    if (branch.isEmpty() || base.isEmpty()) {
        facts.error = QStringLiteral("Branch or base was not recorded; merge state is unknown");
        return facts;
    }
    const ProcessResult merged = git(worktree, {QStringLiteral("merge-base"), QStringLiteral("--is-ancestor"), branch, base});
    if (merged.exitCode != 0 && merged.exitCode != 1) {
        facts.error = QStringLiteral("Cannot compare ") + branch + QStringLiteral(" with ") + base;
        return facts;
    }
    facts.merged = merged.exitCode == 0;
    return facts;
}

QString preserveWorktree(const QString& worktree, const QString& executionId, const QString& previousRef)
{
    if (gitRequired(worktree, {QStringLiteral("status"), QStringLiteral("--porcelain")}).isEmpty()) return {};

    QTemporaryFile temporaryIndex;
    if (!temporaryIndex.open()) fail(QStringLiteral("Cannot create temporary Git index"));
    const QString indexPath = temporaryIndex.fileName();
    temporaryIndex.close();
    if (!QFile::remove(indexPath)) fail(QStringLiteral("Cannot prepare temporary Git index"));

    gitWithIndexRequired(worktree, {QStringLiteral("read-tree"), QStringLiteral("HEAD")}, indexPath);
    gitWithIndexRequired(worktree, {QStringLiteral("add"), QStringLiteral("-A")}, indexPath);
    const QString tree = gitWithIndexRequired(worktree, {QStringLiteral("write-tree")}, indexPath);
    QStringList commitArguments{QStringLiteral("commit-tree"), tree, QStringLiteral("-p"), QStringLiteral("HEAD")};
    if (!previousRef.isEmpty()) {
        const ProcessResult previous = git(worktree, {QStringLiteral("rev-parse"), QStringLiteral("--verify"), previousRef});
        if (previous.exitCode == 0) {
            commitArguments.append({QStringLiteral("-p"), previous.output});
        }
    }
    commitArguments.append({QStringLiteral("-m"), QStringLiteral("runmark: preserve uncommitted work for ") + executionId});
    const QString preservedObject = gitRequired(worktree, commitArguments);
    const QString preservedRef = QStringLiteral("refs/runmark/preserved/") + executionId;
    gitRequired(worktree, {QStringLiteral("update-ref"), preservedRef, preservedObject});
    return preservedRef;
}

void ensureGitExcludes(const Paths& paths)
{
    const ProcessResult insideWorktree = git(paths.root, {QStringLiteral("rev-parse"), QStringLiteral("--is-inside-work-tree")});
    if (insideWorktree.exitCode != 0 || insideWorktree.output != QLatin1String("true")) {
        return;
    }
    const QString repositoryRoot = QDir::cleanPath(QDir(paths.root).absoluteFilePath(
        gitRequired(paths.root, {QStringLiteral("rev-parse"), QStringLiteral("--show-cdup")})));
    const QString commonDir = gitCommonDir(paths.root);
    const QString statePath = QDir::cleanPath(QDir(repositoryRoot).relativeFilePath(paths.state));
    const QStringList patterns{
        QLatin1Char('/') + statePath + QStringLiteral("/ledger/"),
        QLatin1Char('/') + statePath + QStringLiteral("/evidence/"),
        QLatin1Char('/') + statePath + QStringLiteral("/handoffs/"),
        QLatin1Char('/') + statePath + QStringLiteral("/sessions/"),
    };

    QFile exclude(QDir(commonDir).filePath(QStringLiteral("info/exclude")));
    QByteArray contents;
    if (exclude.exists()) {
        if (!exclude.open(QIODevice::ReadOnly)) {
            fail(QStringLiteral("Cannot read Git exclude file: %1").arg(exclude.fileName()));
        }
        contents = exclude.readAll();
        exclude.close();
    }
    QSet<QString> existing;
    for (const QByteArray& line : contents.split('\n')) {
        existing.insert(QString::fromUtf8(line).trimmed());
    }
    QStringList missing;
    for (const QString& pattern : patterns) {
        if (!existing.contains(pattern)) missing.append(pattern);
    }
    if (missing.isEmpty()) return;

    if (!exclude.open(QIODevice::WriteOnly | QIODevice::Append)) {
        fail(QStringLiteral("Cannot write Git exclude file: %1").arg(exclude.fileName()));
    }
    if (!contents.isEmpty() && !contents.endsWith('\n')) exclude.write("\n");
    for (const QString& pattern : missing) {
        exclude.write(pattern.toUtf8());
        exclude.write("\n");
    }
}

} // namespace runmark
