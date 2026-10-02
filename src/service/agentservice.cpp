#include "util/video_rag_log.h"
#include "service/agentservice.h"
#include <QCryptographicHash>

QString AgentService::modelSignature() const {return QString::fromLatin1(QCryptographicHash::hash((m_model+"\n"+m_endpoint).toUtf8(),QCryptographicHash::Sha256).toHex());}
#include "infrastructure/networkclient.h"
#include "infrastructure/imageprocessor.h"
#include "service/settingsservice.h"
#include "service/llmproviderservice.h"
#include "model/build_request_policy.h"
#include <QJsonDocument>
#include <QSet>
#include <QUrl>
#include <QDateTime>
#include <QUuid>
#include <QDebug>
#include <QElapsedTimer>
#include <utility>
#include <limits>

namespace {
constexpr int kMaxImagesPerRequest = 10;  // 架构防御：单次请求图片硬上限

bool isDashScope(const QString& endpoint)
{
    const QString host = QUrl(endpoint).host().toLower();
    return host == QStringLiteral("dashscope.aliyuncs.com")
        || host == QStringLiteral("dashscope-intl.aliyuncs.com")
        || host == QStringLiteral("dashscope-us.aliyuncs.com");
}

bool supportsThinkingSwitch(const QString& model)
{
    const auto name = model.toLower();
    if (name.contains(QStringLiteral("thinking"))) return false;
    return name.startsWith(QStringLiteral("qwen3.8-"))
        || name.startsWith(QStringLiteral("qwen3.7-"))
        || name.startsWith(QStringLiteral("qwen3.6-"))
        || name.startsWith(QStringLiteral("qwen3.5-"));
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
    if (m_network) {
        connect(m_network, &NetworkClient::streamMetadata, this,
                [this](int status, qint64 retryMs, const QJsonObject& usage) {
            if (!m_streaming || !m_oneShotActive) return;
            if (status > 0) m_httpStatus = status;
            if (retryMs >= 0) m_retryAfterMs = retryMs;
            if (!usage.isEmpty()) m_buildUsage = usage;
        });
        connect(m_network, &NetworkClient::streamActivity, this, [this](qint64 bytes, int status) {
            if (!m_streaming || !m_oneShotActive) return;
            if (status > 0) m_httpStatus = status;
            if (bytes > 0) {
                if (m_firstByteMs < 0) {
                    m_firstByteMs = m_requestTimer.elapsed();
                    qCDebug(ragDetailLog) << "[AgentService][Build] first_byte" << m_currentConvId
                             << "latency_ms=" << m_firstByteMs << "http_status=" << m_httpStatus;
                }
                m_receivedBytes += bytes;
                emit responseActivity(m_currentConvId);
            }
        });
    }
    if (m_providers) {
        // 监听激活提供商变更
        connect(m_providers, &LLMProviderService::activeProviderChanged,
                this, &AgentService::applyActiveProvider);
        connect(m_providers, &LLMProviderService::providerUpdated, this,
                [this](const QString& id) {
                    if (id == m_providers->activeProviderId()) applyActiveProvider();
                });
        applyActiveProvider();
    }
}

void AgentService::applyActiveProvider()
{
    if (!m_providers) return;
    const LLMProvider provider = m_providers->activeProvider();
    m_endpoint = m_providers->getEndpoint(provider.id);
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
                                          const QList<QImage>& frames, const ImageEncodingOptions& imageOptions)
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
        QSize encodedSize;
        QByteArray b64;
        if (count < imageOptions.preparedImages.size()) {
            const auto& prepared = imageOptions.preparedImages[count];
            encodedSize = prepared.size;
            b64 = prepared.jpeg.toBase64(); // preserve final grid pixels and JPEG quality exactly
        } else {
            QImage scaled = (frame.width() > imageOptions.maxEdge || frame.height() > imageOptions.maxEdge)
                ? ImageProcessor::scaleToFit(frame, QSize(imageOptions.maxEdge, imageOptions.maxEdge)) : frame;
            encodedSize = scaled.size();
            b64 = ImageProcessor::toBase64Jpeg(scaled, imageOptions.jpegQuality);
        }
        const QString dataUri =
            QStringLiteral("data:image/jpeg;base64,") + QString::fromLatin1(b64);

