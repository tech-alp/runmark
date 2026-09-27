#pragma once

#include "runmark/facts.h"
#include "runmark/project_config.h"
#include "paths.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace runmark {

struct ProcessResult {
    int exitCode;
    QString output;
    QString error;
};

// --- transport: unaware of what it carries (TC-006) ---
ProcessResult git(const QString& repository, const QStringList& arguments);
QString gitRequired(const QString& repository, const QStringList& arguments);
QString gitCommonDir(const QString& repository);

// --- measurement ---
QString baseRef(const RepositoryConfig& repository);
// fetch = false measures against the last fetched remote refs, with no network.
RepoFacts observeRepo(const RepositoryConfig& repository, const QString& repositoryPath, bool fetch = true);
void observeResumeGit(const ProjectConfig& config, const Paths& paths, ResumeFacts& facts);

// Measures whether a finished execution's worktree can be removed safely.
// Reports; never removes. A check that cannot run leaves `error` set rather
// than reporting a clean-and-merged worktree that was never inspected.
WorktreeCleanupFacts observeWorktreeCleanup(const QString& worktree, const QString& branch, const QString& base);

// Captures uncommitted work under refs/runmark/preserved/<exec>. Touches
// neither the working tree nor the global stash stack. Empty when clean.
QString preserveWorktree(const QString& worktree, const QString& executionId, const QString& previousRef = {});

// Writes the directories Runmark produces to .git/info/exclude, never to
// the shared .gitignore.
void ensureGitExcludes(const Paths& paths);

} // namespace runmark
