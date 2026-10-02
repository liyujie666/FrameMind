#pragma once
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QMutex>
#include <memory>

Q_DECLARE_LOGGING_CATEGORY(ragLog)
Q_DECLARE_LOGGING_CATEGORY(ragDetailLog)

// One shared trace per build, including worker threads. Never pass prompts,
// transcripts, model bodies or authentication data as log fields.
class VideoRagLog final {
public:
    enum class Level { Debug, Info, Warning, Error };
    VideoRagLog(const QString& buildId, const QString& videoId, const QString& directory = {});
    void event(const QString& stage, const QString& event, const QString& message,
               QJsonObject fields = {}, Level level = Level::Info);
    void progress(const QString& stage, int completed, int total, const QString& message,
                  QJsonObject fields = {});
    QString filePath() const { return m_file.fileName(); }
    bool available() const;
private:
    void write(const QString&, const QString&, const QString&, QJsonObject, Level);
    QString m_buildId, m_videoId;
    mutable QMutex m_mutex;
    QFile m_file;
    QElapsedTimer m_timer;
    QHash<QString, qint64> m_progressAt;
    quint64 m_sequence = 0;
    bool m_sinkFailed = false;
    bool m_terminal = false;
};
