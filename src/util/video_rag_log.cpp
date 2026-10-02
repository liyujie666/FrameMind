#include "util/video_rag_log.h"
#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QMutexLocker>
#include <QStandardPaths>

Q_LOGGING_CATEGORY(ragLog, "framemind.rag", QtInfoMsg)
Q_LOGGING_CATEGORY(ragDetailLog, "framemind.rag.detail", QtInfoMsg)

VideoRagLog::VideoRagLog(const QString& buildId, const QString& videoId, const QString& directory)
    : m_buildId(buildId), m_videoId(videoId) {
    m_timer.start();
    const auto root = directory.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/diagnostics/video-rag/" + buildId
        : directory;
    if (QDir().mkpath(root)) {
        m_file.setFileName(root + "/build.jsonl");
        m_file.open(QIODevice::WriteOnly | QIODevice::NewOnly);
    }
    if (!m_file.isOpen())
        event("build", "log_unavailable", "构建日志文件无法创建，继续输出到控制台",
              {}, Level::Warning);
}

bool VideoRagLog::available() const {
    QMutexLocker lock(&m_mutex);
    return m_file.isOpen() && !m_sinkFailed;
}

void VideoRagLog::event(const QString& stage, const QString& name, const QString& message,
                       QJsonObject fields, Level level) {
    QMutexLocker lock(&m_mutex);
    write(stage, name, message, std::move(fields), level);
}

void VideoRagLog::progress(const QString& stage, int completed, int total, const QString& message,
                          QJsonObject fields) {
    QMutexLocker lock(&m_mutex);
    const qint64 now = m_timer.elapsed();
    if (completed < total && m_progressAt.contains(stage) && now - m_progressAt[stage] < 5000)
        return;
    m_progressAt[stage] = now;
    fields["completed"] = completed;
    fields["total"] = total;
    write(stage, "progress", message, std::move(fields), Level::Info);
}

void VideoRagLog::write(const QString& stage, const QString& name, const QString& message,
                       QJsonObject fields, Level level) {
    const QString severity = level == Level::Debug ? "DEBUG" : level == Level::Info ? "INFO"
        : level == Level::Warning ? "WARN" : "ERROR";
    const auto timestamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    QJsonObject row{{"schema_version", 1}, {"timestamp_utc", timestamp},
        {"sequence", qint64(++m_sequence)}, {"level", severity},
        {"build_id", m_buildId}, {"video_id", m_videoId}, {"stage", stage},
        {"event", name}, {"elapsed_ms", m_timer.elapsed()}, {"message", message}, {"fields", fields}};
    const auto line = QJsonDocument(row).toJson(QJsonDocument::Compact) + '\n';
    // Preserve the final summary after large traces: only debug records stop at 16 MiB.
    if (m_file.isOpen() && !m_sinkFailed && (level != Level::Debug || m_file.size() < 16 * 1024 * 1024)) {
        if (m_file.write(line) != line.size() || !m_file.flush()) {
            m_sinkFailed = true;
            qCWarning(ragLog).noquote() << "[RAG] build=" + m_buildId.left(8)
                << "stage=build event=log_write_failed 日志文件写入失败";
        }
    }
    // Full nested request/preparation diagnostics belong in the trace file.
    QStringList metrics;
    for (auto it = fields.begin(); it != fields.end(); ++it) {
        if (it.value().isObject() || it.value().isArray()) continue;
        const auto value = it.value().isString() ? it.value().toString().left(500)
            : QString::fromUtf8(QJsonDocument(QJsonArray{it.value()}).toJson(QJsonDocument::Compact)).mid(1).chopped(1);
        QString escaped = value;
        escaped.replace('\n', "\\n").replace('\r', "\\r");
        metrics << it.key() + "=" + escaped;
    }
    QString safeMessage = message;
    safeMessage.replace('\n', "\\n").replace('\r', "\\r");
    const auto console = QString("%1 [%2] [RAG] build=%3 stage=%4 event=%5 +%6ms %7 %8")
        .arg(timestamp, severity, m_buildId.left(8), stage, name)
        .arg(m_timer.elapsed()).arg(safeMessage, metrics.join(' ')).trimmed();
    switch (level) {
    case Level::Debug: qCDebug(ragLog).noquote() << console; break;
    case Level::Info: qCInfo(ragLog).noquote() << console; break;
    case Level::Warning: qCWarning(ragLog).noquote() << console; break;
    case Level::Error: qCCritical(ragLog).noquote() << console; break;
    }
}