        // P0修复：添加图片尺寸信息用于精确token估算
        QJsonObject imageContent;
        imageContent.insert(QStringLiteral("type"), QStringLiteral("image_url"));
        imageContent.insert(QStringLiteral("image_url"),
                            QJsonObject{
                                { QStringLiteral("url"), dataUri },
                                { QStringLiteral("detail"), QStringLiteral("auto") }
                            });
        // 添加尺寸元数据（不影响API调用，仅用于本地token估算）
        imageContent.insert(QStringLiteral("width"), encodedSize.width());
        imageContent.insert(QStringLiteral("height"), encodedSize.height());

        content.append(imageContent);
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
    QJsonArray& history = getOrCreateHistory(convId);
    // === P1修复：VideoContext静态部分复用 ===
    // 检查当前会话是否已缓存了VideoContext静态部分
    HistoryEntry& entry = m_historiesLRU[convId];
    const bool videoChanged = (entry.cachedVideoId != videoCtx.videoId || entry.cachedBuildId!=videoCtx.buildId || entry.cachedBuildRevision!=videoCtx.buildRevision);
    const bool needUpdateStaticContext = videoChanged
                                         || entry.cachedVideoSummary.isEmpty()
                                         || entry.cachedSceneOverview != videoCtx.sceneOverview
                                         || entry.cachedEntityContext != videoCtx.entityContext;

    QString dynamicPrompt;
    if (needUpdateStaticContext) {
        // 视频切换或首次，需要完整的动态部分
        dynamicPrompt = ContextBudgetManager::buildDynamicSystemPrompt(videoCtx);

        // 缓存静态部分
        entry.cachedVideoSummary = videoCtx.videoSummary;
        entry.cachedSceneOverview = videoCtx.sceneOverview;
        entry.cachedBuildId=videoCtx.buildId;entry.cachedBuildRevision=videoCtx.buildRevision;
        entry.cachedEntityContext = videoCtx.entityContext;
        entry.cachedVideoId = videoCtx.videoId;

        qDebug() << "[AgentService] 更新VideoContext静态缓存"
                 << "会话=" << convId
                 << "视频切换=" << videoChanged
                 << "摘要字符=" << videoCtx.videoSummary.size()
                 << "场景概览字符=" << videoCtx.sceneOverview.size()
                 << "实体上下文字符=" << videoCtx.entityContext.size();
    } else {
        // 复用缓存的静态部分，只注入动态部分（检索证据+当前位置）
        VideoContext dynamicOnly;
        dynamicOnly.retrievalEvidence = videoCtx.retrievalEvidence;
        dynamicOnly.currentPositionMs = videoCtx.currentPositionMs;
        // 保留缓存的静态部分（避免重复注入）
        dynamicPrompt = ContextBudgetManager::buildDynamicSystemPrompt(dynamicOnly);

        qDebug() << "[AgentService] 复用VideoContext静态缓存"
                 << "会话=" << convId
                 << "只注入检索证据=" << videoCtx.retrievalEvidence.size() << "字符";
    }
    // === System Prompt 分层构建 ===
    // 静态部分（角色/规则/格式）可被后端 Prompt Caching 命中
    // 动态部分（视频背景/证据）每次可能变化
    const QString staticPrompt = ContextBudgetManager::buildStaticSystemPrompt();
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
    qDebug() << "[AgentService] 构建模型请求"
             << "会话=" << convId
             << "历史消息数=" << history.size()
             << "图片数=" << frames.size()
             << "系统提示词Token数=" << systemTokens
             << "历史Token数=" << m_budgetManager.estimateTokens(history)
             << "证据字符数=" << videoCtx.retrievalEvidence.size()
             << "场景概览字符数=" << videoCtx.sceneOverview.size();
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

    // 开始计时：Payload构建
    m_payloadBuildTimer.start();
    const QJsonObject payload =
        buildRequestPayload(conversationId, text, frames, videoCtx);
    const qint64 payloadBuildMs = m_payloadBuildTimer.elapsed();

    qDebug() << "[AgentService][Timing] request_payload_built"
             << "convId=" << conversationId
             << "mode=plain"
             << "images=" << frames.size()
             << "evidenceChars=" << videoCtx.retrievalEvidence.size()
             << "payload_build_ms=" << payloadBuildMs;

    m_currentConvId = conversationId;
    m_oneShotActive = false;
    m_firstChunkArrived = false;
    m_ttftMs = 0;
    m_requestTimer.start();

