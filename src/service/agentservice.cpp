#include "service/agentservice.h"

#include "infrastructure/networkclient.h"
#include "infrastructure/imageprocessor.h"
#include "service/settingsservice.h"
#include "service/llmproviderservice.h"

#include <QJsonDocument>
#include <QUrl>
#include <QDateTime>
#include <QUuid>
#include <QDebug>
#include <utility>

namespace {
constexpr int kMaxImagesPerRequest = 10;  // 架构防御：单次请求图片硬上限
} // namespace

AgentService::AgentService(NetworkClient* network,
                           SettingsService* settings,
                           LLMProviderService* providers,
                           QObject* parent)
    : QObject(parent)
    , m_network(network)
    , m_settings(settings)
    , m_providers(providers)
{
    if (m_providers) {
        // 监听激活提供商变更
        connect(m_providers, &LLMProviderService::activeProviderChanged,
                this, &AgentService::applyActiveProvider);
        applyActiveProvider();
    }
}

void AgentService::applyActiveProvider()
{
    if (!m_providers) return;
    const LLMProvider provider = m_providers->activeProvider();
    m_endpoint = provider.fullEndpoint();
    m_model = m_providers->getModel(provider.id);
    m_apiKey = m_providers->getApiKey(provider.id);
}

void AgentService::setModel(const QString& modelName)
{
    m_model = modelName;
    // 如果使用了提供商服务，同步更新
    if (m_providers) {
        m_providers->setModel(m_providers->activeProviderId(), modelName);
    }
}

void AgentService::setEndpoint(const QString& endpoint)
{
    m_endpoint = endpoint;
}

QString AgentService::buildSystemPrompt(const VideoContext& ctx)
{
    // 向后兼容：委托给 ContextBudgetManager 的分层构建
    return ContextBudgetManager::buildStaticSystemPrompt()
           + ContextBudgetManager::buildDynamicSystemPrompt(ctx);
}

QJsonObject AgentService::makeUserMessage(const QString& text,
                                          const QList<QImage>& frames)
{
    QJsonObject msg;
    msg.insert(QStringLiteral("role"), QStringLiteral("user"));

    if (frames.isEmpty()) {
        msg.insert(QStringLiteral("content"), text);
        return msg;
    }

    // 多模态：content 为数组（text + image_url...）
    QJsonArray content;
    if (!text.isEmpty()) {
        content.append(QJsonObject{
            { QStringLiteral("type"), QStringLiteral("text") },
            { QStringLiteral("text"), text } });
    }
    int count = 0;
    for (const QImage& frame : frames) {
        if (count >= kMaxImagesPerRequest) break;
        // 缩放到 1024 内 + JPEG quality 80
        QImage scaled = (frame.width() > 1024 || frame.height() > 1024)
                            ? ImageProcessor::scaleToFit(frame, QSize(1024, 1024))
                            : frame;
        const QByteArray b64 = ImageProcessor::toBase64Jpeg(scaled, 80);
        const QString dataUri =
            QStringLiteral("data:image/jpeg;base64,") + QString::fromLatin1(b64);
        content.append(QJsonObject{
            { QStringLiteral("type"), QStringLiteral("image_url") },
            { QStringLiteral("image_url"),
              QJsonObject{ { QStringLiteral("url"), dataUri },
                           { QStringLiteral("detail"), QStringLiteral("auto") } } } });
        ++count;
    }
    msg.insert(QStringLiteral("content"), content);
    return msg;
}

