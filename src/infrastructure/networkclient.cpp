#include "infrastructure/networkclient.h"
#include <QDateTime>

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonArray>
#include <QEventLoop>

NetworkClient::NetworkClient(QObject* parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
}

NetworkClient::~NetworkClient()
{
    cancelStream();
}

void NetworkClient::setAuthToken(const QString& token)
{
    m_authToken = token.trimmed();
}

void NetworkClient::applyCommonHeaders(QNetworkRequest& req,
                                       const QMap<QString, QString>& headers) const
{
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/json"));
    if (!m_authToken.isEmpty()) {
        // trim 已在 setAuthToken 完成，防止 CRLF 注入
        req.setRawHeader("Authorization",
                         QStringLiteral("Bearer %1").arg(m_authToken).toUtf8());
    }
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it) {
        req.setRawHeader(it.key().toUtf8(), it.value().toUtf8());
    }
    req.setTransferTimeout(60000);  // 60s
}

QNetworkReply* NetworkClient::post(const QUrl& url, const QJsonObject& body,
                                   const QMap<QString, QString>& headers)
{
    QNetworkRequest req(url);
    applyCommonHeaders(req, headers);
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    return m_nam->post(req, payload);
}

void NetworkClient::streamPost(const QUrl& url, const QJsonObject& body,
                               std::function<void(const QString&)> onChunk,
                               std::function<void()> onDone,
                               std::function<void(const QString&)> onError)
{
    // 先取消上一个流，保证单流
    cancelStream();

    m_onChunk  = std::move(onChunk);
    m_onChoice = nullptr;
    m_onDone   = std::move(onDone);
    m_onError  = std::move(onError);
    m_buffer.clear();
    m_done = false;

    QNetworkRequest req(url);
    applyCommonHeaders(req, {});
    req.setRawHeader("Accept", "text/event-stream");

    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    m_activeStream = m_nam->post(req, payload);

    connect(m_activeStream, &QNetworkReply::readyRead, this, [this]() {
        if (m_activeStream) {
            parseSSEChunk(m_activeStream->readAll());
        }
    });

    auto* reply = m_activeStream;
    connect(reply, &QNetworkReply::finished, this, [this, reply] { handleStreamFinished(reply); });
}

void NetworkClient::streamPostRaw(const QUrl& url, const QJsonObject& body,
                                    std::function<void(const QJsonObject&)> onChoice,
                                    std::function<void()> onDone,
                                    std::function<void(const QString&)> onError,
                                    int idleTimeoutMs, bool allowHttp2)
{
    cancelStream();

    m_onChunk  = nullptr;
    m_onChoice = std::move(onChoice);
    m_onDone   = std::move(onDone);
    m_onError  = std::move(onError);
    m_buffer.clear();
    m_done = false;

    QNetworkRequest req(url);
    applyCommonHeaders(req, {});
    req.setTransferTimeout(idleTimeoutMs);
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, allowHttp2);
    req.setRawHeader("Accept", "text/event-stream");

    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    m_activeStream = m_nam->post(req, payload);

    auto* reply = m_activeStream;
    connect(reply, &QNetworkReply::metaDataChanged, this, [this, reply] {
        if (m_activeStream != reply) return;
        const auto header = reply->rawHeader("Retry-After").trimmed();
        bool ok = false;
        const qint64 seconds = header.toLongLong(&ok);
        qint64 retryMs = -1;
        if (ok && seconds >= 0)
            retryMs = qMin(seconds, qint64(86400)) * 1000;
        else if (!header.isEmpty()) {
            const auto date = QDateTime::fromString(QString::fromLatin1(header), Qt::RFC2822Date);
            if (date.isValid()) retryMs = qMax(qint64(0), QDateTime::currentDateTimeUtc().msecsTo(date));
        }
        emit streamMetadata(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), retryMs, {});
        emit streamActivity(0, reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt());
    });
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
        if (m_activeStream != reply || m_done) return;
        const auto chunk = reply->readAll();
        if (!chunk.isEmpty())
            emit streamActivity(chunk.size(), reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt());
        if (m_activeStream == reply && !m_done) parseSSEChunk(chunk);
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] { handleStreamFinished(reply); });
}

void NetworkClient::handleStreamFinished(QNetworkReply* reply)
{
    if (m_activeStream != reply) return;
    const auto error = reply->error();
    QString detail;
    // Explicit cancellation disconnects this reply in cancelStream(). A still-active
    // OperationCanceledError (e.g. Qt transfer timeout) must not become a success.
    if (error != QNetworkReply::NoError) {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto body = m_buffer + reply->readAll();
        const auto object = QJsonDocument::fromJson(body).object();
        QString message = object["error"].toObject()["message"].toString();
        if (message.isEmpty()) message = object["message"].toString();
        if (message.isEmpty()) message = reply->errorString();
        detail = status > 0 ? QStringLiteral("HTTP %1: %2").arg(status).arg(message) : message;
    }
    // A completion callback may immediately post the next request. Release the old
    // reply before invoking it; never delete or clear the new active reply afterwards.
    m_activeStream = nullptr;
    reply->deleteLater();
    if (!detail.isEmpty()) {
        m_done = true;
        const auto onError = m_onError;
        if (onError) onError(detail);
    } else if (!m_done) {
        finishStream();
    }
}

