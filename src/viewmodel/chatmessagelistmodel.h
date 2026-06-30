#ifndef FRAMEMIND_CHATMESSAGELISTMODEL_H
#define FRAMEMIND_CHATMESSAGELISTMODEL_H

#include <QAbstractListModel>
#include <QList>

#include "model/chatmessage.h"

/**
 * 聊天消息列表模型（M2-T5）。供 View 读取消息数据。
 */
class ChatMessageListModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles {
        RoleRole = Qt::UserRole + 1,
        ContentRole,
        TimestampRole,
        IsStreamingRole,
        AttachedFramesRole,
        IdRole
    };

    explicit ChatMessageListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // 操作
    int  appendMessage(const ChatMessage& msg);   // 返回新行索引
    void updateContent(int row, const QString& content);
    void appendDeltaSilent(int row, const QString& delta);  // 累积但不发信号（配合节流）
    void flushRow(int row);                                 // 主动发 dataChanged 刷新
    void setStreaming(int row, bool streaming);
    void setMessages(const QList<ChatMessage>& msgs);
    void clear();

    int count() const { return m_messages.size(); }
    ChatMessage messageAt(int row) const;

private:
    QList<ChatMessage> m_messages;
};

#endif // FRAMEMIND_CHATMESSAGELISTMODEL_H
