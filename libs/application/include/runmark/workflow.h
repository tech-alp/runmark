#pragma once

// Use cases. Results come back TYPED: turning them into JSON is the CLI's
// job, into a model the UI's (TC-012). The application layer is not bound to
// any one presentation.

#include "runmark/facts.h"
#include "runmark/finding.h"
#include "runmark/project_config.h"

#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

namespace runmark {

struct StatusResult {
    QString project;
    QVector<RepoFacts> repositories;
    QVector<Finding> findings;
    QVector<SessionFacts> sessions;
    PlanFacts plan;            // per-file progress: how many planned, how many done
    QVector<OpenExecution> openWork;
};

struct StartResult {
    QString exec;
    QString worktree;
    QString branch;
    QString workspaceSource;
    QString baseSha;
    QString preservedRef;      // empty when there was nothing to preserve
    QVector<Finding> warnings;
};

struct FinishResult {
    QString exec;
    QString outcome;
    QString headSha;
    QString preservedRef;
    QString handoff;           // relative to the .runmark root
    // What the worktree looks like now that the execution is closed. Reported
    // at the moment the decision is made, so nobody has to go looking for the
    // standing git.orphaned_worktree finding.
    WorktreeCleanupFacts worktree;
};

struct ResumeResult {
    ResumeFacts facts;
    QVector<Finding> gaps;
};

// What an agent runtime hands its hook on stdin, already parsed. Measured in
// Claude Code 2.1.282 and Codex 0.156.1: both send the same fields.
struct HookInput {
    QString sessionId;
    QString cwd;
    QString transcriptPath;
    QString source;            // SessionStart: startup | resume | ...
    QString reason;            // SessionEnd
    bool stopHookActive = false;
};

struct SessionStartResult {
    // An earlier session of this project that committed work, was asked for a
    // note and still left none: its decisions exist only in its transcript.
    std::optional<SessionFacts> previousWithoutNotes;
    // The most recent other session that left notes, with them. Work done
    // without `rmk start` has no execution to resume; its notes are the
    // continuity.
    std::optional<SessionFacts> lastWithNotes;
    QVector<NoteRecorded> lastNotes;
    // Every task's unfinished or interrupted execution: the resume context
    // shows only the latest execution, so parallel work would go unseen.
    QVector<OpenExecution> openWork;
};

// The project.json that governs `directory`: the directory or a parent, the
// main checkout of a git worktree, or a project registered in
// ~/.config/runmark/projects.json whose worktree root contains it. Empty when
// none does. Agents work inside worktrees outside the project root, so the
// current directory alone is not enough.
QString locateProject(const QString& directory);

QString initializeProject(const QString& folder, const QString& name, const QString& remote,
    const QString& branch, const QString& plan, const QString& taskPrefix);
// Adds a project.json to ~/.config/runmark/projects.json once. initializeProject
// does it for new projects; an existing one is registered by `rmk init`.
void registerProject(const QString& configPath);
ProjectConfig inspectProject(const QString& configPath);
// fetch = false skips git fetch: remote findings use the last fetched refs.
StatusResult projectStatus(const QString& configPath, bool fetch = true);
StartResult startExecution(const QString& configPath, const QString& task, const QString& agent, const QString& repositoryName, const QStringList& instructions = {});
ResumeResult resumeExecution(const QString& configPath, const QString& taskOrExecution);

// Agent session hooks. Each records into .runmark/sessions/<id>.jsonl.
bool isSessionId(const QString& id);
SessionStartResult sessionStarted(const QString& configPath, const HookInput& input);
void sessionWorking(const QString& configPath, const HookInput& input);
// The agent finished a reply. Returns the text to hand back as a one-time
// "block" when a commit landed in this session and no note followed it.
std::optional<QString> sessionStopped(const QString& configPath, const HookInput& input);
void sessionEnded(const QString& configPath, const HookInput& input);
FinishResult finishExecution(const QString& configPath, const QString& executionId, const QString& outcome);
void recordEvidence(const QString& configPath, const QString& executionId, const QString& kind, const QString& summary, const QString& reference);
// An empty executionId writes the note to the agent session rmk runs in
// (CODEX_THREAD_ID / CLAUDE_CODE_SESSION_ID); outside a session it fails.
void recordNote(const QString& configPath, const QString& executionId, const QString& kind, const QString& text, const QString& reference);

} // namespace runmark
