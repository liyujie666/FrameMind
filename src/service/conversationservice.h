#ifndef FRAMEMIND_CONVERSATIONSERVICE_H
#define FRAMEMIND_CONVERSATIONSERVICE_H

#include <QObject>
#include <QList>

#include "model/conversation.h"
#include "model/chatmessage.h"

class DatabaseManager;

/**
 * 会话与消息持久化（M2-T4）。所有方法走 DatabaseManager 绑定接口，禁止拼 SQL。
 */
class ConversationService : public QObject {
    Q_OBJECT
public:
    explicit ConversationService(DatabaseManager* db, QObject* parent = nullptr);

    QList<Conversation> getAllConversations();
    QList<Conversation> getConversationsByVideoId(const QString& videoId);
    Conversation        getLatestConversationForVideo(const QString& videoId);
    Conversation        createConversation(const QString& videoPath = {}, const QString& videoId = {});
    void                deleteConversation(const QString& convId);
    void                updateTitle(const QString& convId, const QString& title);

    QList<ChatMessage>  getMessages(const QString& convId);
    void                saveMessage(const QString& convId, const ChatMessage& msg);
    void                updateMessage(const QString& msgId, const QString& content);

private:
    DatabaseManager* m_db = nullptr;
};

#endif // FRAMEMIND_CONVERSATIONSERVICE_H
