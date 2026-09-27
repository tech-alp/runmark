#pragma once

#include "paths.h"

#include <QJsonObject>
#include <QString>
#include <QVector>

#include <optional>

namespace runmark {

// The only place that knows the ledger's JSONL keys (DATA_MODEL.md §3).
// Everything above reads and writes the typed events from the domain.

// One JSONL file per execution: two agents writing at once never collide,
// so no lock is needed (DATA_MODEL.md §1).
void appendEvent(const Paths& paths, const ExecutionStarted& event);
void appendEvent(const Paths& paths, const ExecutionFinished& event);
void appendEvent(const Paths& paths, const EvidenceRecorded& event);
void appendEvent(const Paths& paths, const NoteRecorded& event);

// Fails on an unreadable file or a line that is not a JSON object; a line of
// an unknown type is kept in Ledger::unrecognised instead.
Ledger readLedger(const Paths& paths);
const ExecutionStarted& startedEvent(const Ledger& ledger, const QString& executionId);
ResumeFacts observeResumeLedger(const Paths& paths, const QString& taskOrExecution);

// Agent sessions, one JSONL file each under .runmark/sessions/ (no lock: one
// writer per session). A note whose exec is empty goes to its session's file.
bool isSafeSessionId(const QString& id);
void recordSessionStarted(const Paths& paths, const SessionFacts& session);
void recordSessionWorking(const Paths& paths, const QString& id, const QString& head);
void recordSessionWaiting(const Paths& paths, const QString& id);
void recordSessionReminded(const Paths& paths, const QString& id, const QString& head);
void recordSessionEnded(const Paths& paths, const QString& id, const QString& reason);
std::optional<SessionFacts> readSession(const Paths& paths, const QString& id);
QVector<SessionFacts> readSessions(const Paths& paths, QString* error);

// Content addressed: the same payload lands in the same file.
QString writeEvidence(const Paths& paths, const QJsonObject& value);

} // namespace runmark
