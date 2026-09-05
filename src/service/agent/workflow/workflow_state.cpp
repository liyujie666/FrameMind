#include "workflow_state.h"

#include <QJsonDocument>

// ─── 键值存储 ────────────────────────────────────────────────────

void WorkflowState::set(const QString& key, const QVariant& value)
{
    m_data[key] = value;
}

QVariant WorkflowState::get(const QString& key, const QVariant& defaultVal) const
{
    return m_data.value(key, defaultVal);
}

bool WorkflowState::contains(const QString& key) const
{
    return m_data.contains(key);
}

void WorkflowState::remove(const QString& key)
{
    m_data.remove(key);
}

QStringList WorkflowState::keys() const
{
    return m_data.keys();
}

// ─── 消息历史 ────────────────────────────────────────────────────

QList<ChatMessage>& WorkflowState::messages()
{
    return m_messages;
}

const QList<ChatMessage>& WorkflowState::messages() const
{
    return m_messages;
}

void WorkflowState::addMessage(const ChatMessage& msg)
{
    m_messages.append(msg);
}

void WorkflowState::clearMessages()
{
    m_messages.clear();
}

// ─── 中间产物 ────────────────────────────────────────────────────

void WorkflowState::addArtifact(const QString& name, const QJsonValue& data)
{
    m_artifacts[name] = data;
}

QJsonValue WorkflowState::artifact(const QString& name) const
{
    return m_artifacts.value(name);
}

bool WorkflowState::hasArtifact(const QString& name) const
{
    return m_artifacts.contains(name);
}

QJsonObject WorkflowState::allArtifacts() const
{
    return m_artifacts;
}

void WorkflowState::mergeArtifacts(const QJsonObject& other)
{
    for (auto it = other.begin(); it != other.end(); ++it) {
        m_artifacts[it.key()] = it.value();
    }
}

// ─── 元信息 ──────────────────────────────────────────────────────

QString WorkflowState::currentNodeId() const
{
    return m_currentNode;
}

void WorkflowState::setCurrentNode(const QString& nodeId)
{
    m_currentNode = nodeId;
}

int WorkflowState::iteration() const
{
    return m_iteration;
}

void WorkflowState::incrementIteration()
{
    ++m_iteration;
}

void WorkflowState::resetIteration()
{
    m_iteration = 0;
}

// ─── 取消控制 ────────────────────────────────────────────────────

bool WorkflowState::isCancelled() const
{
    return m_cancelled.load(std::memory_order_acquire);
}

void WorkflowState::cancel()
{
    m_cancelled.store(true, std::memory_order_release);
}

void WorkflowState::resetCancel()
{
    m_cancelled.store(false, std::memory_order_release);
}

// ─── 序列化 ──────────────────────────────────────────────────────

QJsonObject WorkflowState::serialize() const
{
    QJsonObject obj;

    // 序列化键值数据
    QJsonObject dataObj;
    for (auto it = m_data.begin(); it != m_data.end(); ++it) {
        dataObj[it.key()] = QJsonValue::fromVariant(it.value());
    }
    obj["data"] = dataObj;

    // 序列化消息历史
    QJsonArray msgsArray;
    for (const auto& msg : m_messages) {
        QJsonObject msgObj;
        msgObj["id"] = msg.id;
        msgObj["role"] = ChatMessage::roleToString(msg.role);
        msgObj["content"] = msg.content;
        msgObj["timestamp"] = msg.timestamp.toString(Qt::ISODate);
        msgsArray.append(msgObj);
    }
    obj["messages"] = msgsArray;

    // 序列化产物
    obj["artifacts"] = m_artifacts;

    // 元信息
    obj["currentNode"] = m_currentNode;
    obj["iteration"] = m_iteration;

    return obj;
}

WorkflowState WorkflowState::deserialize(const QJsonObject& json)
{
    WorkflowState state;

    //恢复键值数据
    QJsonObject dataObj = json["data"].toObject();
    for (auto it = dataObj.begin(); it != dataObj.end(); ++it) {
        state.m_data[it.key()] = it.value().toVariant();
    }

    // 恢复消息历史
    QJsonArray msgsArray = json["messages"].toArray();
    for (const auto& val : msgsArray) {
        QJsonObject msgObj = val.toObject();
        ChatMessage msg;
        msg.id = msgObj["id"].toString();
        msg.role = ChatMessage::roleFromString(msgObj["role"].toString());
        msg.content = msgObj["content"].toString();
        msg.timestamp = QDateTime::fromString(msgObj["timestamp"].toString(), Qt::ISODate);
        state.m_messages.append(msg);
    }

    // 恢复产物
    state.m_artifacts = json["artifacts"].toObject();

    // 恢复元信息
    state.m_currentNode = json["currentNode"].toString();
    state.m_iteration = json["iteration"].toInt(0);

    return state;
}
