#include "workflow_checkpoint.h"

#include "infrastructure/databasemanager.h"

#include <QJsonDocument>
#include <QDateTime>

WorkflowCheckpoint::WorkflowCheckpoint()
{
}

void WorkflowCheckpoint::initialize()
{
    DatabaseManager::instance()->exec(
        "CREATE TABLE IF NOT EXISTS workflow_checkpoints ("
        "  id TEXT PRIMARY KEY,"
        "  state_json TEXT NOT NULL,"
        "  current_node TEXT NOT NULL,"
        "  updated_at TEXT NOT NULL"
        ")"
    );
}

void WorkflowCheckpoint::save(const QString& workflowId,
                               const WorkflowState& state,
                               const QString& currentNodeId)
{
    QJsonObject stateJson = state.serialize();
    QByteArray stateBytes = QJsonDocument(stateJson).toJson(QJsonDocument::Compact);
    QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    DatabaseManager::instance()->exec(
        "INSERT OR REPLACE INTO workflow_checkpoints (id, state_json, current_node, updated_at) "
        "VALUES (?, ?, ?, ?)",
        {workflowId, QString::fromUtf8(stateBytes), currentNodeId, now}
    );
}

std::optional<QJsonObject> WorkflowCheckpoint::load(const QString& workflowId)
{
    auto rows = DatabaseManager::instance()->query(
        "SELECT state_json, current_node FROM workflow_checkpoints WHERE id = ?",
        {workflowId}
    );

    if (rows.isEmpty()) {
        return std::nullopt;
    }

    const auto state=QJsonDocument::fromJson(rows.first()["state_json"].toString().toUtf8()).object();
    const auto data=state["data"].toObject();const QString video=data["video_id"].toString();
    if(!video.isEmpty()) {
        const auto activeRows=DatabaseManager::instance()->query("SELECT b.payload_json FROM video_metadata m LEFT JOIN video_rag_builds b ON b.build_id=m.active_build_id WHERE m.video_id=?",{video});
        const auto active=activeRows.isEmpty()?QJsonObject{}:QJsonDocument::fromJson(activeRows.first()["payload_json"].toString().toUtf8()).object();
        if(data["build_id"].toString()!=active["build_id"].toString() || (!active["build_id"].toString().isEmpty() && data["build_revision"].toInt()!=active["revision"].toInt())) {remove(workflowId);return std::nullopt;}
    }

    QJsonObject result;
    QString stateStr = rows.first()["state_json"].toString();
    result["state"] = QJsonDocument::fromJson(stateStr.toUtf8()).object();
    result["currentNode"] = rows.first()["current_node"].toString();
    return result;
}

void WorkflowCheckpoint::remove(const QString& workflowId)
{
    DatabaseManager::instance()->exec(
        "DELETE FROM workflow_checkpoints WHERE id = ?",
        {workflowId}
    );
}

void WorkflowCheckpoint::cleanup(int maxAgeDays)
{
    QString cutoff = QDateTime::currentDateTimeUtc()
                         .addDays(-maxAgeDays)
                         .toString(Qt::ISODate);

    DatabaseManager::instance()->exec(
        "DELETE FROM workflow_checkpoints WHERE updated_at < ?",
        {cutoff}
    );
}
