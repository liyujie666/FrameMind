#include "infrastructure/databasemanager.h"
#include "infrastructure/networkclient.h"
#include "service/agent/one_shot_vlm_channel.h"
#include "service/agent/unit_analysis_worker_pool.h"
#include "service/agentservice.h"
#include "service/llmproviderservice.h"
#include "service/settingsservice.h"
#include <QJsonDocument>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QPointer>
#include <QUuid>
#include <QtTest>

// Exercise the real payload, HTTP/SSE parser and serial channel, without an online model.
class LocalModel : public QTcpServer {
  public:
    struct Response {
        QByteArray body;
        int status = 200;
        bool stall = false;
        int delayMs = 0;
        QByteArray extraHeaders;
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
                    const auto bytesToSend = "HTTP/1.1 " + QByteArray::number(response.status) +
                                  " Result\r\nContent-Type: " +
                                  (response.status == 200 ? "text/event-stream" : "application/json") +
                                  "\r\nContent-Length: " +
                                  QByteArray::number(response.body.size() + (response.stall ? 10000 : 0)) +
                                  "\r\nConnection: close\r\n" + response.extraHeaders + "\r\n" + response.body;
                    QPointer<QTcpSocket> guard(socket);
                    QTimer::singleShot(response.delayMs, this, [guard, bytesToSend, response] {
                        if (!guard) return;
                        guard->write(bytesToSend);
                        if (!response.stall) guard->disconnectFromHost();
                    });
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
    void workerPoolParallelRequestsAndCancellation() {
        LocalModel server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        providers->setEndpoint(providerId, QString("http://127.0.0.1:%1/v1").arg(server.serverPort()));
        server.responses << LocalModel::Response{LocalModel::answer("old", "", false), 200, true}
                         << LocalModel::Response{LocalModel::answer("second"), 200, false, 250};
        UnitAnalysisWorkerPool pool(settings, providers, 3, nullptr, 2000);
        VideoBuildContext context;
        context.buildId = "build"; context.videoId = "video"; context.taskGeneration = 1;
        UnitAnalysisRequest a{context, "unit-a", "a:0", "request-a", 0, 0, 0};
        UnitAnalysisRequest b{context, "unit-b", "b:0", "request-b", 0, 0, 1};
        QList<ModelReply> repliesA, repliesB;
        pool.submit(a, "system A", "page A", {}, [&](ModelReply r) { repliesA << r; });
        QTRY_COMPARE(server.requests.size(), 1); // fix response assignment while A remains in flight
        pool.submit(b, "system B", "page B", {}, [&](ModelReply r) { repliesB << r; });
        QTRY_COMPARE(server.requests.size(), 2);
        QVERIFY(repliesA.isEmpty());
        QVERIFY(repliesB.isEmpty()); // server observed actual overlap, neither request finished
        pool.cancelRequest(a.requestId);
        pool.cancelRequest(a.requestId);
        QTRY_COMPARE(repliesA.size(), 1);
        QVERIFY(repliesA[0].error.startsWith("cancelled:"));
        QTRY_COMPARE(repliesB.size(), 1);
        QCOMPARE(repliesB[0].content, QString("second"));
        QCOMPARE(repliesB[0].httpStatus, 200);
        QVERIFY(repliesB[0].diagnostics["elapsed_ms"].toDouble() >= 0);
        pool.releaseUnit(0, a.unitId);
        pool.releaseUnit(1, b.unitId);
        server.responses << LocalModel::Response{LocalModel::answer("reuse"), 200};
        a.unitId = "next-unit"; a.requestId = "next-request";
        pool.submit(a, "system", "new unit", {}, [&](ModelReply r) { repliesA << r; });
        QTRY_COMPARE(repliesA.size(), 2);
        QCOMPARE(repliesA.last().content, QString("reuse"));
        pool.releaseUnit(0, a.unitId);
        server.responses << LocalModel::Response{LocalModel::answer("cancel-a", ""), 200, true}
                         << LocalModel::Response{LocalModel::answer("cancel-b", ""), 200, true};
        a.requestId = "all-a"; b.requestId = "all-b";
        pool.submit(a, "system", "cancel all", {}, [&](ModelReply r) { repliesA << r; });
        pool.submit(b, "system", "cancel all", {}, [&](ModelReply r) { repliesB << r; });
        QTRY_COMPARE(server.requests.size(), 5);
        pool.cancelBuild(context.cancellationKey());
        pool.cancelBuild(context.cancellationKey());
        QTRY_COMPARE(repliesA.size(), 3);
        QTRY_COMPARE(repliesB.size(), 2);
        QVERIFY(repliesA.last().error.startsWith("cancelled:"));
        QVERIFY(repliesB.last().error.startsWith("cancelled:"));
        a.context.buildId = "new-build"; a.context.taskGeneration = 2;
        a.unitId = "new-build-unit"; a.requestId = "new-build-request";
        server.responses << LocalModel::Response{LocalModel::answer("new-build")};
        pool.submit(a, "system", "new build", {}, [&](ModelReply r) { repliesA << r; });
        QTRY_COMPARE(repliesA.size(), 4);
        QCOMPARE(repliesA.last().content, QString("new-build"));
    }

    void explicitQueuedCancellationAndRateLimitMetadata() {
        LocalModel server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        providers->setEndpoint(providerId, QString("http://127.0.0.1:%1/v1").arg(server.serverPort()));
        NetworkClient network;
        AgentService agent(&network, settings, providers);
        OneShotVlmChannel channel(&agent, nullptr, 2000);
        server.responses << LocalModel::Response{"{\"error\":{\"message\":\"rate limited\"}}", 429, false,
                                                 100, "Retry-After: 2\r\n"};
        QList<ModelReply> active, queued;
        channel.enqueueRequest("active", "sys", "text", {}, OneShotVlmChannel::Priority::Background,
                               "build", [&](ModelReply r) { active << r; });
        channel.enqueueRequest("queued", "sys", "text", {}, OneShotVlmChannel::Priority::Background,
                               "build", [&](ModelReply r) { queued << r; });
        channel.cancelRequest("queued");
        channel.cancelRequest("queued");
        QTRY_COMPARE(queued.size(), 1);
        QVERIFY(queued[0].error.startsWith("cancelled:"));
        QTRY_COMPARE(active.size(), 1);
        QCOMPARE(active[0].httpStatus, 429);
        QCOMPARE(active[0].retryAfterMs, qint64(2000));
        QCOMPARE(server.requests.size(), 1);
        QVERIFY(!channel.isBusy());
    }

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
