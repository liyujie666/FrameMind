#include "video_rag_fixture.h"
#include "infrastructure/databasemanager.h"
#include "service/agent/video_analysis_service.h"
#include "service/rag/video_rag_store.h"
#include "viewmodel/videoanalysisviewmodel.h"
#include "view/player/summarytabwidget.h"
#include "util/video_file_identity.h"
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>
#include <utility>

class VideoAnalysisLifecycleTests : public QObject {
    Q_OBJECT
    QTemporaryDir directory;
    QString path;
    std::unique_ptr<VideoRAGStore> store;
    FixtureBackend backend;
    std::unique_ptr<VideoRAGBuildCoordinator> coordinator;
    std::unique_ptr<VideoAnalysisService> service;
    std::unique_ptr<VideoAnalysisViewModel> vm;
    QVector<std::function<void()>> pending;
    BuildOptions options;

    QString createVideo(const QString &name) {
        const auto value = directory.filePath(name);
        QFile file(value);
        if (!file.open(QIODevice::WriteOnly)) return {};
        file.write(name.toUtf8());
        return value;
    }
    void holdModel() {
        coordinator->setModelRequest([this](const VideoBuildContext &, const QString &system,
                const QString &text, const QList<QImage> &, std::function<void(QString)> done) {
            pending << [=] { done(FixtureBackend::modelReply(system, text)); };
        });
    }
    void completePending() {
        FixtureBackend::installModel(*coordinator);
        auto replies = std::exchange(pending, {});
        for (const auto &reply : replies) { reply(); reply(); } // Duplicate delivery is deliberate.
    }

