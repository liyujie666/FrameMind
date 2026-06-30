#include "service/conversationservice.h"

#include "infrastructure/databasemanager.h"
#include "infrastructure/imageprocessor.h"

#include <QUuid>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonArray>
#include <QByteArray>

namespace {
QString newId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString framesToJson(const QList<QImage>& frames)
{
    if (frames.isEmpty()) return {};
    QJsonArray arr;
    for (const QImage& img : frames) {
        // 存缩略图（≤256 边）的 base64 jpeg，控制库体积
        const QImage thumb = ImageProcessor::scaleToFit(img, QSize(256, 256));
        arr.append(QString::fromLatin1(ImageProcessor::toBase64Jpeg(thumb, 70)));
    }
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

QList<QImage> framesFromJson(const QString& json)
{
    QList<QImage> frames;
    if (json.isEmpty()) return frames;
    const QJsonArray arr = QJsonDocument::fromJson(json.toUtf8()).array();
    for (const auto& v : arr) {
        const QByteArray raw = QByteArray::fromBase64(v.toString().toLatin1());
        QImage img;
        if (img.loadFromData(raw, "JPEG")) {
            frames.append(img);
        }
    }
    return frames;
}
}  // namespace

ConversationService::ConversationService(DatabaseManager* db, QObject* parent)
    : QObject(parent)
    , m_db(db)
{
}

QList<Conversation> ConversationService::getAllConversations()
{
    QList<Conversation> result;
    if (!m_db) return result;
    const auto rows = m_db->query(QStringLiteral(
        "SELECT id, title, video_path, created_at, updated_at "
        "FROM conversations ORDER BY updated_at DESC"));
    for (const auto& row : rows) {
        Conversation c;
        c.id = row.value(QStringLiteral("id")).toString();
        c.title = row.value(QStringLiteral("title")).toString();
        c.videoFilePath = row.value(QStringLiteral("video_path")).toString();
        c.createdAt = row.value(QStringLiteral("created_at")).toDateTime();
        c.updatedAt = row.value(QStringLiteral("updated_at")).toDateTime();
        result.append(c);
    }
    return result;
}

Conversation ConversationService::createConversation(const QString& videoPath)
{
    Conversation c;
    c.id = newId();
    c.title = QStringLiteral("新对话");
    c.videoFilePath = videoPath;
    c.createdAt = QDateTime::currentDateTime();
    c.updatedAt = c.createdAt;
    if (m_db) {
        m_db->exec(QStringLiteral(
            "INSERT INTO conversations(id, title, video_path, created_at, updated_at) "
            "VALUES(?, ?, ?, ?, ?)"),
            { c.id, c.title, c.videoFilePath,
              c.createdAt.toString(Qt::ISODate),
              c.updatedAt.toString(Qt::ISODate) });
    }
    return c;
}

void ConversationService::deleteConversation(const QString& convId)
{
    if (!m_db) return;
    // messages 通过外键 ON DELETE CASCADE 一并删除
    m_db->exec(QStringLiteral("DELETE FROM conversations WHERE id = ?"), { convId });
}

void ConversationService::updateTitle(const QString& convId, const QString& title)
{
    if (!m_db) return;
    m_db->exec(QStringLiteral(
        "UPDATE conversations SET title = ?, updated_at = ? WHERE id = ?"),
        { title, QDateTime::currentDateTime().toString(Qt::ISODate), convId });
}

QList<ChatMessage> ConversationService::getMessages(const QString& convId)
{
    QList<ChatMessage> result;
    if (!m_db) return result;
    const auto rows = m_db->query(QStringLiteral(
        "SELECT id, role, content, attached_frames, timestamp "
        "FROM messages WHERE conversation_id = ? ORDER BY timestamp ASC"),
        { convId });
    for (const auto& row : rows) {
        ChatMessage m;
        m.id = row.value(QStringLiteral("id")).toString();
        m.role = ChatMessage::roleFromString(
            row.value(QStringLiteral("role")).toString());
        m.content = row.value(QStringLiteral("content")).toString();
        m.attachedFrames =
            framesFromJson(row.value(QStringLiteral("attached_frames")).toString());
        m.timestamp = row.value(QStringLiteral("timestamp")).toDateTime();
        result.append(m);
    }
    return result;
}

void ConversationService::saveMessage(const QString& convId, const ChatMessage& msg)
{
    if (!m_db) return;
    const QString id = msg.id.isEmpty() ? newId() : msg.id;
    m_db->exec(QStringLiteral(
        "INSERT INTO messages(id, conversation_id, role, content, attached_frames, timestamp) "
        "VALUES(?, ?, ?, ?, ?, ?)"),
        { id, convId, ChatMessage::roleToString(msg.role), msg.content,
          framesToJson(msg.attachedFrames),
          (msg.timestamp.isValid() ? msg.timestamp : QDateTime::currentDateTime())
              .toString(Qt::ISODate) });
    // 更新会话的 updated_at，便于按最近排序
    m_db->exec(QStringLiteral(
        "UPDATE conversations SET updated_at = ? WHERE id = ?"),
        { QDateTime::currentDateTime().toString(Qt::ISODate), convId });
}

void ConversationService::updateMessage(const QString& msgId, const QString& content)
{
    if (!m_db) return;
    m_db->exec(QStringLiteral("UPDATE messages SET content = ? WHERE id = ?"),
               { content, msgId });
}
