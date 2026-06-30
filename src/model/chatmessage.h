#ifndef FRAMEMIND_CHATMESSAGE_H
#define FRAMEMIND_CHATMESSAGE_H

#include <QString>
#include <QList>
#include <QImage>
#include <QDateTime>
#include <QMetaType>

/// 聊天消息领域模型（architecture-design.md §3.3.1）
struct ChatMessage {
    enum Role { User, Assistant, System };

    QString        id;
    Role           role = User;
    QString        content;          // Markdown 文本
    QList<QImage>  attachedFrames;   // 附带的视频帧
    QDateTime      timestamp;
    bool           isStreaming = false;

    static QString roleToString(Role r)
    {
        switch (r) {
        case Assistant: return QStringLiteral("assistant");
        case System:    return QStringLiteral("system");
        case User:
        default:        return QStringLiteral("user");
        }
    }

    static Role roleFromString(const QString& s)
    {
        if (s == QLatin1String("assistant")) return Assistant;
        if (s == QLatin1String("system"))    return System;
        return User;
    }
};

Q_DECLARE_METATYPE(ChatMessage)

#endif // FRAMEMIND_CHATMESSAGE_H
