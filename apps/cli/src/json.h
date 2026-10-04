#pragma once

// The output contract of TC-007. The application layer returns typed results;
// turning them into JSON is the CLI's job -- another surface (the desktop)
// makes something else from the same result, and the application knows about
// neither (TC-012).

#include "runmark/workflow.h"

#include <QJsonObject>
#include <QString>

namespace runmark {

QJsonObject toJson(const Finding& finding);
QJsonObject toJson(const RepoFacts& facts);
QJsonObject toJson(const StatusResult& result);
QJsonObject toJson(const StartResult& result);
QJsonObject toJson(const WorktreeCleanupFacts& facts);
QJsonObject toJson(const FinishResult& result);
QJsonObject toJson(const ResumeResult& result);
QJsonObject toJson(const SessionFacts& session);

// One short line for a terminal sidebar (herdr cuts it near 22 characters):
// plan progress, open executions, warnings; empty parts are left out.
QString statusSummary(const StatusResult& result);
// The second presentation of the same result: markdown for an agent to read.
QString resumeMarkdown(const QJsonObject& package);
// Appended to the session-start context when an earlier session committed
// work and left no note: its decisions live only in its transcript.
QString sessionWithoutNotesMarkdown(const SessionFacts& session);
// Appended to the session-start context: what the last session that left
// notes decided and left open.
QString sessionNotesMarkdown(const SessionFacts& session, const QVector<NoteRecorded>& notes);
// Heads the session-start context: executions other than the one resumed that
// still wait for someone. Empty when there are none.
// Session-start section for commits on no remote; empty when there are none.
QString localOnlyMarkdown(const QVector<RepoFacts>& repositories);
QString openWorkMarkdown(const QVector<OpenExecution>& open);

} // namespace runmark
