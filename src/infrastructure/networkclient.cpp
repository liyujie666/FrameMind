#include "infrastructure/networkclient.h"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonArray>

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

    m_onChunk = std::move(onChunk);
    m_onDone  = std::move(onDone);
    m_onError = std::move(onError);
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

    connect(m_activeStream, &QNetworkReply::finished, this, [this]() {
        if (!m_activeStream) return;
        const auto err = m_activeStream->error();
        if (err != QNetworkReply::NoError && err != QNetworkReply::OperationCanceledError) {
            const QString msg = m_activeStream->errorString();
            // HTTP 错误体可能含 OpenAI 风格 error 信息
            const QByteArray rest = m_activeStream->readAll();
            QString detail = msg;
            if (!rest.isEmpty()) {
                QJsonObject obj = QJsonDocument::fromJson(rest).object();
                const QString apiMsg = obj.value(QStringLiteral("error"))
                                           .toObject()
                                           .value(QStringLiteral("message"))
                                           .toString();
                if (!apiMsg.isEmpty()) detail = apiMsg;
            }
            if (m_onError) m_onError(detail);
            m_activeStream->deleteLater();
            m_activeStream = nullptr;
            return;
        }
        // 正常结束：若服务端未发 [DONE]，也走完成回调
        if (!m_done) {
            finishStream();
        }
        m_activeStream->deleteLater();
        m_activeStream = nullptr;
    });
}

void NetworkClient::parseSSEChunk(const QByteArray& chunk)
{
    m_buffer.append(chunk);

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
            const QJsonArray choices = obj.value(QStringLiteral("choices")).toArray();
            if (choices.isEmpty()) continue;
            const QJsonObject delta =
                choices.at(0).toObject().value(QStringLiteral("delta")).toObject();
            const QString content = delta.value(QStringLiteral("content")).toString();
            if (!content.isEmpty() && m_onChunk) {
                m_onChunk(content);
            }
            // tool_calls 在 M4 处理，这里忽略
        }
    }
}

void NetworkClient::finishStream()
{
    if (m_done) return;
    m_done = true;
    if (m_onDone) m_onDone();
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
