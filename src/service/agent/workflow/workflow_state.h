#pragma once

#include <QString>
#include <QVariant>
#include <QVariantMap>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QList>
#include <atomic>

#include "model/chatmessage.h"

/**
 * @brief 工作流共享状态容器
 *
 * 跨节点流转的状态，支持键值存储、消息历史、中间产物、序列化/反序列化。
 * 每个节点执行时可读写此状态，用于在节点间传递数据。
 */
class WorkflowState
{
public:
    WorkflowState() = default;

    //─── 键值存储 ───────────────────────────────────────────────

    void set(const QString& key, const QVariant& value);
    QVariant get(const QString& key, const QVariant& defaultVal = {}) const;
    bool contains(const QString& key) const;
    void remove(const QString& key);
    QStringList keys() const;

    // ─── 消息历史 ───────────────────────────────────────────────

    QList<ChatMessage>& messages();
    const QList<ChatMessage>& messages() const;
    void addMessage(const ChatMessage& msg);
    void clearMessages();

    // ─── 中间产物（Tool 结果、检索结果等） ─────────────────────

    void addArtifact(const QString& name, const QJsonValue& data);
    QJsonValue artifact(const QString& name) const;
    bool hasArtifact(const QString& name) const;
    QJsonObject allArtifacts() const;
    void mergeArtifacts(const QJsonObject& other);

    // ─── 元信息 ─────────────────────────────────────────────────

    QString currentNodeId() const;
    void setCurrentNode(const QString& nodeId);

    int iteration() const;
    void incrementIteration();
    void resetIteration();

    // ─── 取消控制 ───────────────────────────────────────────────

    bool isCancelled() const;
    void cancel();
    void resetCancel();

    // ─── 序列化 ─────────────────────────────────────────────────

    QJsonObject serialize() const;
    static WorkflowState deserialize(const QJsonObject& json);

private:
    QVariantMap m_data;
    QList<ChatMessage> m_messages;
    QJsonObject m_artifacts;
    QString m_currentNode;
    int m_iteration = 0;
    std::atomic<bool> m_cancelled{false};
};