QJsonObject AgentService::buildRequestPayload(const QString& convId,
                                              const QString& text,
                                              const QList<QImage>& frames,
                                              const VideoContext& videoCtx)
{
    QJsonArray& history = m_histories[convId];

    // === System Prompt 分层构建 ===
    // 静态部分（角色/规则/格式）可被后端 Prompt Caching 命中
    // 动态部分（视频背景/证据）每次可能变化
    const QString staticPrompt = ContextBudgetManager::buildStaticSystemPrompt();
    const QString dynamicPrompt = ContextBudgetManager::buildDynamicSystemPrompt(videoCtx);
    const QString fullSystemPrompt = staticPrompt + dynamicPrompt;
    const int systemTokens = m_budgetManager.estimateTextTokens(fullSystemPrompt);

    // 当前 user 消息（同时记入历史）
    const QJsonObject userMsg = makeUserMessage(text, frames);
    const int currentUserTokens = m_budgetManager.estimateMessageTokens(userMsg);

    // === Token 预算截断 ===
    applyBudgetTruncation(history, systemTokens, currentUserTokens);

    // 组装 messages
    QJsonArray messages;
    messages.append(QJsonObject{
        { QStringLiteral("role"), QStringLiteral("system") },
        { QStringLiteral("content"), fullSystemPrompt } });
    for (const auto& v : std::as_const(history)) {
        messages.append(v);
    }
    messages.append(userMsg);
    history.append(userMsg);

    QJsonObject payload;
    payload.insert(QStringLiteral("model"), m_model);
    payload.insert(QStringLiteral("stream"), true);
    payload.insert(QStringLiteral("messages"), messages);

    qDebug() << "[AgentService] buildRequestPayload"
             << "conv=" << convId
             << "historyMsgs=" << history.size()
             << "images=" << frames.size()
             << "systemTokens=" << systemTokens
             << "historyTokens=" << m_budgetManager.estimateTokens(history)
             << "evidenceChars=" << videoCtx.retrievalEvidence.size()
             << "sceneOverviewChars=" << videoCtx.sceneOverview.size();

    if (m_settings) {
        bool ok = false;
        const double temp =
            m_settings->get(QStringLiteral("llm.temperature"),
                            QStringLiteral("0.7")).toDouble(&ok);
        if (ok) payload.insert(QStringLiteral("temperature"), temp);
        const int maxTok =
            m_settings->get(QStringLiteral("llm.max_tokens"),
                            QStringLiteral("2048")).toInt(&ok);
        if (ok) payload.insert(QStringLiteral("max_tokens"), maxTok);
    }
    return payload;
}

void AgentService::sendMessage(const QString& conversationId,
                               const QString& text,
                               const QList<QImage>& frames,
                               const VideoContext& videoCtx)
{
    if (!m_network) {
        emit responseError(conversationId, tr("网络组件未初始化"));
        return;
    }

    // 重新应用当前提供商配置（确保使用最新的 API Key）
    applyActiveProvider();

    // 安全：API Key 从密钥服务取，禁止落库/打印
    if (m_apiKey.isEmpty()) {
        emit responseError(conversationId,
                           tr("未配置 API Key，请在「设置 → AI」中选择提供商并填写 API Key"));
        return;
    }
    m_network->setAuthToken(m_apiKey);

    const QJsonObject payload =
        buildRequestPayload(conversationId, text, frames, videoCtx);

    m_currentConvId = conversationId;
    m_accumulated.clear();
    m_streaming = true;

    QString base = m_endpoint;
    while (base.endsWith('/')) base.chop(1);
    const QUrl url(base + QStringLiteral("/chat/completions"));

    m_network->streamPost(
        url, payload,
        // onChunk
        [this](const QString& delta) {
            m_accumulated += delta;
            emit responseChunk(m_currentConvId, delta);
        },
        // onDone
        [this]() {
            m_streaming = false;
            // 记入历史
            m_histories[m_currentConvId].append(QJsonObject{
                { QStringLiteral("role"), QStringLiteral("assistant") },
                { QStringLiteral("content"), m_accumulated } });
            ChatMessage msg;
            msg.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            msg.role = ChatMessage::Assistant;
            msg.content = m_accumulated;
            msg.timestamp = QDateTime::currentDateTime();
            msg.isStreaming = false;
            emit responseFinished(m_currentConvId, msg);
        },
        // onError
        [this](const QString& err) {
            m_streaming = false;
            emit responseError(m_currentConvId, err);
        });
}

void AgentService::stopGeneration()
{
    if (!m_streaming) return;
    if (m_network) m_network->cancelStream();
    m_streaming = false;

    // 把已接收的部分作为最终结果落地（标记非流式）
    m_histories[m_currentConvId].append(QJsonObject{
        { QStringLiteral("role"), QStringLiteral("assistant") },
        { QStringLiteral("content"), m_accumulated } });
    ChatMessage msg;
    msg.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    msg.role = ChatMessage::Assistant;
    msg.content = m_accumulated;
    msg.timestamp = QDateTime::currentDateTime();
    msg.isStreaming = false;
    emit responseFinished(m_currentConvId, msg);
}

void AgentService::seedHistory(const QString& conversationId,
                               const QList<ChatMessage>& messages)
{
    QJsonArray arr;
    for (const ChatMessage& m : messages) {
        if (m.role == ChatMessage::System) continue;  // system 由本服务统一生成
        // 历史仅回灌文本（图片不重复上送，控制体积）
        arr.append(QJsonObject{
            { QStringLiteral("role"), ChatMessage::roleToString(m.role) },
            { QStringLiteral("content"), m.content } });
    }
    m_histories.insert(conversationId, arr);
}

