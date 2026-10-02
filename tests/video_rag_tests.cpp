#include "infrastructure/databasemanager.h"
#include "model/video_representation_codec.h"
#include "service/agent/tools/get_semantic_unit_tool.h"
#include "service/agent/workflow/workflow_checkpoint.h"
#include "service/rag/evidence_composer.h"
#include "service/rag/qa_cache_manager.h"
#include "service/rag/semantic_unit_builder.h"
#include "service/rag/unit_carry_context.h"
#include "service/rag/strategies/video_rag_strategy_registry.h"
#include "service/rag/video_rag_retriever.h"
#include "service/rag/video_rag_store.h"
#include "util/video_file_identity.h"
#include "video_rag_fixture.h"
#include <QTemporaryDir>
#include <QtTest>
#include "util/video_rag_log.h"
#include <future>

struct CloseFixtureDatabase {
    ~CloseFixtureDatabase() { DatabaseManager::instance()->close(); }
};

class VideoRAGTests : public QObject {
    Q_OBJECT
  private slots:
    void buildLogConcurrentAndThrottled() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        VideoRagLog log("build-test", "video-test", dir.path());
        QVERIFY(log.available());
        std::vector<std::future<void>> writers;
        for (int worker = 0; worker < 4; ++worker)
            writers.push_back(std::async(std::launch::async, [&, worker] {
                for (int i = 0; i < 40; ++i)
                    log.event("understand", "page", "diagnostic", {{"worker", worker}}, VideoRagLog::Level::Debug);
            }));
        for (auto& writer : writers) writer.get();
        for (int i = 0; i <= 100; ++i) log.progress("understand", i, 100, "progress");
        log.event("build", "finished", "done", {{"status", "partial"}}, VideoRagLog::Level::Warning);
        QFile file(log.filePath()); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto lines = file.readAll().trimmed().split('\n');
        QCOMPARE(lines.size(), 163); // 160 detail rows + first/final progress + terminal result
        qint64 sequence = 0;
        for (const auto& line : lines) {
            QJsonParseError error;
            const auto row = QJsonDocument::fromJson(line, &error).object();
            QCOMPARE(error.error, QJsonParseError::NoError);
            QCOMPARE(row["sequence"].toInteger(), ++sequence);
            QCOMPARE(row["build_id"].toString(), QString("build-test"));
            QCOMPARE(row["video_id"].toString(), QString("video-test"));
            QVERIFY(!row["timestamp_utc"].toString().isEmpty());
        }
        QCOMPARE(QJsonDocument::fromJson(lines.last()).object()["fields"].toObject()["status"].toString(), QString("partial"));
        // A file in place of a directory must not prevent console logging.
        VideoRagLog unavailable("other-build", "video", log.filePath());
        QVERIFY(!unavailable.available());
        unavailable.event("build", "failed", "failure", {}, VideoRagLog::Level::Error);
    }
    void rollingCarryValidationAndBudget() {
        const QJsonArray facts{QJsonObject{{"text", "early important fact"},
                                         {"source_chunk_ids", QJsonArray{"source0"}}}};
        auto carry = [](const QString& ref, const QString& stateText) {
            return QJsonObject{{"topic", "topic"},
                {"current_state", QJsonObject{{"text", stateText}, {"fact_refs", QJsonArray{ref}}}},
                {"key_fact_refs", QJsonArray{ref}}, {"pending_threads", QJsonArray{"open question"}},
                {"uncertainties", QJsonArray{}}};
        };
        UnitCarryContext context;
        QCOMPARE(context.input()["last_successful_page"].toInt(), -1);
        QVERIFY(context.acceptPage(0, facts, carry("P0.F0", "confirmed")));
        QVERIFY(context.acceptPage(1, {}, carry("P0.F0", "still confirmed")));
        QCOMPARE(context.input()["key_facts"].toArray()[0].toObject()["text"].toString(),
                 QString("early important fact"));
        context.failPage();
        context.failPage();
        QCOMPARE(context.input()["gap_count"].toInt(), 2);
        QCOMPARE(context.input()["last_successful_page"].toInt(), 1);
        // Unknown/future refs and missing carry preserve accepted page, using program fallback.
        QVERIFY(!context.acceptPage(4, facts, carry("P99.F0", "invented")));
        QVERIFY(context.input()["current_state"].toObject()["text"].toString() != "invented");
        QCOMPARE(context.input()["gap_count"].toInt(), 0);
        QCOMPARE(context.input()["last_successful_page"].toInt(), 4);
        QVERIFY(!context.acceptPage(5, {}, QJsonValue{}));
        UnitCarryContext otherUnit;
        QVERIFY(!otherUnit.acceptPage(1, {}, carry("P0.F0", "cross unit")));
        auto invalid = carry("P0.F0", "new state");
        invalid["topic"] = QString(81, 'x');
        QVERIFY(!context.acceptPage(6, {}, invalid));
        // Valid replacement drops resolved threads rather than accumulating history.
        auto resolved = carry("P0.F0", "resolved");
        auto resolvedState = resolved["current_state"].toObject();
        resolvedState["extra_model_field"] = "untrusted extra";
        resolved["current_state"] = resolvedState;
        resolved["pending_threads"] = QJsonArray{};
        QVERIFY(context.acceptPage(7, {}, resolved));
        QVERIFY(context.input()["pending_threads"].toArray().isEmpty());
        QVERIFY(!context.input()["current_state"].toObject().contains("extra_model_field"));
        const QJsonArray huge{QJsonObject{{"text", QString(2000, 'x')}}};
        QVERIFY(!context.acceptPage(8, huge, carry("P8.F0", "huge fact")));
        QVERIFY(QString::fromUtf8(QJsonDocument(context.input()).toJson(QJsonDocument::Compact)).size() <= 1000);
        QVERIFY(!context.input()["current_state"].toObject()["text"].toString().contains("huge"));
        auto oldPlan = VideoRAGBuildPlan::fromJson(QJsonObject{});
        VideoRAGBuildPlan newPlan;
        QVERIFY(oldPlan.unitUnderstandingVersion != newPlan.unitUnderstandingVersion);
        QCOMPARE(VideoRAGBuildPlan::fromJson(newPlan.toJson()).fingerprint(), newPlan.fingerprint());
    }

    void concurrentUnitsSerialPagesAndCarry_data() {
        QTest::addColumn<int>("concurrency");
        QTest::newRow("one") << 1;
        QTest::newRow("two") << 2;
        QTest::newRow("three") << 3;
    }
    void concurrentUnitsSerialPagesAndCarry() {
        QFETCH(int, concurrency);
        QTemporaryDir dir;
        CloseFixtureDatabase closeBeforeTempDirectory;
        auto* db = DatabaseManager::instance();
        QVERIFY(db->initialize(dir.filePath("concurrent.sqlite")));
        VideoRAGStore store(db);
        QVERIFY(store.initialize());
        FixtureBackend backend;
        backend.longSpeechChars = 9000;
        backend.framePath = dir.filePath("frame.png");
        QImage image(64, 64, QImage::Format_RGB32); image.fill(Qt::blue);
        QVERIFY(image.save(backend.framePath));
        const auto path = dir.filePath("fixture.mp4");
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("concurrency"); file.close();
        VideoRAGBuildCoordinator coordinator(&backend, &store);
        coordinator.setUnitConcurrency(concurrency);
        FixtureBackend::installModel(coordinator);
        QSet<QString> active;
        QHash<QString, int> nextPages;
        QHash<QString, QJsonObject> previousInput;
        QHash<QString, int> attempts;
        QVector<UnitAnalysisRequest> identities;
        int maximum = 0, sent = 0, summaries = 0;
        QString retriedPage;
        QElapsedTimer backoff;
        coordinator.setModelRequest([&](const auto&, const QString& system, const QString& text,
                                        const auto&, std::function<void(QString)> done) {
            if (!text.contains("local_candidates")) {
                ++summaries;
                QVERIFY(active.isEmpty());
            }
            QTimer::singleShot(0, &coordinator, [=] { done(FixtureBackend::modelReply(system, text)); });
        });
        coordinator.setUnitModelRequest([&](const UnitAnalysisRequest& r, const QString& system,
                                            const QString& text, const auto&, std::function<void(ModelReply)> done) {
            identities << r;
            QVERIFY(!r.requestId.isEmpty());
            QVERIFY(!active.contains(r.unitId));
            QCOMPARE(r.pageOrdinal, nextPages.value(r.unitId));
            const auto input = SemanticUnitBuilder::parseObject(text);
            const auto carry = input["carry_context"].toObject();
            QVERIFY(QString::fromUtf8(QJsonDocument(carry).toJson(QJsonDocument::Compact)).size() <= 1000);
            if (r.attempt == 1) {
                QCOMPARE(carry, previousInput.value(r.pageId));
                QVERIFY(backoff.elapsed() >= 1000);
            }
            else previousInput[r.pageId] = carry;
            if (r.pageOrdinal == 0) QCOMPARE(carry["last_successful_page"].toInt(), -1);
            else QCOMPARE(carry["last_successful_page"].toInt(), r.pageOrdinal - 1);
            active.insert(r.unitId);
            maximum = qMax(maximum, int(active.size()));
            QVERIFY(maximum <= concurrency);
            ++sent;
            const int ordinal = r.pageOrdinal;
            const bool failOnce = retriedPage.isEmpty() && ordinal == 1;
            if (failOnce) retriedPage = r.pageId;
            ++attempts[r.pageId];
            // Reverse completion delays to break original unit order.
            QTimer::singleShot(10 + (2 - r.workerId) * 10, &coordinator, [&, r, system, text, done, failOnce] {
                active.remove(r.unitId);
                if (failOnce) {
                    backoff.start();
                    ModelReply limited{{}, "fixture rate limit"};
                    limited.httpStatus = 429;
                    limited.retryAfterMs = 10;
                    done(limited);
                    done({"{}", {}}); // duplicate result must not advance retry/page/coverage
                    return;
                }
                auto result = SemanticUnitBuilder::parseObject(FixtureBackend::modelReply(system, text));
                result["summary"] = QString("unit %1 page %2").arg(r.unitId).arg(r.pageOrdinal);
                auto accepted = result["facts"].toArray();
                auto fact = accepted[0].toObject();
                fact["text"] = result["summary"];
                accepted[0] = fact;
                result["facts"] = accepted;
                const auto ref = QString("P%1.F0").arg(r.pageOrdinal);
                result["carry_context"] = QJsonObject{{"topic", "fixture"},
                    {"current_state", QJsonObject{{"text", "confirmed"}, {"fact_refs", QJsonArray{ref}}}},
                    {"key_fact_refs", QJsonArray{ref}}, {"pending_threads", QJsonArray{}},
                    {"uncertainties", QJsonArray{}}};
                // Intentionally invalid carry on one page: no extra model retry.
                if (r.pageOrdinal == 2) result.remove("carry_context");
                ++nextPages[r.unitId];
                const auto body = QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact));
                done({body, {}});
                done({body, {}});
            });
        });
        QVector<int> progress;
        connect(&coordinator, &VideoRAGBuildCoordinator::progress, &coordinator,
                [&](int value, const QString& message) {
            if (message.startsWith(QStringLiteral("理解语义单元"))) progress << value;
        });
        QSignalSpy finished(&coordinator, &VideoRAGBuildCoordinator::finished);
        BuildOptions options; options.typeOverride = VideoContentType::Unknown;
        coordinator.start(path, options);
        QTRY_VERIFY_WITH_TIMEOUT(!coordinator.isRunning(), 10000);
        QCOMPARE(finished.size(), 1);
        QCOMPARE(maximum, concurrency);
        QCOMPARE(summaries, 1);
        QVERIFY(nextPages.size() >= 3);
        QVERIFY(!retriedPage.isEmpty());
        QCOMPARE(attempts.value(retriedPage), 2);
        const auto build = store.activeBuild(VideoFileIdentity::legacyId(path));
        const auto metrics = build.artifacts["unit_analysis"].toObject();
        QCOMPARE(metrics["max_in_flight"].toInt(), concurrency);
        QCOMPARE(metrics["retries"].toInt(), 1);
        QCOMPARE(metrics["rate_limits"].toInt(), 1);
        QVERIFY(metrics["carry_fallbacks"].toInt() > 0);
        QCOMPARE(sent, metrics["total_pages"].toInt() + 1);
        QCOMPARE(metrics["completed_pages"].toInt(), metrics["total_pages"].toInt());
        QVERIFY(std::is_sorted(progress.begin(), progress.end()));
        auto units = store.listUnits(build.buildId);
        int64_t last = -1;
        for (const auto& unit : units) {
            if (unit.kind == "chapter") continue;
            QVERIFY(unit.startMs >= last); last = unit.startMs;
            QVERIFY(unit.coverage.complete());
        }
        QSet<QString> requestIds;
        for (const auto& r : identities) {
            QVERIFY(!requestIds.contains(r.requestId)); requestIds.insert(r.requestId);
        }
    }

    void unitTimeoutIsolationGapAndLateReply() {
        QTemporaryDir dir;
        CloseFixtureDatabase closeBeforeTempDirectory;
        auto* db = DatabaseManager::instance();
        QVERIFY(db->initialize(dir.filePath("timeout.sqlite")));
        VideoRAGStore store(db); QVERIFY(store.initialize());
        FixtureBackend backend;
        backend.longSpeechChars = 9000;
        backend.framePath = dir.filePath("frame.png");
        QImage image(64, 64, QImage::Format_RGB32); image.fill(Qt::blue);
        QVERIFY(image.save(backend.framePath));
        const auto path = dir.filePath("fixture.mp4");
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("timeout"); file.close();
        VideoRAGBuildCoordinator coordinator(&backend, &store);
        FixtureBackend::installModel(coordinator);
        coordinator.setUnitConcurrency(3);
        coordinator.setUnitWatchdogTimeoutProvider([](int) { return 100; });
        QStringList cancellations;
        coordinator.setUnitRequestCancellation([&](const QString& id) { cancellations << id; });
        QString failedUnit;
        QVector<std::function<void(ModelReply)>> late;
        bool sawGap = false;
        coordinator.setUnitModelRequest([&](const UnitAnalysisRequest& r, const QString& system,
                                            const QString& text, const auto&, std::function<void(ModelReply)> done) {
            if (failedUnit.isEmpty()) failedUnit = r.unitId;
            const auto input = SemanticUnitBuilder::parseObject(text);
            if (r.unitId == failedUnit && r.pageOrdinal == 1) {
                late << done; // let both attempts time out
                return;
            }
            const auto carry = input["carry_context"].toObject();
            if (r.unitId == failedUnit && r.pageOrdinal == 2) {
                QCOMPARE(carry["gap_count"].toInt(), 1);
                QCOMPARE(carry["last_successful_page"].toInt(), 0);
                sawGap = true;
            }
            QTimer::singleShot(5, &coordinator, [done, system, text] {
                done({FixtureBackend::modelReply(system, text), {}});
            });
        });
        QSignalSpy finished(&coordinator, &VideoRAGBuildCoordinator::finished);
        BuildOptions options; options.typeOverride = VideoContentType::Unknown;
        coordinator.start(path, options);
        QTRY_VERIFY_WITH_TIMEOUT(!coordinator.isRunning(), 10000);
        QVERIFY(sawGap);
        QCOMPARE(cancellations.size(), 2);
        QCOMPARE(late.size(), 2);
        QCOMPARE(finished.size(), 1);
        const auto build = store.activeBuild(VideoFileIdentity::legacyId(path));
        QCOMPARE(build.state, ArtifactState::Partial);
        int completeOtherUnits = 0;
        for (const auto& u : store.listUnits(build.buildId)) {
            if (u.kind == "chapter") continue;
            if (u.unitId == failedUnit) QCOMPARE(u.coverage.failedPages.size(), 1);
            else { QVERIFY(u.coverage.complete()); ++completeOtherUnits; }
        }
        QVERIFY(completeOtherUnits >= 2);
        for (const auto& done : late) done({"{\"summary\":\"stale\",\"facts\":[]}", {}});
        QCOMPARE(finished.size(), 1);
        QCOMPARE(store.activeBuild(build.videoId).buildId, build.buildId);
        // Superseding a build while its pages are active must reject their old generation.
        QVector<std::function<void(ModelReply)>> replaced;
        coordinator.setUnitModelRequest([&](const auto&, const auto&, const auto&, const auto&,
                                            std::function<void(ModelReply)> done) { replaced << done; });
        options.forceDerivedRebuild = true;
        coordinator.start(path, options);
        QTRY_VERIFY(replaced.size() >= 2);
        coordinator.cancel();
        coordinator.setUnitModelRequest([&](const auto&, const QString& system, const QString& text,
                                            const auto&, std::function<void(ModelReply)> done) {
            QTimer::singleShot(0, &coordinator, [=] { done({FixtureBackend::modelReply(system, text), {}}); });
        });
        coordinator.start(path, options);
        for (const auto& done : replaced) done({"{\"summary\":\"stale\",\"facts\":[]}", {}});
        QTRY_VERIFY_WITH_TIMEOUT(!coordinator.isRunning(), 10000);
        QCOMPARE(finished.size(), 3); // initial Partial, cancelled candidate, fresh successful build
        QVERIFY(store.activeBuild(build.videoId).buildId != build.buildId);
        // This oversized fixture may remain Partial due to upstream segmentation budget fallback.
        // Its replacement build must have no page failures from the cancelled generation.
        const auto fresh = store.activeBuild(build.videoId);
        QVERIFY(!fresh.diagnostics.join(";").contains("analysis_failed:"));
        for (const auto& u : store.listUnits(fresh.buildId)) QVERIFY(u.coverage.complete());
    }

    void migrationAndPublication() {
        QTemporaryDir dir;
        CloseFixtureDatabase closeBeforeTempDirectory;
        QVERIFY(dir.isValid());
        auto db = DatabaseManager::instance();
        QVERIFY(db->initialize(dir.filePath("test.sqlite")));
        VideoRAGStore store(db);
        QVERIFY(store.initialize());
        QVERIFY(store.initialize());
        VideoRepresentation r;
        r.videoId = "fixture";
        r.metadata.durationMs = 90000;
        r.metadata.hasAudio = true;
        Scene s;
        s.id = 0;
        s.endMs = 90000;
        s.keyframeMs = 123;
        s.keyframePath = "frame.jpg";
        SceneFrame f;
        f.requestedMs = 120;
        f.ptsMs = 123;
        f.imagePath = "frame.jpg";
        s.representativeFrames << f;
        r.scenes << s;
        VideoChunk raw;
        raw.chunkId = "speech1";
        raw.videoId = r.videoId;
        raw.startMs = 0;
        raw.endMs = 90000;
        raw.textContent = "原始证据";
        raw.chunkType = VideoChunk::SpeechSegment;
        raw.metadata.insert("raw_snapshot_id", "raw1");
        QVERIFY(store.saveRawSnapshot("raw1", r.videoId, representationToJson(r), {raw}));
        raw.textContent = "attempted mutation";
        QVERIFY(!store.saveRawSnapshot("raw1", r.videoId, representationToJson(r), {raw}));
        QVERIFY(!store.insertChunk(VideoRAGStore::TextSegments, raw));
        QCOMPARE(store.rawChunks("raw1").first().textContent, QString("原始证据"));
        auto restored = representationFromJson(store.loadRawSnapshot("raw1"));
        QCOMPARE(restored.metadata.durationMs, 90000);
        QVERIFY(restored.metadata.hasAudio);
        QCOMPARE(restored.scenes.first().representativeFrames.first().ptsMs, 123);
        VideoBuildManifest m;
        m.videoId = r.videoId;
        m.filePath = dir.filePath("fixture.mp4");
        m.buildId = "b1";
        m.rawSnapshotId = "raw1";
        m.state = ArtifactState::Partial;
        SemanticUnit u;
        u.unitId = "u1";
        u.buildId = "b1";
        u.endMs = 90000;
        u.sourceChunkIds << "speech1";
        QVERIFY(store.saveUnitBatch(m, {u}, {}));
        QVERIFY(store.publishBuild(m, {}));
        QCOMPARE(store.activeBuild(r.videoId).buildId, QString("b1"));
        QVERIFY(!store.saveUnitBatch(m, {u}, {}));
        WorkflowCheckpoint checkpoint;
        checkpoint.initialize();
        WorkflowState state;
        state.set("video_id", r.videoId);
        state.set("build_id", m.buildId);
        state.set("build_revision", m.revision);
        checkpoint.save("fixture_checkpoint", state, "reason");
        QVERIFY(checkpoint.load("fixture_checkpoint"));
        QACacheManager qa(&store, nullptr);
        qa.setQueryEncoder([](const QString &) { return std::vector<float>{1.0f, 0.0f}; });
        RetrievalResult proof;
        proof.chunk = store.rawChunks("raw1").first();
        proof.chunk.metadata["read_build_id"] = m.buildId;
        proof.chunk.metadata["read_revision"] = m.revision;
        qa.cache(r.videoId, "原始证据是什么", "原始证据", 0.9f, {}, {proof});
        QVERIFY(qa.tryAnswer(r.videoId, "原始证据是什么"));
        auto second = m;
        second.buildId = "b2";
        u.buildId = "b2";
        QVERIFY(store.saveUnitBatch(second, {u}, {}));
        QVERIFY(!store.publishBuild(second, "wrong"));
        QCOMPARE(store.activeBuild(r.videoId).buildId, QString("b1"));
        QVERIFY(store.publishBuild(second, "b1"));
        QVERIFY(!store.saveUnitBatch(m, {u}, {}));
        QVERIFY(!checkpoint.load("fixture_checkpoint"));
        QVERIFY(!qa.tryAnswer(r.videoId, "原始证据是什么"));
        VideoRAGStore reopened(db);
        QVERIFY(reopened.initialize());
        reopened.loadVideo(r.videoId);
        QCOMPARE(reopened.activeBuild(r.videoId).buildId, QString("b2"));
        QCOMPARE(reopened.listUnits("b2").size(), 1);
        QCOMPARE(reopened.listChunks(VideoRAGStore::TextSegments, r.videoId).size(), 1);
    }
    void strategyMapping() {
        VideoContentProfile p;
        AvailableCapabilities caps;
        QCOMPARE(VideoRAGStrategyRegistry::resolve(p, caps).strategyId, QString("generic_v1"));
        p.primaryType = VideoContentType::Meeting;
        auto plan = VideoRAGStrategyRegistry::resolve(p, caps);
        QVERIFY(plan.audioFirst);
        QVERIFY(plan.factKinds.contains("decision"));
        p.primaryType = VideoContentType::Interview;
        QVERIFY(VideoRAGStrategyRegistry::resolve(p, caps).factKinds.contains("answer"));
        p.primaryType = VideoContentType::Educational;
        QCOMPARE(VideoRAGStrategyRegistry::resolve(p, caps).unitKind, QString("concept"));
        p.primaryType = VideoContentType::Tutorial;
        QCOMPARE(VideoRAGStrategyRegistry::resolve(p, caps).unitKind, QString("step"));
    }
    void fiveStrategyPipeline_data() {
        QTest::addColumn<VideoContentType>("type");
        QTest::newRow("meeting") << VideoContentType::Meeting;
        QTest::newRow("interview") << VideoContentType::Interview;
        QTest::newRow("lecture") << VideoContentType::Educational;
        QTest::newRow("tutorial") << VideoContentType::Tutorial;
        QTest::newRow("generic") << VideoContentType::Unknown;
    }
    void fiveStrategyPipeline() {
        QFETCH(VideoContentType, type);
        QTemporaryDir dir;
        CloseFixtureDatabase closeBeforeTempDirectory;
        auto *db = DatabaseManager::instance();
        QVERIFY(db->initialize(dir.filePath("fixture.sqlite")));
        VideoRAGStore store(db);
        QVERIFY(store.initialize());
        FixtureBackend backend;
        QImage image(64, 64, QImage::Format_RGB32);
        image.fill(Qt::blue);
        backend.framePath = dir.filePath("frame.png");
        QVERIFY(image.save(backend.framePath));
        const auto path = dir.filePath("fixture.mp4");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("five strategy fixture");
        file.close();
        VideoRAGBuildCoordinator coordinator(&backend, &store);
        FixtureBackend::installModel(coordinator);
        BuildOptions options;
        options.typeOverride = type;
        coordinator.start(path, options);
        QTRY_VERIFY(!coordinator.isRunning());
        const auto manifest = store.activeBuild(VideoFileIdentity::legacyId(path));
        QCOMPARE(manifest.state, ArtifactState::Ready);
        QCOMPARE(manifest.profile.primaryType, type);
        QFile trace(manifest.artifacts["build_log_file"].toString());
        QVERIFY(trace.open(QIODevice::ReadOnly));
        const auto traceBytes = trace.readAll();
        QVERIFY(!traceBytes.contains(QStringLiteral("后半段批准42万元").toUtf8()));
        QStringList started, ended;
        QJsonObject last;
        for (const auto& line : traceBytes.trimmed().split('\n')) {
            const auto row = QJsonDocument::fromJson(line).object();
            QCOMPARE(row["build_id"].toString(), manifest.buildId);
            if (row["event"] == "started" && row["stage"] != "build") started << row["stage"].toString();
            if (row["event"] == "finished" && row["stage"] != "build") ended << row["stage"].toString();
            last = row;
        }
        QCOMPARE(started, ended);
        QVERIFY(started.contains("extract"));
        QVERIFY(started.contains("understand"));
        QCOMPARE(last["event"].toString(), QString("finished"));
        QCOMPARE(last["fields"].toObject()["status"].toString(), QString("ready"));

        const auto units = store.listUnits(manifest.buildId);
        QVERIFY(!units.isEmpty());
        for (const auto &unit : units) {
            QVERIFY(unit.coverage.complete());
            QVERIFY(!unit.sourceChunkIds.isEmpty());
        }
        VideoRAGRetriever retriever(&store);
        VideoRAGRetriever::Constraints c;
        c.videoId = manifest.videoId;
        c.chunkType = VideoChunk::UnitSummary;
        auto hits = retriever.retrieve(QStringLiteral("预算42万元"), c, 20);
        QVERIFY(!hits.isEmpty());
        bool sources = false;
        for (const auto &hit : hits)
            if (!hit.chunk.metadata.value("expanded_sources").toList().isEmpty())
                sources = true;
        QVERIFY(sources);
        if (type == VideoContentType::Tutorial) {
            QVERIFY(units.size() > 1);
            QCOMPARE(units.first().nextUnitId, units[1].unitId);
        }
        // A derived hit has no own keyframe; images come from its raw source expansion.
        RetrievalResult visual;
        visual.chunk.metadata["expanded_sources"] =
            QVariantList{QVariantMap{{"keyframe_path", backend.framePath}}};
        QCOMPARE(EvidenceComposer::mergeFrames({}, {visual, visual}, 4, 128).size(), 1);
    }
    void segmentationAndCoverage() {
        VideoRepresentation r;
        r.metadata.durationMs = 240000;
        Scene shot;
        shot.id = 0;
        shot.endMs = 240000;
        r.scenes << shot;
        QVector<VideoChunk> raw;
        for (int i = 0; i < 8; ++i) {
            VideoChunk c;
            c.chunkId = QString("s%1").arg(i);
            c.videoId = "v";
            c.startMs = i * 30000;
            c.endMs = (i + 1) * 30000;
            c.chunkType = VideoChunk::SpeechSegment;
            c.textContent =
                i == 4 ? QStringLiteral("接下来学习第二个主题") : QStringLiteral("解释概念和例子");
            raw << c;
        }
        VideoContentProfile p;
        p.primaryType = VideoContentType::Educational;
        auto plan = VideoRAGStrategyRegistry::resolve(p, {});
        auto units = SemanticUnitBuilder::candidates(r, raw, plan, "b");
        QVERIFY(units.size() > 1);
        QVERIFY(units.last().sourceChunkIds.contains("s7"));
        plan.evidencePageChars = 768;
        raw[0].textContent = QString(5000, QChar(0x4E2D));
        auto pages = SemanticUnitBuilder::pages(units.first(), raw, plan);
        int covered = 0;
        for (const auto &page : pages)
            for (auto ev : page.evidence)
                if (ev.toObject()["source_id"] == "s0")
                    covered += ev.toObject()["text"].toString().size();
        QCOMPARE(covered, 5000);
        QVector<SemanticUnit> corrected;
        QString error;
        QJsonObject result{{"units", QJsonArray{QJsonObject{{"start_ms", 0},
                                                            {"end_ms", 240000},
                                                            {"source_chunk_ids", QJsonArray{"invented"}}}}}};
        QVERIFY(!SemanticUnitBuilder::correct(result, units, raw, plan, &corrected, &error));
        QJsonArray facts{QJsonObject{
            {"kind", "concept"}, {"text", "虚构引用"}, {"source_chunk_ids", QJsonArray{"invented"}}}};
        bool valid = true;
        QVERIFY(SemanticUnitBuilder::validatedFacts(facts, pages.first(), plan, &valid).isEmpty());
        QVERIFY(!valid);
        p.primaryType = VideoContentType::Tutorial;
        auto steps = SemanticUnitBuilder::candidates(r, raw, VideoRAGStrategyRegistry::resolve(p, {}), "t");
        QCOMPARE(steps.first().nextUnitId, steps[1].unitId);
        QCOMPARE(steps[1].previousUnitId, steps.first().unitId);
        // A question and its answer span two visual shots and stay one dialogue unit.
        VideoRepresentation dialogue;
        dialogue.metadata.durationMs = 60000;
        Scene first;
        first.id = 0;
        first.endMs = 30000;
        Scene second;
        second.id = 1;
        second.startMs = 30000;
        second.endMs = 60000;
        dialogue.scenes = {first, second};
        auto question = raw[1];
        question.chunkId = "question";
        question.startMs = 0;
        question.endMs = 30000;
        question.textContent = QStringLiteral("预算是多少？");
        auto answer = question;
        answer.chunkId = "answer";
        answer.startMs = 30000;
        answer.endMs = 60000;
        answer.textContent = QStringLiteral("预算是42万元。");
        p.primaryType = VideoContentType::Interview;
        auto qa = SemanticUnitBuilder::candidates(dialogue, {question, answer},
                                                  VideoRAGStrategyRegistry::resolve(p, {}), "qa");
        QCOMPARE(qa.size(), 1);
        QCOMPARE(qa.first().shotIds.size(), 2);
    }
    void buildCancellationAndRetrieval() {
        QTemporaryDir dir;
        CloseFixtureDatabase closeBeforeTempDirectory;
        QVERIFY(dir.isValid());
        auto *db = DatabaseManager::instance();
        QVERIFY(db->initialize(dir.filePath("pipeline.sqlite")));
        VideoRAGStore store(db);
        QVERIFY(store.initialize());
        FixtureBackend backend;
        QImage frame(64, 64, QImage::Format_RGB32);
        frame.fill(Qt::blue);
        backend.framePath = dir.filePath("frame.png");
        QVERIFY(frame.save(backend.framePath));
        const QString path = dir.filePath("fixture.mp4");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("deterministic fixture file");
        file.close();
        VideoRAGBuildCoordinator coordinator(&backend, &store);
        FixtureBackend::installModel(coordinator);
        QSignalSpy finished(&coordinator, &VideoRAGBuildCoordinator::finished);
        BuildOptions options;
        options.typeOverride = VideoContentType::Meeting;
        coordinator.start(path, options);
        QTRY_COMPARE(finished.size(), 1);
        const auto videoId = VideoFileIdentity::legacyId(path);
        const auto first = store.activeBuild(videoId);
        QVERIFY(!first.buildId.isEmpty());
        QVERIFY(first.profile.userOverride);
        QCOMPARE(first.profile.primaryType, VideoContentType::Meeting);
        QCOMPARE(backend.probes, 0);
        QVERIFY(!backend.published.semanticUnits.isEmpty());
        VideoRAGRetriever retriever(&store);
        VideoRAGRetriever::Constraints constraints;
        constraints.videoId = videoId;
        auto hits = retriever.retrieve(QStringLiteral("后半段预算42万元"), constraints, 10);
        QVERIFY(!hits.isEmpty());
        bool supported = false;
        for (const auto &hit : hits)
            if (hit.chunk.textContent.contains(QStringLiteral("42万元")))
                supported = true;
        QVERIFY(supported);
        constraints.startMsGte = 60000;
        constraints.endMsLte = 90000;
        auto plan = retriever.compileQueryPlan(QStringLiteral("开头预算"), constraints);
        QCOMPARE(plan.startMs, 60000);
        QCOMPARE(plan.endMs, 90000);
        for (const auto &hit : retriever.retrieve(QStringLiteral("预算"), constraints, 10)) {
            QVERIFY(hit.chunk.endMs > 60000);
            QVERIFY(hit.chunk.startMs < 90000);
        }
        GetSemanticUnitTool tool(&store);
        tool.setVideoId(videoId);
        bool toolDone = false;
        tool.executeAsync("call", QJsonObject{{"timestamp_ms", 60000}}, [&](const ToolResult &result) {
            QVERIFY(result.success);
            toolDone = true;
        });
        QVERIFY(toolDone);
        backend.delayExtraction = true;
        options.forceDerivedRebuild = true;
        options.typeOverride = VideoContentType::Tutorial;
        coordinator.start(path, options);
        QVERIFY(coordinator.isRunning());
        coordinator.cancel();
        QVERIFY(!coordinator.isRunning());
        QCOMPARE(store.typeOverride(videoId, VideoFileIdentity::fingerprint(path)).value(),
                 VideoContentType::Tutorial);
        QCOMPARE(store.activeBuild(videoId).buildId, first.buildId);
        for (const auto &callback : backend.delayed)
            callback();
        QCoreApplication::processEvents();
        QCOMPARE(store.activeBuild(videoId).buildId, first.buildId);
        backend.delayed.clear();
        backend.delayExtraction = false;
        backend.failExtraction = true;
        coordinator.start(path, options);
        QTRY_VERIFY(!coordinator.isRunning());
        QCOMPARE(store.activeBuild(videoId).buildId, first.buildId);
        backend.failExtraction = false;
        coordinator.changeType(path, VideoContentType::Interview);
        QTRY_VERIFY(!coordinator.isRunning());
        auto changed = store.activeBuild(videoId);
        QVERIFY(changed.buildId != first.buildId);
        QCOMPARE(changed.profile.primaryType, VideoContentType::Interview);
        constraints = {};
        constraints.videoId = videoId;
        constraints.buildId = first.buildId;
        constraints.revision = first.revision;
        QVERIFY(retriever.retrieve(QStringLiteral("预算"), constraints).isEmpty());
        BuildOptions automatic;
        automatic.forceDerivedRebuild = true;
        automatic.clearTypeOverride = true;
        coordinator.start(path, automatic);
        QTRY_VERIFY(!coordinator.isRunning());
        QVERIFY(!store.typeOverride(videoId, VideoFileIdentity::fingerprint(path)));
        QVERIFY(!store.activeBuild(videoId).profile.userOverride);
        QCOMPARE(backend.probes, 1);
    }
    void failedPageAndStaleCallback() {
        QTemporaryDir dir;
        CloseFixtureDatabase closeBeforeTempDirectory;
        auto *db = DatabaseManager::instance();
        QVERIFY(db->initialize(dir.filePath("partial.sqlite")));
        VideoRAGStore store(db);
        QVERIFY(store.initialize());
        FixtureBackend backend;
        backend.framePath = dir.filePath("frame.png");
        QImage image(64, 64, QImage::Format_RGB32);
        image.fill(Qt::blue);
        QVERIFY(image.save(backend.framePath));
        auto path = dir.filePath("fixture.mp4");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("partial test fixture");
        file.close();
        VideoRAGBuildCoordinator coordinator(&backend, &store);
        BuildOptions options;
        options.typeOverride = VideoContentType::Meeting;
        coordinator.setModelRequest([](const VideoBuildContext &, const QString &system, const QString &text,
                                       const QList<QImage> &, std::function<void(QString)> done) {
            QTimer::singleShot(0, [=] {
                done(text.contains("core_evidence") ? QString() : FixtureBackend::modelReply(system, text));
            });
        });
        coordinator.start(path, options);
        QTRY_VERIFY(!coordinator.isRunning());
        const auto partial = store.activeBuild(VideoFileIdentity::legacyId(path));
        QCOMPARE(partial.state, ArtifactState::Partial);
        bool failed = false;
        for (const auto &u : store.listUnits(partial.buildId))
            if (!u.coverage.failedPages.isEmpty())
                failed = true;
        QVERIFY(failed);
        QVERIFY(partial.diagnostics.contains("summary_skipped:no_successful_pages"));
        QVERIFY(partial.summary.contains("成功处理0页"));
        QVERIFY(!partial.summary.contains("[…]"));
        for (const auto &chunk : store.listChunks(VideoRAGStore::TextSegments, partial.videoId))
            QVERIFY(chunk.chunkType != VideoChunk::UnitSummary);
        QVector<std::function<void(QString)>> callbacks;
        coordinator.setModelRequest([&](const VideoBuildContext &, const QString &, const QString &,
                                        const QList<QImage> &,
                                        std::function<void(QString)> done) { callbacks << done; });
        options.forceDerivedRebuild = true;
        coordinator.start(path, options);
        QTRY_VERIFY(!callbacks.isEmpty());
        coordinator.cancel();
        for (const auto &callback : callbacks)
            callback("{\"units\":[]}");
        QCOMPARE(store.activeBuild(partial.videoId).buildId, partial.buildId);
        backend.caps.asr = false;
        FixtureBackend::installModel(coordinator);
        coordinator.start(path, options);
        QTRY_VERIFY(!coordinator.isRunning());
        QCOMPARE(store.activeBuild(partial.videoId).state, ArtifactState::Partial);
        backend.caps.asr = true;
        backend.longSpeechChars = 10000;
        int summaryCalls = 0;
        coordinator.setModelRequest([&](const VideoBuildContext &, const QString &system, const QString &text,
                                        const QList<QImage> &, std::function<void(QString)> done) {
            const bool reduction = system.contains(QStringLiteral("全部输入"));
            if (reduction)
                ++summaryCalls;
            QTimer::singleShot(
                0, [=] { done(reduction ? QString() : FixtureBackend::modelReply(system, text)); });
        });
        coordinator.start(path, options);
        QTRY_VERIFY_WITH_TIMEOUT(!coordinator.isRunning(), 10000);
        const auto fallback = store.activeBuild(partial.videoId);
        QCOMPARE(fallback.state, ArtifactState::Partial);
        QVERIFY(fallback.diagnostics.contains("summary_partial"));
        QVERIFY(summaryCalls > 2);
        QVERIFY(summaryCalls < 20);
        int processed = 0;
        for (const auto &unit : store.listUnits(fallback.buildId)) {
            QVERIFY(unit.coverage.complete());
            processed += unit.coverage.processedPages;
        }
        QVERIFY(processed > 10);
    }
    void modelValidationFailures_data() {
        QTest::addColumn<QString>("mode");
        QTest::newRow("plain_text") << QString("plain_text");
        QTest::newRow("bad_schema") << QString("bad_schema");
        QTest::newRow("fabricated_source") << QString("fabricated_source");
        QTest::newRow("http_error") << QString("http_error");
    }
    void modelValidationFailures() {
        QFETCH(QString, mode);
        QTemporaryDir dir;
        CloseFixtureDatabase closeBeforeTempDirectory;
        auto *db = DatabaseManager::instance();
        QVERIFY(db->initialize(dir.filePath("validation.sqlite")));
        VideoRAGStore store(db); QVERIFY(store.initialize());
        FixtureBackend backend;
        backend.framePath = dir.filePath("frame.png");
        QImage image(64,64,QImage::Format_RGB32); image.fill(Qt::blue); QVERIFY(image.save(backend.framePath));
        const auto path = dir.filePath("fixture.mp4");
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("validation fixture"); file.close();
        VideoRAGBuildCoordinator coordinator(&backend, &store);
        int summaryCalls = 0, repairCalls = 0;
        coordinator.setDetailedModelRequest([&](const VideoBuildContext &, const QString &system, const QString &text,
            const QList<QImage> &, std::function<void(ModelReply)> done) {
            ModelReply reply{FixtureBackend::modelReply(system,text), {}};
            if (system.contains("全部输入")) ++summaryCalls;
            if (text.contains("core_evidence")) {
                if (system.contains("修复上次错误")) ++repairCalls;
                if (mode == "plain_text") reply.content = "Markdown answer without JSON";
                if (mode == "bad_schema") reply.content = "{\"summary\":\"text\"}";
                if (mode == "fabricated_source") {
                    auto object = SemanticUnitBuilder::parseObject(reply.content);
                    auto fact = object["facts"].toArray().first().toObject();
                    fact["source_chunk_ids"] = QJsonArray{"invented-source"};
                    object["facts"] = QJsonArray{fact};
                    reply.content = QString::fromUtf8(QJsonDocument(object).toJson());
                }
                if (mode == "http_error") reply.error = "HTTP 429: rate limited";
            }
            QTimer::singleShot(0, [done, reply] { done(reply); });
        });
        BuildOptions options; options.typeOverride = VideoContentType::Meeting;
        coordinator.start(path, options); QTRY_VERIFY(!coordinator.isRunning());
        const auto build = store.activeBuild(VideoFileIdentity::legacyId(path));
        QCOMPARE(build.state, ArtifactState::Partial);
        QFile trace(build.artifacts["build_log_file"].toString());
        QVERIFY(trace.open(QIODevice::ReadOnly));
        const auto traceLines = trace.readAll().trimmed().split('\n');
        const auto terminal = QJsonDocument::fromJson(traceLines.last()).object();
        QCOMPARE(terminal["event"].toString(), QString("finished"));
        QCOMPARE(terminal["level"].toString(), QString("WARN"));
        QCOMPARE(terminal["fields"].toObject()["status"].toString(), QString("partial"));

        QCOMPARE(summaryCalls, 0);
        QVERIFY(repairCalls > 0);
        const QString error = mode == "plain_text" ? "invalid_json" : mode == "bad_schema" ? "invalid_schema"
                            : mode == "fabricated_source" ? "invalid_source" : "HTTP 429";
        QVERIFY(build.diagnostics.join('\n').contains(error));
        QVERIFY(build.summary.contains(error));
        QCOMPARE(build.summary.count(error), 1);
        for (const auto &unit : store.listUnits(build.buildId)) {
            QCOMPARE(unit.coverage.processedPages, 0);
            QVERIFY(!unit.coverage.failedPages.isEmpty());
            QVERIFY(unit.facts.isEmpty());
        }
        QVERIFY(!store.rawChunks(build.rawSnapshotId).isEmpty());
    }
    void successfulRepairAndShortSummaryFallback() {
        QTemporaryDir dir;
        CloseFixtureDatabase closeBeforeTempDirectory;
        auto *db = DatabaseManager::instance(); QVERIFY(db->initialize(dir.filePath("repair.sqlite")));
        VideoRAGStore store(db); QVERIFY(store.initialize());
        FixtureBackend backend; backend.framePath = dir.filePath("frame.png");
        QImage image(64,64,QImage::Format_RGB32); image.fill(Qt::blue); QVERIFY(image.save(backend.framePath));
        const auto path = dir.filePath("fixture.mp4");
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("repair fixture"); file.close();
        VideoRAGBuildCoordinator coordinator(&backend, &store);
        int repairs = 0;
        coordinator.setModelRequest([&](const VideoBuildContext &, const QString &system, const QString &text,
            const QList<QImage> &, std::function<void(QString)> done) {
            auto reply = FixtureBackend::modelReply(system,text);
            if (system.contains("全部输入")) reply.clear();
            else if (text.contains("core_evidence")) {
                if (!system.contains("修复上次错误")) reply = "invalid";
                else {
                    ++repairs;
                    QVERIFY(system.contains("invalid_json"));
                    auto object = SemanticUnitBuilder::parseObject(reply);
                    object["summary"] = "unique-summary-" + SemanticUnitBuilder::parseObject(text)["page_id"].toString();
                    reply = QString::fromUtf8(QJsonDocument(object).toJson());
                }
            }
            QTimer::singleShot(0, [done,reply] { done(reply); });
        });
        BuildOptions options; options.typeOverride = VideoContentType::Meeting;
        coordinator.start(path,options); QTRY_VERIFY(!coordinator.isRunning());
        const auto build = store.activeBuild(VideoFileIdentity::legacyId(path));
        QVERIFY(repairs > 0);
        QVERIFY(build.diagnostics.contains("summary_partial"));
        QVERIFY(!build.diagnostics.join('\n').contains("analysis_failed"));
        int processed = 0;
        for (const auto &unit : store.listUnits(build.buildId)) {
            QVERIFY(unit.coverage.complete());
            if (unit.parentUnitId.isEmpty()) processed += unit.coverage.processedPages;
        }
        QCOMPARE(build.summary.count("unique-summary-"), processed);
        QVERIFY(!build.summary.contains("[…]"));
    }
    void transactionRollbackAndLegacy() {
        QTemporaryDir dir;
        CloseFixtureDatabase closeBeforeTempDirectory;
        auto *db = DatabaseManager::instance();
        QVERIFY(db->initialize(dir.filePath("rollback.sqlite")));
        VideoRAGStore store(db);
        QVERIFY(store.initialize());
        VideoChunk legacy;
        legacy.videoId = "legacy";
        legacy.chunkId = "legacy_fact";
        legacy.textContent = "old evidence";
        legacy.endMs = 1;
        QVERIFY(store.insertChunk(VideoRAGStore::TextSegments, legacy));
        QCOMPARE(store.listChunks(VideoRAGStore::TextSegments, "legacy").size(), 1);
        // Two independent facts at the same time must survive retrieval deduplication.
        auto other = legacy;
        other.chunkId = "other_fact";
        other.textContent = "old evidence budget";
        QVERIFY(store.insertChunk(VideoRAGStore::TextSegments, other));
        VideoRAGRetriever retriever(&store);
        VideoRAGRetriever::Constraints scope;
        scope.videoId = "legacy";
        QCOMPARE(retriever.retrieve("old evidence", scope, 10).size(), 2);
        QVERIFY(db->exec("CREATE TRIGGER reject_units BEFORE INSERT ON video_semantic_units BEGIN SELECT "
                         "RAISE(ABORT,'fixture rollback'); END"));
        VideoBuildManifest m;
        m.buildId = "candidate";
        m.videoId = "legacy";
        m.rawSnapshotId = "raw";
        m.state = ArtifactState::Partial;
        SemanticUnit u;
        u.buildId = m.buildId;
        u.unitId = "unit";
        u.endMs = 1;
        QVERIFY(!store.saveUnitBatch(m, {u}, {}));
        QVERIFY(store.listUnits(m.buildId).isEmpty());
        QVERIFY(store.activeBuild("legacy").buildId.isEmpty());
        QVERIFY(db->query("SELECT * FROM video_rag_builds WHERE build_id='candidate'").isEmpty());
        QCOMPARE(store.listChunks(VideoRAGStore::TextSegments, "legacy").size(), 2);
    }
    void capabilitiesAndLifecycle() {
        QTemporaryDir dir;
        CloseFixtureDatabase closeBeforeTempDirectory;
        auto *db = DatabaseManager::instance();
        QVERIFY(db->initialize(dir.filePath("lifecycle.sqlite")));
        VideoRAGStore store(db);
        QVERIFY(store.initialize());
        FixtureBackend backend;
        QImage frame(64, 64, QImage::Format_RGB32);
        frame.fill(Qt::blue);
        backend.framePath = dir.filePath("frame.png");
        QVERIFY(frame.save(backend.framePath));
        const auto a = dir.filePath("a.mp4"), b = dir.filePath("b.mp4");
        for (const auto &path : {a, b}) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(path.toUtf8());
        }
        VideoRAGBuildCoordinator coordinator(&backend, &store);
        FixtureBackend::installModel(coordinator);
        BuildOptions options;
        options.typeOverride = VideoContentType::Meeting;
        backend.delayExtraction = true;
        coordinator.start(a, options);
        coordinator.start(b, options);
        QCOMPARE(backend.delayed.size(), 2);
        for (const auto &callback : backend.delayed)
            callback();
        QTRY_VERIFY(!coordinator.isRunning());
        QVERIFY(store.activeBuild(VideoFileIdentity::legacyId(a)).buildId.isEmpty());
        QVERIFY(!store.activeBuild(VideoFileIdentity::legacyId(b)).buildId.isEmpty());
        backend.delayed.clear();
        backend.delayExtraction = false;
        const auto previous = store.activeBuild(VideoFileIdentity::legacyId(b));
        QString signature = "model_a";
        coordinator.setModelSignatureProvider([&] { return signature; });
        QVector<std::function<void(QString)>> held;
        coordinator.setModelRequest([&](const VideoBuildContext &, const QString &, const QString &,
                                        const QList<QImage> &,
                                        std::function<void(QString)> done) { held << done; });
        options.forceDerivedRebuild = true;
        coordinator.start(b, options);
        QTRY_VERIFY(!held.isEmpty());
        signature = "model_b";
        held.first()("{}");
        QVERIFY(!coordinator.isRunning());
        QCOMPARE(store.activeBuild(previous.videoId).buildId, previous.buildId);
        held.clear();
        {
            auto scoped = std::make_unique<VideoRAGBuildCoordinator>(&backend, &store);
            scoped->setModelRequest([&](const VideoBuildContext &, const QString &, const QString &,
                                        const QList<QImage> &,
                                        std::function<void(QString)> done) { held << done; });
            scoped->start(b, options);
            QTRY_VERIFY(!held.isEmpty());
        }
        for (const auto &done : held)
            done("{}");
        QCOMPARE(store.activeBuild(previous.videoId).buildId, previous.buildId);
        backend.hasAudio = false;
        backend.caps = {false, false, false};
        FixtureBackend::installModel(coordinator);
        options.typeOverride = VideoContentType::Unknown;
        coordinator.start(b, options);
        QTRY_VERIFY(!coordinator.isRunning());
        QCOMPARE(store.activeBuild(previous.videoId).state, ArtifactState::Ready);
        options.clearTypeOverride = true;
        options.typeOverride.reset();
        coordinator.setModelRequest([](const VideoBuildContext &, const QString &system, const QString &text,
                                       const QList<QImage> &, std::function<void(QString)> done) {
            QTimer::singleShot(0, [=] {
                done(system.contains(QStringLiteral("分类")) ? QString()
                                                             : FixtureBackend::modelReply(system, text));
            });
        });
        coordinator.start(b, options);
        QTRY_VERIFY(!coordinator.isRunning());
        const auto fallback = store.activeBuild(previous.videoId);
        QCOMPARE(fallback.plan.strategyId, QString("generic_v1"));
        QCOMPARE(fallback.profile.source, QString("fallback"));
        QCOMPARE(fallback.state, ArtifactState::Partial);
    }
};
QTEST_GUILESS_MAIN(VideoRAGTests)
#include "video_rag_tests.moc"
