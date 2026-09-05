#ifndef FRAMEMIND_CONVERSATION_H
#define FRAMEMIND_CONVERSATION_H

#include <QString>
#include <QDateTime>
#include <QMetaType>

/// 对话领域模型（architecture-design.md §3.3.1 / §八 conversations 表）
struct Conversation {
    QString   id;
    QString   title = QStringLiteral("新对话");
    QString   videoFilePath;
    QString   videoId;
    QDateTime createdAt;
    QDateTime updatedAt;
};

Q_DECLARE_METATYPE(Conversation)

#endif // FRAMEMIND_CONVERSATION_H
