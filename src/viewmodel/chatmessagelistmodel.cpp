#include "viewmodel/chatmessagelistmodel.h"

ChatMessageListModel::ChatMessageListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int ChatMessageListModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) return 0;
    return m_messages.size();
}

QVariant ChatMessageListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_messages.size())
        return {};
    const ChatMessage& m = m_messages.at(index.row());
    switch (role) {
    case RoleRole:        return static_cast<int>(m.role);
    case ContentRole:     return m.content;
    case TimestampRole:   return m.timestamp;
    case IsStreamingRole: return m.isStreaming;
    case IdRole:          return m.id;
    case AttachedFramesRole: return m.attachedFrames.size();
    case AgentStatusRole: return m.agentStatus;
    case StartTimeRole:   return m.startTime;
    case ElapsedMsRole:   return m.elapsedMs;
    default:              return {};
    }
}

QHash<int, QByteArray> ChatMessageListModel::roleNames() const
{
    return {
        { RoleRole,           "role" },
        { ContentRole,        "content" },
        { TimestampRole,      "timestamp" },
        { IsStreamingRole,    "isStreaming" },
        { AttachedFramesRole, "attachedFrames" },
        { IdRole,             "id" },
        { AgentStatusRole,    "agentStatus" },
        { StartTimeRole,      "startTime" },
        { ElapsedMsRole,      "elapsedMs" },
    };
}

int ChatMessageListModel::appendMessage(const ChatMessage& msg)
{
    const int row = m_messages.size();
    beginInsertRows({}, row, row);
    m_messages.append(msg);
    endInsertRows();
    return row;
}

void ChatMessageListModel::updateContent(int row, const QString& content)
{
    if (row < 0 || row >= m_messages.size()) return;
    m_messages[row].content = content;
    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx, { ContentRole });
}

void ChatMessageListModel::appendDeltaSilent(int row, const QString& delta)
{
    if (row < 0 || row >= m_messages.size()) return;
    m_messages[row].content += delta;
    // 故意不发 dataChanged，由 flushRow 在节流时机统一刷新
}

void ChatMessageListModel::flushRow(int row)
{
    if (row < 0 || row >= m_messages.size()) return;
    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx, { ContentRole });
}

void ChatMessageListModel::setStreaming(int row, bool streaming)
{
    if (row < 0 || row >= m_messages.size()) return;
    m_messages[row].isStreaming = streaming;
    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx, { IsStreamingRole });
}

void ChatMessageListModel::setAgentStatus(int row, const QString& status)
{
    if (row < 0 || row >= m_messages.size()) return;
    m_messages[row].agentStatus = status;
    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx, { AgentStatusRole });
}

void ChatMessageListModel::setElapsedMs(int row, qint64 elapsedMs)
{
    if (row < 0 || row >= m_messages.size()) return;
    m_messages[row].elapsedMs = elapsedMs;
    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx, { ElapsedMsRole });
}

ChatMessage& ChatMessageListModel::messageRefAt(int row)
{
    static ChatMessage empty;
    if (row < 0 || row >= m_messages.size()) return empty;
    return m_messages[row];
}

void ChatMessageListModel::setMessages(const QList<ChatMessage>& msgs)
{
    beginResetModel();
    m_messages = msgs;
    endResetModel();
}

void ChatMessageListModel::clear()
{
    beginResetModel();
    m_messages.clear();
    endResetModel();
}

ChatMessage ChatMessageListModel::messageAt(int row) const
{
    if (row < 0 || row >= m_messages.size()) return {};
    return m_messages.at(row);
}
