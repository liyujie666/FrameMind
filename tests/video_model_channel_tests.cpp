#include "infrastructure/databasemanager.h"
#include "infrastructure/networkclient.h"
#include "service/agent/one_shot_vlm_channel.h"
#include "service/agentservice.h"
#include "service/llmproviderservice.h"
#include "service/settingsservice.h"
#include <QJsonDocument>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

// Exercise the real payload, HTTP/SSE parser and serial channel, without an online model.
class LocalModel : public QTcpServer {
  public:
    struct Response {
        QByteArray body;
        int status = 200;
        bool stall = false;
    };
    QList<Response> responses;
    QList<QJsonObject> requests;
    LocalModel() {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto *socket = nextPendingConnection();
                auto bytes = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, bytes] {
                    *bytes += socket->readAll();
                    const int end = bytes->indexOf("\r\n\r\n");
                    if (end < 0)
                        return;
                    int length = 0;
                    for (const auto &line : bytes->left(end).split('\n'))
                        if (line.toLower().startsWith("content-length:"))
                            length = line.mid(15).trimmed().toInt();
                    if (bytes->size() < end + 4 + length)
                        return;
                    disconnect(socket, &QTcpSocket::readyRead, this, nullptr);
                    requests << QJsonDocument::fromJson(bytes->mid(end + 4, length)).object();
                    const auto response = responses.isEmpty() ? Response{} : responses.takeFirst();
                    socket->write("HTTP/1.1 " + QByteArray::number(response.status) +
                                  " Result\r\nContent-Type: " +
                                  (response.status == 200 ? "text/event-stream" : "application/json") +
                                  "\r\nContent-Length: " +
                                  QByteArray::number(response.body.size() + (response.stall ? 10000 : 0)) +
                                  "\r\nConnection: close\r\n\r\n" + response.body);
                    if (!response.stall)
                        socket->disconnectFromHost();
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }
    static QByteArray answer(const QString &content, const QString &finish = "stop", bool done = false) {
        QJsonObject choice{{"delta", QJsonObject{{"content", content}}}};
        if (!finish.isEmpty())
            choice["finish_reason"] = finish;
        QByteArray body =
            "data: " +
            QJsonDocument(QJsonObject{{"choices", QJsonArray{choice}}}).toJson(QJsonDocument::Compact) +
            "\n\n";
        if (done)
            body += "data: [DONE]\n\n";
        return body;
    }
};

