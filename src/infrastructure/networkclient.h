#ifndef FRAMEMIND_NETWORKCLIENT_H
#define FRAMEMIND_NETWORKCLIENT_H

#include <QObject>
#include <QUrl>
#include <QJsonObject>
#include <QByteArray>
#include <QMap>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

/**
 * HTTP + SSE 流式通信封装（api-protocol.md §2.4 / §7.1 / §7.2）。
 *
 * 必须在创建线程（主线程）使用 QNetworkAccessManager。
 * 流式：监听 readyRead，内部 buffer 处理跨 chunk 的 "\n\n" 分割，
 * 忽略以 ':' 开头的注释行，解析 "data: " 后的 JSON 或 [DONE]。
 */
class NetworkClient : public QObject {
    Q_OBJECT
public:
    explicit NetworkClient(QObject* parent = nullptr);
    ~NetworkClient() override;

    /// 设置 Bearer Token（内部 trim，防 CRLF 注入）；空则不加鉴权头
    void setAuthToken(const QString& token);

    /// 普通 POST（非流式）
    QNetworkReply* post(const QUrl& url, const QJsonObject& body,
                        const QMap<QString, QString>& headers = {});

    /// SSE 流式 POST（仅解析 delta.content，兼容旧调用方）
    void streamPost(const QUrl& url, const QJsonObject& body,
                    std::function<void(const QString& chunk)> onChunk,
                    std::function<void()> onDone,
                    std::function<void(const QString& error)> onError);

    /**
     * SSE 流式 POST，回传完整 delta JSON 对象。
     * 供 Tool Calling 场景使用（M4 起），可访问 delta.tool_calls / finish_reason。
     */
    void streamPostRaw(const QUrl& url, const QJsonObject& body,
                        std::function<void(const QJsonObject& choice)> onChoice,
                        std::function<void()> onDone,
                        std::function<void(const QString& error)> onError);

    /// 简单 GET 请求用于连通性检测（同步）
    bool testConnection(const QUrl& url, QString* errorString = nullptr);

    /// 立即终止当前流式请求
    void cancelStream();

private:
    void applyCommonHeaders(class QNetworkRequest& req,
                            const QMap<QString, QString>& headers) const;
    void parseSSEChunk(const QByteArray& chunk);
    void finishStream();

    QNetworkAccessManager* m_nam = nullptr;
    QNetworkReply*         m_activeStream = nullptr;
    QByteArray             m_buffer;
    QString                m_authToken;
    bool                   m_done = false;

    std::function<void(const QString&)>       m_onChunk;   // 兼容路径
    std::function<void(const QJsonObject&)>   m_onChoice;  // 完整 delta 路径
    std::function<void()>                     m_onDone;
    std::function<void(const QString&)>       m_onError;
};

#endif // FRAMEMIND_NETWORKCLIENT_H