void AgentService::clearHistory(const QString& conversationId)
{
    m_histories.remove(conversationId);
}

// ============================================================
// Tool Calling 版本（M4）
// ============================================================

void AgentService::sendMessageWithTools(const QString& conversationId,
                                         const QString& text,
                                         const QList<QImage>& frames,
                                         const VideoContext& videoCtx,
                                         const QJsonArray& tools,
                                         const QJsonValue& toolChoice)
{
    if (!m_network) {
        emit responseError(conversationId, tr("网络组件未初始化"));
        return;
    }
    applyActiveProvider();
    if (m_apiKey.isEmpty()) {
        emit responseError(conversationId,
                            tr("未配置 API Key，请在设置中填写"));
        return;
    }
    m_network->setAuthToken(m_apiKey);

    if (!videoCtx.isEmpty()) m_activeCtx = videoCtx;
    const VideoContext& ctx = videoCtx.isEmpty() ? m_activeCtx : videoCtx;

    QJsonObject payload = buildRequestPayload(conversationId, text, frames, ctx);
    if (!tools.isEmpty()) {
        payload.insert(QStringLiteral("tools"), tools);
        payload.insert(QStringLiteral("tool_choice"), toolChoice);
    }
    sendStreamWithTools(conversationId, payload);
}

void AgentService::continueWithToolResults(const QString& conversationId,
                                             const QJsonArray& assistantToolCallMsg,
                                             const QJsonArray& toolMessages,
                                             const QJsonArray& tools)
{
    if (!m_network) {
        emit responseError(conversationId, tr("网络组件未初始化"));
        return;
    }
    applyActiveProvider();
    m_network->setAuthToken(m_apiKey);

    // === P0: 压缩 tool 结果体积 ===
    QJsonArray compressedToolMessages = toolMessages;
    m_budgetManager.compressToolResults(compressedToolMessages);

    // 把 assistant tool_calls 消息 + 压缩后的 tool 结果消息 追加到历史
    QJsonArray& history = m_histories[conversationId];
    for (const auto& v : assistantToolCallMsg) history.append(v);
    for (const auto& v : compressedToolMessages) history.append(v);

    // === Token 预算截断 ===
    // 注意：不截断最近的 assistant tool_calls + tool 消息对，否则 API 会返回 400。
    // 只对早期历史做截断（truncateHistory 已内置尾部保护）。
    const QString systemContent = ContextBudgetManager::buildStaticSystemPrompt()
                                  + ContextBudgetManager::buildDynamicSystemPrompt(m_activeCtx);
    const int systemTokens = m_budgetManager.estimateTextTokens(systemContent);
    applyBudgetTruncation(history, systemTokens, 0);

    // 安全检查：确保截断后 history 中最后的 tool 消息有对应的 assistant tool_calls
    // 如果截断导致消息对不完整，直接用原始消息构建请求
    bool historyValid = true;
    if (!history.isEmpty()) {
        // 检查是否存在 tool 消息没有对应的 assistant tool_calls
        for (int i = 0; i < history.size(); ++i) {
            const QString role = history.at(i).toObject()
                                     .value(QStringLiteral("role")).toString();
            if (role == QLatin1String("tool")) {
                // 往前找是否有 assistant tool_calls 消息
                bool foundAssistant = false;
                for (int j = i - 1; j >= 0; --j) {
                    const QJsonObject msg = history.at(j).toObject();
                    if (msg.value(QStringLiteral("role")).toString() == QLatin1String("assistant")
                        && !msg.value(QStringLiteral("tool_calls")).toArray().isEmpty()) {
                        foundAssistant = true;
                        break;
                    }
                    if (msg.value(QStringLiteral("role")).toString() == QLatin1String("user"))
                        break;
                }
                if (!foundAssistant) {
                    historyValid = false;
                    break;
                }
            }
        }
    }

    if (!historyValid) {
        // 截断破坏了消息结构，重建最小历史：只保留当前轮的 tool 调用
        qWarning() << "[AgentService] truncation broke tool message pairs, rebuilding minimal history";
        history = QJsonArray();
        for (const auto& v : assistantToolCallMsg) history.append(v);
        for (const auto& v : compressedToolMessages) history.append(v);
    }

    // 构造 messages
    QJsonArray messages;
    messages.append(QJsonObject{
        { QStringLiteral("role"), QStringLiteral("system") },
        { QStringLiteral("content"), systemContent } });
    for (const auto& v : std::as_const(history)) messages.append(v);

    QJsonObject payload;
    payload.insert(QStringLiteral("model"), m_model);
    payload.insert(QStringLiteral("stream"), true);
    payload.insert(QStringLiteral("messages"), messages);
    if (!tools.isEmpty()) {
        payload.insert(QStringLiteral("tools"), tools);
        payload.insert(QStringLiteral("tool_choice"), QStringLiteral("auto"));
    }
    sendStreamWithTools(conversationId, payload);
}