class VideoModelChannelTests : public QObject {
    Q_OBJECT
    QTemporaryDir directory;
    SettingsService *settings = nullptr;
    LLMProviderService *providers = nullptr;
    QString providerId;
  private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setApplicationName("FrameMindModelTests-" +
                                             QUuid::createUuid().toString(QUuid::WithoutBraces));
        auto *db = DatabaseManager::instance();
        QVERIFY(db->initialize(directory.filePath("model.sqlite")));
        settings = new SettingsService(db, this);
        providers = new LLMProviderService(settings, this);
        auto provider = LLMProviderPresets::custom();
        providerId = provider.id = "local-model-fixture";
        provider.name = "Local fixture";
        provider.endpoint = "http://127.0.0.1:1/v1";
        provider.apiKeyName = "local-model-fixture-key";
        provider.defaultModel = "fixture-model";
        QVERIFY(providers->addCustomProvider(provider));
        QVERIFY(providers->setApiKey(providerId, "fixture-secret"));
        providers->setActiveProvider(providerId);
    }
    void cleanupTestCase() {
        QVERIFY(providers->deleteApiKey(providerId));
        DatabaseManager::instance()->close();
    }
    void isolatedPromptAndSerialRequests() {
        LocalModel server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        providers->setEndpoint(providerId, QString("http://127.0.0.1:%1/v1").arg(server.serverPort()));
        // The first response lacks [DONE], exercising completion from QNetworkReply::finished.
        auto second = LocalModel::answer("{\"summary\":\"second\"}", "stop", true);
        second.replace("\n", "\r\n"); // SSE newline conventions may differ between compatible providers.
        server.responses = {{LocalModel::answer("{\"summary\":\"first\"}")}, {second}};
        NetworkClient network;
        AgentService agent(&network, settings, providers);
        const auto signature = agent.modelSignature();
        providers->setModel(providerId, "updated-fixture-model");
        QVERIFY(agent.modelSignature() != signature);
        providers->setModel(providerId, "fixture-model");
        OneShotVlmChannel channel(&agent);
        ChatMessage history;
        history.role = ChatMessage::User;
        history.content = "old chat";
        agent.seedHistory("unused", {history});
        QList<ModelReply> replies;
        QImage image(16, 16, QImage::Format_RGB32);
        image.fill(Qt::blue);
        channel.enqueueDetailed("JSON contract", "raw evidence", {image},
                                OneShotVlmChannel::Priority::Background, "b",
                                [&](ModelReply reply) { replies << reply; });
        channel.enqueueDetailed("Second contract", "second evidence", {},
                                OneShotVlmChannel::Priority::Background, "b",
                                [&](ModelReply reply) { replies << reply; });
        QTRY_COMPARE(replies.size(), 2);
        QVERIFY2(replies[0].error.isEmpty(), qPrintable(replies[0].error));
        QVERIFY2(replies[1].error.isEmpty(), qPrintable(replies[1].error));
        QVERIFY(replies[1].content.contains("second"));
        QCOMPARE(server.requests.size(), 2);
        const auto payload = server.requests.first();
        const auto messages = payload["messages"].toArray();
        QCOMPARE(messages.size(), 2);
        QCOMPARE(messages[0].toObject()["role"].toString(), QString("system"));
        QCOMPARE(messages[0].toObject()["content"].toString(), QString("JSON contract"));
        const auto content = messages[1].toObject()["content"].toArray();
        QCOMPARE(content[0].toObject()["text"].toString(), QString("raw evidence"));
        QVERIFY(!content[1].toObject().contains("width"));
        QVERIFY(content[1].toObject()["image_url"].toObject()["url"].toString().startsWith(
            "data:image/jpeg;base64,"));
        QVERIFY(!payload.contains("tools"));
        QCOMPARE(payload["max_tokens"].toInt(), 4096);
        QVERIFY(!channel.isBusy());
    }
    void httpErrorAndRecovery() {
        LocalModel server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        providers->setEndpoint(providerId, QString("http://127.0.0.1:%1/v1").arg(server.serverPort()));
        server.responses = {{"{\"error\":{\"message\":\"bad fixture-secret\"}}", 401},
                            {LocalModel::answer("{\"summary\":\"recovered\"}")}};
        NetworkClient network;
        AgentService agent(&network, settings, providers);
        OneShotVlmChannel channel(&agent);
        QList<ModelReply> replies;
        for (int i = 0; i < 2; ++i)
            channel.enqueueDetailed("JSON", "evidence", {}, OneShotVlmChannel::Priority::Background, "b",
                                    [&](ModelReply reply) { replies << reply; });
        QTRY_COMPARE(replies.size(), 2);
        QVERIFY(replies[0].error.contains("HTTP 401"));
        QVERIFY(replies[0].error.contains("[redacted]"));
        QVERIFY(!replies[0].error.contains("fixture-secret"));
        QVERIFY2(replies[1].error.isEmpty(), qPrintable(replies[1].error));
    }
    void incompleteResponse_data() {
        QTest::addColumn<QString>("finish");
        QTest::addColumn<QString>("expected");
        QTest::newRow("length") << QString("length") << QString("output_truncated");
        QTest::newRow("missing_finish") << QString("") << QString("incomplete_response");
        QTest::newRow("filtered") << QString("content_filter") << QString("incomplete_response");
    }
    void incompleteResponse() {
        QFETCH(QString, finish);
        QFETCH(QString, expected);
        LocalModel server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        providers->setEndpoint(providerId, QString("http://127.0.0.1:%1/v1").arg(server.serverPort()));
        server.responses = {{LocalModel::answer("{\"summary\":\"partial\"}", finish)}};
        NetworkClient network;
        AgentService agent(&network, settings, providers);
        OneShotVlmChannel channel(&agent);
        QList<ModelReply> replies;
        channel.enqueueDetailed("JSON", "evidence", {}, OneShotVlmChannel::Priority::Background, "b",
                                [&](ModelReply reply) { replies << reply; });
        QTRY_COMPARE(replies.size(), 1);
        QVERIFY(replies[0].content.isEmpty());
        QVERIFY2(replies[0].error.contains(expected), qPrintable(replies[0].error));
    }
    void timeoutDiscardsPartialAndContinues() {
        LocalModel server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        providers->setEndpoint(providerId, QString("http://127.0.0.1:%1/v1").arg(server.serverPort()));
        server.responses = {{LocalModel::answer("{\"summary\":\"looks valid\"}", ""), 200, true},
                            {LocalModel::answer("{\"summary\":\"next\"}")}};
        NetworkClient network;
        AgentService agent(&network, settings, providers);
        OneShotVlmChannel channel(&agent, nullptr, 500);
        QList<ModelReply> replies;
        for (int i = 0; i < 2; ++i)
            channel.enqueueDetailed("JSON", "evidence", {}, OneShotVlmChannel::Priority::Background, "b",
                                    [&](ModelReply reply) { replies << reply; });
        QTRY_COMPARE(replies.size(), 2);
        QVERIFY(replies[0].content.isEmpty());
        QVERIFY(replies[0].error.contains("timeout:"));
        QVERIFY2(replies[1].error.isEmpty(), qPrintable(replies[1].error));
    }
    void cancellationDropsOldBuildAndContinues() {
        LocalModel server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        providers->setEndpoint(providerId, QString("http://127.0.0.1:%1/v1").arg(server.serverPort()));
        server.responses = {{LocalModel::answer("partial", ""), 200, true},
                            {LocalModel::answer("{\"summary\":\"new build\"}")}};
        NetworkClient network;
        AgentService agent(&network, settings, providers);
        OneShotVlmChannel channel(&agent);
        int oldCallbacks = 0;
        QList<ModelReply> replies;
        for (int i = 0; i < 2; ++i)
            channel.enqueueDetailed("JSON", "old evidence", {}, OneShotVlmChannel::Priority::Background,
                                    "old", [&](ModelReply) { ++oldCallbacks; });
        channel.enqueueDetailed("JSON", "new evidence", {}, OneShotVlmChannel::Priority::Background, "new",
                                [&](ModelReply reply) { replies << reply; });
        QTRY_COMPARE(server.requests.size(), 1);
        channel.cancelBackground("old");
        QTRY_COMPARE(replies.size(), 1);
        QCOMPARE(oldCallbacks, 0);
        QCOMPARE(server.requests.size(), 2);
        QVERIFY2(replies[0].error.isEmpty(), qPrintable(replies[0].error));
        QVERIFY(replies[0].content.contains("new build"));
        QVERIFY(!channel.isBusy());
    }
};
QTEST_GUILESS_MAIN(VideoModelChannelTests)
#include "video_model_channel_tests.moc"