void NetworkClient::parseSSEChunk(const QByteArray& chunk)
{
    auto* reply = m_activeStream;
    m_buffer.append(chunk);
    m_buffer.replace("\r\n", "\n");

    int eventEnd;
    while ((eventEnd = m_buffer.indexOf("\n\n")) != -1) {
        const QByteArray event = m_buffer.left(eventEnd);
        m_buffer.remove(0, eventEnd + 2);

        const QList<QByteArray> lines = event.split('\n');
        for (QByteArray line : lines) {
            if (line.endsWith('\r')) line.chop(1);
            // 注释/心跳行（以 ':' 开头）忽略；非 data: 行忽略
            if (line.startsWith(':')) continue;
            if (!line.startsWith("data:")) continue;

            QByteArray payload = line.mid(5);  // 去掉 "data:"
            if (payload.startsWith(' ')) payload = payload.mid(1);

            if (payload == "[DONE]") {
                finishStream();
                return;
            }

            const QJsonObject obj = QJsonDocument::fromJson(payload).object();
            if (obj["usage"].isObject())
                emit streamMetadata(reply ? reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() : 0,
                                    -1, obj["usage"].toObject());
            const QJsonArray choices = obj.value(QStringLiteral("choices")).toArray();
            if (choices.isEmpty()) continue;
            const QJsonObject choice = choices.at(0).toObject();

            // Raw 路径：把整个 choice 对象透传给调用方（Tool Calling 场景）
            if (m_onChoice) {
                const auto onChoice = m_onChoice;
                onChoice(choice);
                if (m_activeStream != reply || m_done) return;
                continue;
            }

            // 兼容路径：仅回调 delta.content
            const QJsonObject delta = choice.value(QStringLiteral("delta")).toObject();
            const QString content = delta.value(QStringLiteral("content")).toString();
            if (!content.isEmpty() && m_onChunk) {
                const auto onChunk = m_onChunk;
                onChunk(content);
                if (m_activeStream != reply || m_done) return;
            }
        }
    }
}

void NetworkClient::finishStream()
{
    if (m_done) return;
    m_done = true;
    const auto onDone = m_onDone;
    if (onDone) onDone();
}

void NetworkClient::cancelStream()
{
    if (m_activeStream) {
        // 立即终止：断开信号 + abort
        disconnect(m_activeStream, nullptr, this, nullptr);
        m_activeStream->abort();
        m_activeStream->deleteLater();
        m_activeStream = nullptr;
    }
    m_buffer.clear();
    m_done = true;
}

bool NetworkClient::testConnection(const QUrl& url, QString* errorString)
{
    // 用最小 chat/completions 请求探测连通性，所有 OpenAI 兼容平台均支持此接口。
    // 不使用 GET /models，因为部分平台（如阿里百炼）不支持该端点。
    QString base = url.toString();
    while (base.endsWith('/')) base.chop(1);
    const QUrl chatUrl(base + QStringLiteral("/chat/completions"));

    QNetworkRequest req(chatUrl);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    if (!m_authToken.isEmpty()) {
        req.setRawHeader("Authorization",
                         QStringLiteral("Bearer %1").arg(m_authToken).toUtf8());
    }
    req.setTransferTimeout(15000);

    // 最小合法请求体，max_tokens=1 让服务端尽快返回
    const QByteArray payload =
        R"({"model":"__probe__","messages":[{"role":"user","content":"hi"}],"max_tokens":1,"stream":false})";

    QNetworkReply* reply = m_nam->post(req, payload);
    if (!reply) {
        if (errorString) *errorString = QStringLiteral("无法创建网络请求");
        return false;
    }

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    const auto netErr    = reply->error();
    const int  statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body = reply->readAll();
    const QString replyErrStr = reply->errorString();
    reply->deleteLater();

    // 有 HTTP 状态码说明服务器收到了请求，先按状态码判断
    if (statusCode > 0) {
        if (statusCode == 401) {
            if (errorString) {
                QString detail = QStringLiteral("HTTP 401: API Key 无效，请检查密钥是否正确");
                const QJsonDocument doc = QJsonDocument::fromJson(body);
                if (!doc.isNull()) {
                    const QString msg = doc.object()
                        .value(QStringLiteral("error")).toObject()
                        .value(QStringLiteral("message")).toString();
                    if (!msg.isEmpty()) detail = QStringLiteral("HTTP 401: ") + msg;
                }
                *errorString = detail;
            }
            return false;
        }
        if (statusCode == 403) {
            if (errorString)
                *errorString = QStringLiteral("HTTP 403: 访问被拒绝，请检查账户余额或 IP 白名单");
            return false;
        }
        // 其余任何状态码（200、400、404、5xx）都说明连通性正常
        return true;
    }

    // statusCode == 0：纯网络层错误（DNS 失败、连接被拒、超时等）
    if (netErr != QNetworkReply::NoError) {
        if (errorString) *errorString = replyErrStr;
        return false;
    }

    return true;
}
