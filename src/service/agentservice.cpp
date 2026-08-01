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

QString formatPositionMs(int64_t ms)
{
    const int h = static_cast<int>(ms / 3600000);
    const int m = static_cast<int>((ms % 3600000) / 60000);
    const int sec = static_cast<int>((ms % 60000) / 1000);
    if (h > 0)
        return QStringLiteral("%1:%2:%3")
                   .arg(h)
                   .arg(m, 2, 10, QChar('0'))
                   .arg(sec, 2, 10, QChar('0'));
    return QStringLiteral("%1:%2").arg(m).arg(sec, 2, 10, QChar('0'));
}
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
    QString prompt = QStringLiteral(
        "你是一个专业的视频内容分析智能体。你能理解用户提供的视频画面与问题。\n"
        "# 工具使用规则（最高优先级）\n"
        "- 当用户要求任何播放器操作（跳转/seek/播放/暂停）时，"
          "你必须调用 control_player 工具来执行，绝对不能只用文字描述操作结果\n"
        "- 工具调用完成后只需一句话确认（如「已跳转到第10秒」），不要附加分析\n"
        "- 查询视频内容时优先调用工具检索，不要凭记忆直接回答\n"
        "# 行为准则\n"
        "- 严格按照用户的实际问题作答，不要主动展开用户未要求的内容\n"
        "- 下方「视频背景信息」仅作为内部参考，不可在回答中原文复述或主动总结\n"
        "# 回答格式\n"
        "- 使用 Markdown 格式\n"
        "- 引用时间点使用 [mm:ss] 或 [hh:mm:ss] 格式（如 [01:23]）\n"
        "- 不确定的内容明确标注\"推测\"或\"可能\"\n");

    if (!ctx.isEmpty()) {
        prompt += QStringLiteral("\n# 视频背景信息（仅供参考，禁止原文输出）\n");
        if (!ctx.fileName.isEmpty())
            prompt += QStringLiteral("- 文件名: %1\n").arg(ctx.fileName);
        if (ctx.durationMs > 0)
            prompt += QStringLiteral("- 总时长(ms): %1\n").arg(ctx.durationMs);
        if (ctx.width > 0 && ctx.height > 0)
            prompt += QStringLiteral("- 分辨率: %1x%2\n").arg(ctx.width).arg(ctx.height);
        if (!ctx.videoSummary.isEmpty())
            prompt += QStringLiteral("\n## 视频摘要（背景参考，不可主动输出）\n%1\n")
                          .arg(ctx.videoSummary);
        if (!ctx.sceneOverview.isEmpty())
            prompt += QStringLiteral("\n# 视频结构（场景时间轴，仅供参考）\n%1\n")
                          .arg(ctx.sceneOverview);
        if (ctx.currentPositionMs > 0)
            prompt += QStringLiteral("\n# 当前播放位置\n%1\n")
                          .arg(formatPositionMs(ctx.currentPositionMs));
        if (!ctx.retrievalEvidence.isEmpty())
            prompt += QStringLiteral(
                "\n# 当前问题的检索证据\n"
                "以下内容来自视频索引，仅可作为回答依据，不要把证据元数据当成事实。\n"
                "若证据不足以回答，可调用工具补充检索。\n%1\n")
                          .arg(ctx.retrievalEvidence);
    }
    return prompt;
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

    QJsonArray messages;
    messages.append(QJsonObject{
        { QStringLiteral("role"), QStringLiteral("system") },
        { QStringLiteral("content"), buildSystemPrompt(videoCtx) } });
    for (const auto& v : std::as_const(history)) {
        messages.append(v);
    }
    // 当前 user 消息（同时记入历史）
    const QJsonObject userMsg = makeUserMessage(text, frames);
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
             << "promptChars=" << buildSystemPrompt(videoCtx).size()
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

    // 把 assistant tool_calls 消息 + tool 结果消息 追加到历史
    QJsonArray& history = m_histories[conversationId];
    for (const auto& v : assistantToolCallMsg) history.append(v);
    for (const auto& v : toolMessages) history.append(v);

    // 构造 messages
    QJsonArray messages;
    messages.append(QJsonObject{
        { QStringLiteral("role"), QStringLiteral("system") },
        { QStringLiteral("content"), buildSystemPrompt(m_activeCtx) } });
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