  private slots:
    void init() {
        QVERIFY(directory.isValid());
        auto *db = DatabaseManager::instance();
        QVERIFY(db->initialize(directory.filePath(QUuid::createUuid().toString() + ".sqlite")));
        store = std::make_unique<VideoRAGStore>(db);
        QVERIFY(store->initialize());
        backend = FixtureBackend{};
        QImage frame(64, 64, QImage::Format_RGB32);
        frame.fill(Qt::blue);
        backend.framePath = directory.filePath("frame.png");
        QVERIFY(frame.save(backend.framePath));
        path = createVideo("fixture.mp4");
        QVERIFY(!path.isEmpty());
        coordinator = std::make_unique<VideoRAGBuildCoordinator>(&backend, store.get());
        FixtureBackend::installModel(*coordinator);
        service = std::make_unique<VideoAnalysisService>(nullptr, nullptr, store.get(), nullptr, db);
        service->setBuildCoordinator(coordinator.get());
        vm = std::make_unique<VideoAnalysisViewModel>(service.get(), nullptr);
        vm->onVideoOpened(path);
        options = {};
        options.typeOverride = VideoContentType::Meeting;
    }
    void cleanup() {
        pending.clear();
        vm.reset();
        coordinator.reset();
        service.reset();
        store.reset();
        DatabaseManager::instance()->close();
    }
    void oldSummaryReplayKeepsProgressAndLateBinding() {
        coordinator->start(path, options);
        QTRY_VERIFY(!coordinator->isRunning());
        const auto oldId = vm->displayedBuildId();
        QVERIFY(!oldId.isEmpty());
        const auto oldSummary = vm->videoSummary();
        QSignalSpy started(coordinator.get(), &VideoRAGBuildCoordinator::buildStarted);
        QSignalSpy terminated(coordinator.get(), &VideoRAGBuildCoordinator::buildTerminated);
        QStringList events;
        connect(coordinator.get(), &VideoRAGBuildCoordinator::buildStarted, this,
                [&] { events << "start"; });
        connect(coordinator.get(), &VideoRAGBuildCoordinator::representationReady, this,
                [&] { events << "display"; });
        holdModel();
        options.forceDerivedRebuild = true;
        coordinator->start(path, options);
        QCOMPARE(events.mid(0, 2), QStringList({"start", "display"}));
        QVERIFY(vm->isIndexing());
        QCOMPARE(vm->displayedBuildId(), oldId);
        QCOMPARE(vm->videoSummary(), oldSummary);
        QVERIFY(vm->runningBuildId() != oldId);
        SummaryTabWidget widget;
        widget.setViewModel(vm.get()); // Bind with both a cached summary and an active build.
        auto *progress = widget.findChild<QWidget *>("summaryBuildProgress");
        QVERIFY(progress);
        QVERIFY(!progress->isHidden());
        emit service->summaryReady(oldSummary); // Legacy notifications cannot terminate a run.
        QVERIFY(vm->isIndexing());
        QVERIFY(!progress->isHidden());
        completePending();
        QTRY_VERIFY(!coordinator->isRunning());
        QCOMPARE(terminated.size(), 1);
        QVERIFY(!vm->isIndexing());
        QVERIFY(progress->isHidden());
        const auto context = qvariant_cast<VideoBuildContext>(started.first().first());
        QCOMPARE(qvariant_cast<VideoBuildContext>(terminated.first().first()).buildId, context.buildId);
        QCOMPARE(vm->displayedBuildId(), context.buildId);
    }
    void samePathReplacementRejectsOldEvents() {
        QSignalSpy started(coordinator.get(), &VideoRAGBuildCoordinator::buildStarted);
        QSignalSpy terminated(coordinator.get(), &VideoRAGBuildCoordinator::buildTerminated);
        holdModel();
        coordinator->start(path, options);
        QTRY_VERIFY(!pending.isEmpty());
        const auto old = qvariant_cast<VideoBuildContext>(started.first().first());
        options.forceDerivedRebuild = true;
        coordinator->start(path, options);
        const auto run = vm->runningBuildId();
        QVERIFY(!run.isEmpty());
        QVERIFY(run != old.buildId);
        QCOMPARE(terminated.size(), 1);
        QVERIFY(vm->isIndexing());
        const auto percent = vm->indexPercent();
        emit service->buildProgress(old, 99, "stale progress");
        VideoBuildManifest failed;
        failed.filePath = path;
        failed.buildId = old.buildId;
        failed.state = ArtifactState::Failed;
        emit service->buildTerminated(old, failed);
        VideoRepresentation stale;
        stale.metadata.filePath = path;
        stale.build = failed;
        stale.videoSummary = "stale same-path result";
        emit service->representationReady(old, stale);
        QCOMPARE(vm->runningBuildId(), run);
        QCOMPARE(vm->indexPercent(), percent);
        QVERIFY(vm->isIndexing());
        QVERIFY(vm->videoSummary() != stale.videoSummary);
        QTRY_VERIFY(pending.size() >= 2);
        completePending();
        QTRY_VERIFY(!coordinator->isRunning());
        QCOMPARE(terminated.size(), 2);
        QCOMPARE(vm->displayedBuildId(), run);
        QVERIFY(!vm->isIndexing());
    }
    void cacheHitTerminatesRunWithDifferentResultIdentity() {
        coordinator->start(path, options);
        QTRY_VERIFY(!coordinator->isRunning());
        const auto active = vm->displayedBuildId();
        const int calls = backend.extractions;
        QSignalSpy started(coordinator.get(), &VideoRAGBuildCoordinator::buildStarted);
        QSignalSpy terminated(coordinator.get(), &VideoRAGBuildCoordinator::buildTerminated);
        coordinator->start(path, options);
        QCOMPARE(terminated.size(), 1);
        const auto context = qvariant_cast<VideoBuildContext>(started.first().first());
        const auto result = qvariant_cast<VideoBuildManifest>(terminated.first()[1]);
        QVERIFY(context.buildId != active);
        QCOMPARE(result.buildId, active);
        QCOMPARE(vm->displayedBuildId(), active);
        QVERIFY(vm->runningBuildId().isEmpty());
        QVERIFY(!vm->isIndexing());
        QCOMPARE(backend.extractions, calls);
    }
    void switchVideoCancelFailureAndLateReplies() {
        QSignalSpy started(coordinator.get(), &VideoRAGBuildCoordinator::buildStarted);
        QSignalSpy terminated(coordinator.get(), &VideoRAGBuildCoordinator::buildTerminated);
        holdModel();
        coordinator->start(path, options);
        QTRY_VERIFY(!pending.isEmpty());
        const auto old = qvariant_cast<VideoBuildContext>(started.first().first());
        const auto other = createVideo("other.mp4");
        vm->onVideoOpened(other);
        coordinator->start(other, options);
        QVERIFY(vm->isIndexing());
        emit service->buildProgress(old, 88, "old video");
        QVERIFY(vm->indexPercent() != 88);
        coordinator->cancel();
        coordinator->cancel();
        QCOMPARE(terminated.size(), 2);
        QVERIFY(!vm->isIndexing());
        completePending();
        QTest::qWait(30);
        QCOMPARE(terminated.size(), 2);
        QVERIFY(store->activeBuild(old.videoId).buildId.isEmpty());
        const auto missing = directory.filePath("missing.mp4");
        vm->onVideoOpened(missing);
        coordinator->start(missing, options);
        QCOMPARE(terminated.size(), 3);
        QCOMPARE(qvariant_cast<VideoBuildManifest>(terminated.last()[1]).state, ArtifactState::Failed);
        QVERIFY(!vm->isIndexing());
    }
    void openingAfterStartAdoptsRunAndDestructionIgnoresReplies() {
        vm.reset();
        holdModel();
        coordinator->start(path, options);
        QTRY_VERIFY(!pending.isEmpty());
        vm = std::make_unique<VideoAnalysisViewModel>(service.get(), nullptr);
        vm->onVideoOpened(path);
        QVERIFY(vm->isIndexing());
        QVERIFY(!vm->runningBuildId().isEmpty());
        coordinator.reset();
        QVERIFY(!vm->isIndexing());
        QVERIFY(!service->runningBuildContext());
        vm.reset();
        auto replies = std::exchange(pending, {});
        for (const auto &reply : replies) reply();
        service->cancelBuild(); // QPointer no longer refers to the deleted coordinator.
        QCoreApplication::processEvents();
        QVERIFY(store->activeBuild(VideoFileIdentity::legacyId(path)).buildId.isEmpty());
    }
    void failedRebuildPreservesDisplayedResultAndProgressValue() {
        coordinator->start(path, options);
        QTRY_VERIFY(!coordinator->isRunning());
        const auto active = vm->displayedBuildId();
        const auto summary = vm->videoSummary();
        SummaryTabWidget widget;
        widget.setViewModel(vm.get());
        EvidenceGridConfig invalid;
        invalid.maxEdge = 0;
        coordinator->setUnitGridConfig(invalid);
        options.forceDerivedRebuild = true;
        QSignalSpy terminal(coordinator.get(), &VideoRAGBuildCoordinator::buildTerminated);
        coordinator->start(path, options);
        QCOMPARE(terminal.size(), 1);
        QCOMPARE(qvariant_cast<VideoBuildManifest>(terminal.first()[1]).state, ArtifactState::Failed);
        QVERIFY(!vm->isIndexing());
        QCOMPARE(vm->displayedBuildId(), active);
        QCOMPARE(vm->videoSummary(), summary);
        QCOMPARE(vm->indexPercent(), 0);
        QVERIFY(widget.findChild<QWidget *>("summaryBuildProgress")->isHidden());
        QCOMPARE(store->activeBuild(VideoFileIdentity::legacyId(path)).buildId, active);
    }
    void synchronousRestartAfterPublicationKeepsPublishedTerminal() {
        QSignalSpy terminal(coordinator.get(), &VideoRAGBuildCoordinator::buildTerminated);
        bool restarted = false;
        QString committed;
        connect(coordinator.get(), &VideoRAGBuildCoordinator::representationReady, this,
                [&](const VideoBuildContext &context, const VideoRepresentation &representation) {
            if (restarted || context.buildId != representation.build.buildId) return;
            restarted = true;
            committed = context.buildId;
            holdModel();
            options.forceDerivedRebuild = true;
            coordinator->start(path, options);
        });
        coordinator->start(path, options);
        QTRY_VERIFY(restarted);
        QCOMPARE(terminal.size(), 1);
        const auto result = qvariant_cast<VideoBuildManifest>(terminal.first()[1]);
        QCOMPARE(result.buildId, committed);
        QVERIFY(result.state == ArtifactState::Ready || result.state == ArtifactState::Partial);
        QVERIFY(vm->isIndexing());
        coordinator->cancel();
        QCOMPARE(terminal.size(), 2);
        QCOMPARE(store->activeBuild(VideoFileIdentity::legacyId(path)).buildId, committed);
        QVERIFY(store->activeBuild(VideoFileIdentity::legacyId(path)).state != ArtifactState::Cancelled);
    }
};
QTEST_MAIN(VideoAnalysisLifecycleTests)
#include "video_analysis_lifecycle_tests.moc"