    qDebug() << "[AgentService][Timing] stream_request_start"
             << "convId=" << conversationId
             << "mode=plain"
             << "images=" << frames.size();

    m_accumulated.clear();
    m_streaming = true;
    QString base = m_endpoint;
    while (base.endsWith('/')) base.chop(1);
    const QUrl url(base + QStringLiteral("/chat/completions"));
    m_network->streamPost(
        url, payload,
        // onChunk
        [this](const QString& delta) {
            // 首包到达统计
            if (!m_firstChunkArrived) {
                m_firstChunkArrived = true;
                m_ttftMs = m_requestTimer.elapsed();
                qDebug() << "[AgentService][Timing] first_chunk_received"
                         << "convId=" << m_currentConvId
                         << "TTFT_ms=" << m_ttftMs;
            }
            m_accumulated += delta;
            emit responseChunk(m_currentConvId, delta);
        },
        // onDone
        [this]() {
            m_streaming = false;
            const qint64 totalLatencyMs = m_requestTimer.elapsed();
            const int outputTokens = m_budgetManager.estimateTextTokens(m_accumulated);
            const qint64 generationTimeMs = totalLatencyMs - m_ttftMs;
            const double tokensPerSecond = generationTimeMs > 0
                                               ? (outputTokens * 1000.0) / generationTimeMs
                                               : 0.0;

            qDebug() << "[AgentService][Timing] stream_request_finished"
                     << "convId=" << m_currentConvId
                     << "mode=plain"
                     << "total_latency_ms=" << totalLatencyMs
                     << "TTFT_ms=" << m_ttftMs
                     << "generation_time_ms=" << generationTimeMs
                     << "output_tokens=" << outputTokens
                     << "tokens_per_second=" << QString::number(tokensPerSecond, 'f', 2);

            // 记入历史
            QJsonArray& history = getOrCreateHistory(m_currentConvId);
            history.append(QJsonObject{
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
            const qint64 errorLatencyMs = m_requestTimer.elapsed();
            qDebug() << "[AgentService][Timing] stream_request_error"
                     << "convId=" << m_currentConvId
                     << "error_latency_ms=" << errorLatencyMs
                     << "error=" << err;
            emit responseError(m_currentConvId, err);
        });
}

void AgentService::sendOneShot(const QString& conversationId, const QString& systemPrompt,
                               const QString& text, const QList<QImage>& frames, const ImageEncodingOptions& imageOptions)
{
    m_retryAfterMs = -1;
    m_buildUsage = {};
    m_httpStatus = 0;
    m_buildImageDiagnostics = {};
    m_buildRequestStartedAtMs = 0;
    m_requestTimer.invalidate();
    m_firstByteMs = m_firstContentMs = -1;
    m_receivedBytes = m_reasoningChars = 0;
    if (!m_network) {
        emit responseError(conversationId, tr("网络组件未初始化"));
        return;
    }
    applyActiveProvider();
    if (m_apiKey.isEmpty()) {
        emit responseError(conversationId, tr("未配置 API Key，请检查 AI 提供商设置"));
        return;
    }
    if (frames.size() > kMaxImagesPerRequest) {
        emit responseError(conversationId, tr("证据图片超过单次请求上限，请拆分证据页"));
        return;
    }
    if (imageOptions.maxEdge < 1 || imageOptions.maxEdge > 8192 || imageOptions.jpegQuality < 1 ||
        imageOptions.jpegQuality > 100 ||
        ((imageOptions.requirePrepared || !imageOptions.preparedImages.isEmpty()) &&
         imageOptions.preparedImages.size() != frames.size())) {
        emit responseError(conversationId, tr("invalid_image_encoding: 图片编码参数或数量无效"));
        return;
    }
    qint64 encodedBytes = 0;
    QJsonArray encodedSizes;
    for (int i = 0; i < imageOptions.preparedImages.size(); ++i) {
        const auto& prepared = imageOptions.preparedImages[i];
        if (prepared.jpeg.isEmpty() || !prepared.size.isValid() || prepared.size != frames[i].size() ||
            prepared.size.width() > imageOptions.maxEdge || prepared.size.height() > imageOptions.maxEdge) {
            emit responseError(conversationId, tr("invalid_image_encoding: 预编码图片无效或尺寸不一致"));
            return;
        }
        encodedBytes += prepared.jpeg.size();
        encodedSizes.append(QJsonObject{{"width", prepared.size.width()}, {"height", prepared.size.height()}});
    }
    m_buildImageDiagnostics = {{"transmitted_images", frames.size()}, {"prepared_image_bytes", encodedBytes},
                              {"prepared_image_sizes", encodedSizes}};
    // Send only the task contract and its evidence. Chat formatting/tool rules do not apply.
    auto user = makeUserMessage(text, frames, imageOptions);
    if (user["content"].isArray()) {
        QJsonArray content;
        for (auto part : user["content"].toArray()) {
            auto object = part.toObject();
            object.remove("width");
            object.remove("height");
            content.append(object);
        }
        user["content"] = content;
    }
    const int maxTokens = qBound(1024, m_settings
        ? m_settings->get("llm.build_max_tokens", "4096").toInt() : 4096, 16384);
    QJsonObject payload{{"model", m_model}, {"stream", true}, {"temperature", 0.1},
                        {"max_tokens", maxTokens},
                        {"messages", QJsonArray{QJsonObject{{"role", "system"}, {"content", systemPrompt}}, user}}};
    // Vendor extension belongs only to supported hybrid models on DashScope.
    const bool disableThinking = isDashScope(m_endpoint) && supportsThinkingSwitch(m_model);
    if (disableThinking) payload.insert("enable_thinking", false);
    m_currentConvId = conversationId;
    m_accumulated.clear();
    m_pendingFinishReason.clear();
    m_streaming = true;
    m_oneShotActive = true;
    m_receivedBytes = m_reasoningChars = 0;
    m_firstByteMs = m_firstContentMs = -1;
    m_httpStatus = 0;
    m_requestTimer.start();
    qCDebug(ragDetailLog) << "[AgentService][Build] request_start" << conversationId << "model=" << m_model
             << "frames=" << frames.size() << "input_chars=" << systemPrompt.size() + text.size()
             << "max_tokens=" << maxTokens << "thinking_disabled=" << disableThinking
             << "idle_timeout_ms=" << buildIdleTimeoutMs() << "total_timeout_ms=" << buildTotalTimeoutMs();
    m_network->setAuthToken(m_apiKey);
    QString base = m_endpoint;
    while (base.endsWith('/')) base.chop(1);
    m_buildRequestStartedAtMs = QDateTime::currentMSecsSinceEpoch();
    m_network->streamPostRaw(QUrl(base + "/chat/completions"), payload,
        [this, conversationId](const QJsonObject& choice) {
            if (!m_streaming || m_currentConvId != conversationId) return;
            const auto delta = choice["delta"].toObject();
            m_reasoningChars += delta["reasoning_content"].toString().size();
            const auto content = delta["content"].toString();
            if (!content.isEmpty() && m_firstContentMs < 0) m_firstContentMs = m_requestTimer.elapsed();
            m_accumulated += content;
            if (choice["finish_reason"].isString())
                m_pendingFinishReason = choice["finish_reason"].toString();
        },
        [this, conversationId] {
            if (!m_streaming || m_currentConvId != conversationId) return;
            m_streaming = false;
            qCDebug(ragDetailLog) << "[AgentService][Build] request_finished" << conversationId
                     << "finish_reason=" << m_pendingFinishReason << requestDiagnostics();
            if (m_pendingFinishReason != "stop") {
                emit responseError(conversationId, m_pendingFinishReason == "length"
                    ? tr("output_truncated: 模型输出达到长度上限，请减小证据页或增加构建输出额度")
                    : tr("incomplete_response: 模型未正常结束（%1）").arg(m_pendingFinishReason));
                return;
            }
            if (m_accumulated.trimmed().isEmpty()) {
                emit responseError(conversationId, tr("empty_response: 模型未返回正文；%1").arg(requestDiagnostics()));
                return;
            }
            ChatMessage message;
            message.role = ChatMessage::Assistant;
            message.content = m_accumulated;
            message.timestamp = QDateTime::currentDateTime();
            emit responseFinished(conversationId, message);
        },
        [this, conversationId](QString error) {
            if (!m_streaming || m_currentConvId != conversationId) return;
            m_streaming = false;
            if (!m_apiKey.isEmpty()) error.replace(m_apiKey, "[redacted]");
            const auto detail = error.left(500) + QStringLiteral("；") + requestDiagnostics();
            qWarning() << "[AgentService][Build] request_error" << conversationId << detail;
            emit responseError(conversationId, detail);
        }, buildIdleTimeoutMs(), !isDashScope(m_endpoint));
}

int AgentService::buildIdleTimeoutMs() const
{
    bool ok = false;
    const int value = m_settings ? m_settings->get("llm.build_idle_timeout_ms").toInt(&ok) : 0;
    return ok ? qBound(10000, value, 300000) : BuildRequestPolicy::IdleTimeoutMs;
}

ModelReply AgentService::buildReplyMetadata() const
{
    ModelReply reply;
    reply.httpStatus = m_httpStatus;
    reply.retryAfterMs = m_retryAfterMs;
    reply.diagnostics = {{"elapsed_ms", m_requestTimer.isValid() ? m_requestTimer.elapsed() : 0},
                         {"request_started_at_ms", m_buildRequestStartedAtMs},
                         {"first_byte_ms", m_firstByteMs}, {"first_content_ms", m_firstContentMs},
                         {"received_bytes", m_receivedBytes}, {"usage_available", !m_buildUsage.isEmpty()}};
    if (!m_buildUsage.isEmpty()) reply.diagnostics["usage"] = m_buildUsage;
    for (auto it = m_buildImageDiagnostics.begin(); it != m_buildImageDiagnostics.end(); ++it)
        reply.diagnostics[it.key()] = it.value();
    return reply;
}

int AgentService::buildTotalTimeoutMs() const
{
    bool ok = false;
    const int value = m_settings ? m_settings->get("llm.build_total_timeout_ms").toInt(&ok) : 0;
    return qMax(buildIdleTimeoutMs(), ok ? qBound(10000, value, 1800000) : BuildRequestPolicy::TotalTimeoutMs);
}

QString AgentService::requestDiagnostics() const
{
    return QStringLiteral("elapsed_ms=%1 http=%2 bytes=%3 first_byte_ms=%4 first_content_ms=%5 reasoning_chars=%6 content_chars=%7")
        .arg(m_requestTimer.isValid() ? m_requestTimer.elapsed() : 0).arg(m_httpStatus)
        .arg(m_receivedBytes).arg(m_firstByteMs).arg(m_firstContentMs).arg(m_reasoningChars).arg(m_accumulated.size());
}

void AgentService::abortRequest(const QString& conversationId, const QString& reason)
{
    if (!m_streaming || m_currentConvId != conversationId) return;
    m_streaming = false;
    if (m_network) m_network->cancelStream();
    const QString detail = m_oneShotActive ? reason + QStringLiteral("；") + requestDiagnostics() : reason;
    if (m_oneShotActive) qWarning() << "[AgentService][Build] request_aborted" << conversationId << detail;
    emit responseError(conversationId, detail);
}

void AgentService::stopGeneration()
{
    if (!m_streaming) return;
    if (m_network) m_network->cancelStream();
    m_streaming = false;

    const qint64 totalLatencyMs = m_requestTimer.elapsed();
    const int outputTokens = m_budgetManager.estimateTextTokens(m_accumulated);
    qDebug() << "[AgentService][Timing] generation_stopped"
             << "convId=" << m_currentConvId
             << "total_latency_ms=" << totalLatencyMs
             << "TTFT_ms=" << m_ttftMs
             << "output_tokens=" << outputTokens
             << "stop_reason=user_cancel";

    // 把已接收的部分作为最终结果落地（标记非流式）
    QJsonArray& history = getOrCreateHistory(m_currentConvId);
    history.append(QJsonObject{
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

    // 使用LRU缓存
    evictLRUIfNeeded();
    m_historiesLRU[conversationId] = { arr, QDateTime::currentMSecsSinceEpoch() };

    // 保持向后兼容
    m_histories.insert(conversationId, arr);
}

void AgentService::clearHistory(const QString& conversationId)
{
    m_histories.remove(conversationId);
    m_historiesLRU.remove(conversationId);
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

    // 开始计时：Payload构建
    m_payloadBuildTimer.start();
    QJsonObject payload = buildRequestPayload(conversationId, text, frames, ctx);
    if (!tools.isEmpty()) {
        payload.insert(QStringLiteral("tools"), tools);
        payload.insert(QStringLiteral("tool_choice"), toolChoice);

        // 调试日志：输出工具定义
        qDebug() << "[AgentService] 发送工具调用请求"
                 << "会话=" << conversationId
                 << "工具数量=" << tools.size()
                 << "tool_choice=" << QJsonDocument(toolChoice.toObject()).toJson(QJsonDocument::Compact);

        // 输出每个工具的名称
        QStringList toolNames;
        for (const auto& toolValue : tools) {
            const QJsonObject tool = toolValue.toObject();
            const QString toolName = tool.value(QStringLiteral("function"))
                                         .toObject()
                                         .value(QStringLiteral("name"))
                                         .toString();
            if (!toolName.isEmpty()) {
                toolNames.append(toolName);
            }
        }
        qDebug() << "[AgentService] 可用工具列表:" << toolNames.join(", ");
    }
    const qint64 payloadBuildMs = m_payloadBuildTimer.elapsed();
    qDebug() << "[AgentService][Timing] tool_request_payload_built"
             << "convId=" << conversationId
             << "tool_count=" << tools.size()
             << "payload_build_ms=" << payloadBuildMs;

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

    // 开始计时：Payload构建
    m_payloadBuildTimer.start();

    // === P0: 压缩 tool 结果体积 ===
    QJsonArray compressedToolMessages = toolMessages;
    m_budgetManager.compressToolResults(compressedToolMessages);
    // 把 assistant tool_calls 消息 + 压缩后的 tool 结果消息 追加到历史
    QJsonArray& history = getOrCreateHistory(conversationId);
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
        // 检查是否存在 tool 消息没有对应的 assistant tool_calls 消息
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

    const qint64 payloadBuildMs = m_payloadBuildTimer.elapsed();
    qDebug() << "[AgentService][Timing] tool_continue_payload_built"
             << "convId=" << conversationId
             << "tool_count=" << tools.size()
             << "payload_build_ms=" << payloadBuildMs;

    sendStreamWithTools(conversationId, payload);
}

void AgentService::sendStreamWithTools(const QString& convId,
                                       const QJsonObject& payload)
{
    m_currentConvId       = convId;
    m_oneShotActive = false;
    m_accumulated.clear();
    m_pendingToolCalls    = QJsonArray();
    m_pendingFinishReason.clear();
    m_toolStreamError.clear();
    m_firstChunkArrived = false;
    m_ttftMs = 0;
    m_streaming = true;

    m_requestTimer.start();
    qDebug() << "[AgentService][Timing] tool_stream_request_start"
             << "convId=" << convId
             << "tool_count=" << m_pendingToolCalls.size();

    QString base = m_endpoint;
    while (base.endsWith('/')) base.chop(1);
    const QUrl url(base + QStringLiteral("/chat/completions"));
    m_network->streamPostRaw(
        url, payload,
        // onChoice: 完整 delta 对象
        [this, convId](const QJsonObject& choice) {
            if (!m_streaming || m_currentConvId != convId) return;
            const QJsonObject delta = choice.value(QStringLiteral("delta")).toObject();
            const QString content = delta.value(QStringLiteral("content")).toString();

            // 首包到达统计（只要有内容或tool_calls都算首包）
            if (!m_firstChunkArrived && (!content.isEmpty() || !delta.value(QStringLiteral("tool_calls")).toArray().isEmpty())) {
                m_firstChunkArrived = true;
                m_ttftMs = m_requestTimer.elapsed();
                qDebug() << "[AgentService][Timing] tool_first_chunk_received"
                         << "convId=" << m_currentConvId
                         << "TTFT_ms=" << m_ttftMs;
            }

            if (!content.isEmpty()) {
                m_accumulated += content;
                emit responseChunk(m_currentConvId, content);
                if (!m_streaming || m_currentConvId != convId) return;
            }
            // 累积 tool_calls 增量
            const QJsonArray tcArr = delta.value(QStringLiteral("tool_calls")).toArray();
            for (const auto& v : tcArr) {
                if (!m_toolStreamError.isEmpty()) break;
                if (!v.isObject()) {
                    m_toolStreamError = QStringLiteral("工具调用片段不是对象");
                    break;
                }
                const QJsonObject tc = v.toObject();
                const QString id = tc.value(QStringLiteral("id")).toString();
                int index = -1;
                if (tc.contains(QStringLiteral("index"))) {
                    const auto value = tc.value(QStringLiteral("index"));
                    index = value.toInt(-1);
                    if (!value.isDouble() || value.toDouble() != index) index = -1;
                } else if (!id.isEmpty()) {
                    for (int i = 0; i < m_pendingToolCalls.size(); ++i) {
                        if (m_pendingToolCalls[i].toObject()["id"].toString() == id) {
                            index = i;
                            break;
                        }
                    }
                    if (index < 0) index = m_pendingToolCalls.size();
                } else if (tcArr.size() == 1 && m_pendingToolCalls.size() <= 1) {
                    index = 0;
                }
                if (index < 0 || index >= 64) {
                    m_toolStreamError = QStringLiteral("工具调用缺少可确定的索引或索引越界");
                    break;
                }
                // 扩容
                while (m_pendingToolCalls.size() <= index) {
                    m_pendingToolCalls.append(QJsonObject{});
                }
                QJsonObject slot = m_pendingToolCalls.at(index).toObject();
                if (!id.isEmpty()) {
                    const QString existing = slot["id"].toString();
                    if (!existing.isEmpty() && existing != id) {
                        m_toolStreamError = QStringLiteral("工具调用%1的 ID 冲突").arg(index);
                        break;
                    }
                    slot.insert(QStringLiteral("id"), id);
                }
                const QJsonObject fn = tc.value(QStringLiteral("function")).toObject();
                if (!fn.isEmpty()) {
                    QJsonObject slotFn = slot.value(QStringLiteral("function")).toObject();
                    const QString name = fn["name"].toString();
                    if (!name.isEmpty()) {
                        const QString existing = slotFn["name"].toString();
                        if (!existing.isEmpty() && existing != name) {
                            m_toolStreamError = QStringLiteral("工具调用%1的名称冲突").arg(index);
                            break;
                        }
                        slotFn.insert(QStringLiteral("name"), name);
                    }
                    if (fn.contains(QStringLiteral("arguments"))) {
                        const QString cur = slotFn.value(QStringLiteral("arguments")).toString();
                        const auto args = fn.value(QStringLiteral("arguments"));
                        if (args.isString()) {
                            slotFn.insert(QStringLiteral("arguments"), cur + args.toString());
                        } else if (args.isObject() && cur.isEmpty()) {
                            slotFn.insert(QStringLiteral("arguments"), QString::fromUtf8(
                                QJsonDocument(args.toObject()).toJson(QJsonDocument::Compact)));
                        } else if (!args.isNull()) {
                            m_toolStreamError = QStringLiteral("工具调用%1的参数片段类型无效").arg(index);
                            break;
                        }
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
        [this, convId]() {
            if (!m_streaming || m_currentConvId != convId) return;
            m_streaming = false;
            QJsonArray normalized;
            QSet<QString> ids;
            for (int i = 0; i < m_pendingToolCalls.size() && m_toolStreamError.isEmpty(); ++i) {
                const auto call = m_pendingToolCalls[i].toObject();
                if (call.isEmpty()) continue; // Sparse stream indices are not tool calls.
                const auto function = call["function"].toObject();
                const QString id = call["id"].toString();
                QJsonParseError error{};
                const auto args = QJsonDocument::fromJson(function["arguments"].toString().toUtf8(), &error);
                if (id.trimmed().isEmpty() || function["name"].toString().trimmed().isEmpty()
                    || ids.contains(id) || error.error != QJsonParseError::NoError || !args.isObject()) {
                    m_toolStreamError = QStringLiteral("工具调用%1缺少 ID/名称、ID 重复或参数不是完整 JSON 对象").arg(i);
                    break;
                }
                ids.insert(id);
                normalized.append(call);
            }
            if (!m_toolStreamError.isEmpty()) {
                emit responseError(convId, QStringLiteral("invalid_tool_call: ") + m_toolStreamError);
                return;
            }
            m_pendingToolCalls = normalized;
            const qint64 totalLatencyMs = m_requestTimer.elapsed();
            const int outputTokens = m_budgetManager.estimateTextTokens(m_accumulated);
            const qint64 generationTimeMs = totalLatencyMs - m_ttftMs;
            const double tokensPerSecond = generationTimeMs > 0
                                               ? (outputTokens * 1000.0) / generationTimeMs
                                               : 0.0;

            // tool_calls 轮次的 assistant 消息由 continueWithToolResults 回填；
            // stop/length 时在此写入最终 assistant 文本，保证多轮历史闭环。
            if (m_pendingToolCalls.isEmpty()
                && (m_pendingFinishReason == QLatin1String("stop")
                    || m_pendingFinishReason == QLatin1String("length")
                    || (m_pendingFinishReason != QLatin1String("tool_calls")
                        && !m_accumulated.isEmpty()))) {
                QJsonArray& history = getOrCreateHistory(m_currentConvId);
                history.append(QJsonObject{
                                           { QStringLiteral("role"), QStringLiteral("assistant") },
                                           { QStringLiteral("content"), m_accumulated } });
            }

            qDebug() << "[AgentService][Timing] tool_stream_request_finished"
                     << "convId=" << m_currentConvId
                     << "finish_reason=" << m_pendingFinishReason
                     << "tool_call_count=" << m_pendingToolCalls.size()
                     << "total_latency_ms=" << totalLatencyMs
                     << "TTFT_ms=" << m_ttftMs
                     << "generation_time_ms=" << generationTimeMs
                     << "output_text_tokens=" << outputTokens
                     << "tokens_per_second=" << QString::number(tokensPerSecond, 'f', 2);

            qDebug() << "[AgentService] 模型流式响应结束"
                     << "会话=" << m_currentConvId
                     << "结束原因=" << m_pendingFinishReason
                     << "工具调用数=" << m_pendingToolCalls.size()
                     << "回答字符数=" << m_accumulated.size();

            const auto calls = m_pendingToolCalls;
            const auto reason = m_pendingFinishReason;
            const auto text = m_accumulated;
            emit responseFinishedWithTools(convId, calls, reason, text);
        },
        // onError
        [this, convId](const QString& err) {
            if (!m_streaming || m_currentConvId != convId) return;
            m_streaming = false;
            const qint64 errorLatencyMs = m_requestTimer.elapsed();
            qDebug() << "[AgentService][Timing] tool_stream_request_error"
                     << "convId=" << m_currentConvId
                     << "error_latency_ms=" << errorLatencyMs
                     << "error=" << err;
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
        qDebug() << "[AgentService] 历史消息已截断"
                 << "截断前Token数=" << before << "截断后Token数=" << after
                 << "剩余消息数=" << history.size();
    }
}

// ============================================================
// LRU 历史缓存管理（P0修复：防止内存泄漏）
// ============================================================
QJsonArray& AgentService::getOrCreateHistory(const QString& conversationId)
{
    // 检查LRU缓存中是否存在
    auto it = m_historiesLRU.find(conversationId);
    if (it != m_historiesLRU.end()) {
        // 更新访问时间
        it->lastAccessTime = QDateTime::currentMSecsSinceEpoch();
        // 同步到旧的m_histories（向后兼容）
        m_histories[conversationId] = it->messages;
        return it->messages;
    }
    // 不存在，创建新条目前先检查是否需要淘汰
    evictLRUIfNeeded();
    // 创建新的历史条目
    HistoryEntry entry;
    entry.messages = QJsonArray();
    entry.lastAccessTime = QDateTime::currentMSecsSinceEpoch();
    m_historiesLRU[conversationId] = entry;
    m_histories[conversationId] = entry.messages;
    return m_historiesLRU[conversationId].messages;
}

void AgentService::touchHistory(const QString& conversationId)
{
    auto it = m_historiesLRU.find(conversationId);
    if (it != m_historiesLRU.end()) {
        it->lastAccessTime = QDateTime::currentMSecsSinceEpoch();
    }
}

void AgentService::evictLRUIfNeeded()
{
    if (m_historiesLRU.size() < kMaxCachedConversations) {
        return;
    }
    // 找到最久未使用的条目
    QString oldestConvId;
    qint64 oldestTime = std::numeric_limits<qint64>::max();
    for (auto it = m_historiesLRU.begin(); it != m_historiesLRU.end(); ++it) {
        // 跳过当前正在流式处理的会话
        if (it.key() == m_currentConvId && m_streaming) {
            continue;
        }

        if (it->lastAccessTime < oldestTime) {
            oldestTime = it->lastAccessTime;
            oldestConvId = it.key();
        }
    }
    if (!oldestConvId.isEmpty()) {
        qDebug() << "[AgentService] LRU淘汰会话历史"
                 << "会话ID=" << oldestConvId
                 << "缓存数=" << m_historiesLRU.size()
                 << "→" << (m_historiesLRU.size() - 1);
        m_historiesLRU.remove(oldestConvId);
        m_histories.remove(oldestConvId);
    }
}
