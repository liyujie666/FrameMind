#include "service/agentservice.h"

#include "infrastructure/networkclient.h"
#include "infrastructure/imageprocessor.h"
#include "service/settingsservice.h"
#include "service/llmproviderservice.h"

#include <QJsonDocument>
#include <QUrl>
#include <QDateTime>
#include <QUuid>
#include <utility>

namespace {
constexpr int kMaxImagesPerRequest = 10;  // 架构防御：单次请求图片硬上限
}

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
    // M2 最小版：无视频上下文时给通用提示；有则附带元信息（M3 完整化）
    QString prompt = QStringLiteral(
        "你是一个专业的视频内容分析智能体。你能理解用户提供的视频画面与问题。\n"
        "# 回答格式\n"
        "- 使用 Markdown 格式\n"
        "- 引用时间点使用 [mm:ss] 或 [hh:mm:ss] 格式（如 [01:23]）\n"
        "- 不确定的内容明确标注\"推测\"或\"可能\"\n");

    if (!ctx.isEmpty()) {
        prompt += QStringLiteral("\n# 当前视频信息\n");
        if (!ctx.fileName.isEmpty())
            prompt += QStringLiteral("- 文件名: %1\n").arg(ctx.fileName);
        if (ctx.durationMs > 0)
            prompt += QStringLiteral("- 总时长(ms): %1\n").arg(ctx.durationMs);
        if (ctx.width > 0 && ctx.height > 0)
            prompt += QStringLiteral("- 分辨率: %1x%2\n").arg(ctx.width).arg(ctx.height);
        if (!ctx.videoSummary.isEmpty())
            prompt += QStringLiteral("\n# 视频摘要\n%1\n").arg(ctx.videoSummary);
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