void AgentService::sendStreamWithTools(const QString& convId,
                                          const QJsonObject& payload)
{
    m_currentConvId       = convId;
    m_accumulated.clear();
    m_pendingToolCalls    = QJsonArray();
    m_pendingFinishReason.clear();
    m_streaming = true;

    QString base = m_endpoint;
    while (base.endsWith('/')) base.chop(1);
    const QUrl url(base + QStringLiteral("/chat/completions"));

    m_network->streamPostRaw(
        url, payload,
        // onChoice: 完整 delta 对象
        [this](const QJsonObject& choice) {
            const QJsonObject delta = choice.value(QStringLiteral("delta")).toObject();
            const QString content = delta.value(QStringLiteral("content")).toString();
            if (!content.isEmpty()) {
                m_accumulated += content;
                emit responseChunk(m_currentConvId, content);
            }
            // 累积 tool_calls 增量
            const QJsonArray tcArr = delta.value(QStringLiteral("tool_calls")).toArray();
            for (const auto& v : tcArr) {
                const QJsonObject tc = v.toObject();
                const int index = tc.value(QStringLiteral("index")).toInt(0);
                // 扩容
                while (m_pendingToolCalls.size() <= index) {
                    m_pendingToolCalls.append(QJsonObject{});
                }
                QJsonObject slot = m_pendingToolCalls.at(index).toObject();
                if (tc.contains(QStringLiteral("id"))) {
                    slot.insert(QStringLiteral("id"), tc.value(QStringLiteral("id")));
                }
                const QJsonObject fn = tc.value(QStringLiteral("function")).toObject();
                if (!fn.isEmpty()) {
                    QJsonObject slotFn = slot.value(QStringLiteral("function")).toObject();
                    if (fn.contains(QStringLiteral("name"))) {
                        slotFn.insert(QStringLiteral("name"), fn.value(QStringLiteral("name")));
                    }
                    if (fn.contains(QStringLiteral("arguments"))) {
                        const QString add = fn.value(QStringLiteral("arguments")).toString();
                        const QString cur = slotFn.value(QStringLiteral("arguments")).toString();
                        slotFn.insert(QStringLiteral("arguments"), cur + add);
                    }
                    slot.insert(QStringLiteral("function"), slotFn);
                }
                slot.insert(QStringLiteral("type"), QStringLiteral("function"));
                m_pendingToolCalls.replace(index, slot);
            }
            const QString fr = choice.value(QStringLiteral("finish_reason")).toString();
            if (!fr.isEmpty()) m_pendingFinishReason = fr;
        },
        // onDone
        [this]() {
            m_streaming = false;
            // tool_calls 轮次的 assistant 消息由 continueWithToolResults 回填；
            // stop/length 时在此写入最终 assistant 文本，保证多轮历史闭环。
            if (m_pendingFinishReason == QLatin1String("stop")
                || m_pendingFinishReason == QLatin1String("length")
                || (m_pendingFinishReason != QLatin1String("tool_calls")
                    && !m_accumulated.isEmpty())) {
                m_histories[m_currentConvId].append(QJsonObject{
                    { QStringLiteral("role"), QStringLiteral("assistant") },
                    { QStringLiteral("content"), m_accumulated } });
            }
            qDebug() << "[AgentService] streamDone"
                     << "conv=" << m_currentConvId
                     << "finish=" << m_pendingFinishReason
                     << "toolCalls=" << m_pendingToolCalls.size()
                     << "answerChars=" << m_accumulated.size();
            emit responseFinishedWithTools(m_currentConvId,
                                            m_pendingToolCalls,
                                            m_pendingFinishReason,
                                            m_accumulated);
        },
        // onError
        [this](const QString& err) {
            m_streaming = false;
            emit responseError(m_currentConvId, err);
        });
}

// ============================================================
// Token 预算管理
// ============================================================

void AgentService::applyBudgetTruncation(QJsonArray& history,
                                          int systemTokens,
                                          int currentUserTokens)
{
    const int before = m_budgetManager.estimateTokens(history);
    const int after = m_budgetManager.truncateHistory(history, systemTokens, currentUserTokens);
    if (before != after) {
        qDebug() << "[AgentService] history truncated"
                 << "before=" << before << "after=" << after
                 << "msgs=" << history.size();
    }
}
