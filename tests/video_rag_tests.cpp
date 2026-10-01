#include <QtTest>
#include <QTemporaryDir>
#include "infrastructure/databasemanager.h"
#include "service/rag/video_rag_store.h"
#include "service/rag/strategies/video_rag_strategy_registry.h"
#include "model/video_representation_codec.h"
#include "service/rag/semantic_unit_builder.h"

class VideoRAGTests : public QObject {
    Q_OBJECT
private slots:
    void migrationAndPublication() {
        QTemporaryDir dir;QVERIFY(dir.isValid());
        auto db=DatabaseManager::instance();QVERIFY(db->initialize(dir.filePath("test.sqlite")));
        VideoRAGStore store(db);QVERIFY(store.initialize());QVERIFY(store.initialize());
        VideoRepresentation r;r.videoId="fixture";r.metadata.durationMs=90000;r.metadata.hasAudio=true;
        Scene s;s.id=0;s.endMs=90000;s.keyframeMs=123;s.keyframePath="frame.jpg";
        SceneFrame f;f.requestedMs=120;f.ptsMs=123;f.imagePath="frame.jpg";s.representativeFrames<<f;r.scenes<<s;
        VideoChunk raw;raw.chunkId="speech1";raw.videoId=r.videoId;raw.startMs=0;raw.endMs=90000;
        raw.textContent="原始证据";raw.chunkType=VideoChunk::SpeechSegment;raw.metadata.insert("raw_snapshot_id","raw1");
        QVERIFY(store.saveRawSnapshot("raw1",r.videoId,representationToJson(r),{raw}));
        auto restored=representationFromJson(store.loadRawSnapshot("raw1"));
        QCOMPARE(restored.metadata.durationMs,90000);QVERIFY(restored.metadata.hasAudio);
        QCOMPARE(restored.scenes.first().representativeFrames.first().ptsMs,123);
        VideoBuildManifest m;m.videoId=r.videoId;m.filePath=dir.filePath("fixture.mp4");m.buildId="b1";m.rawSnapshotId="raw1";m.state=ArtifactState::Partial;
        SemanticUnit u;u.unitId="u1";u.buildId="b1";u.endMs=90000;u.sourceChunkIds<<"speech1";
        QVERIFY(store.saveUnitBatch(m,{u},{}));QVERIFY(store.publishBuild(m,{}));
        QCOMPARE(store.activeBuild(r.videoId).buildId,QString("b1"));
        auto second=m;second.buildId="b2";u.buildId="b2";QVERIFY(store.saveUnitBatch(second,{u},{}));
        QVERIFY(!store.publishBuild(second,"wrong"));QCOMPARE(store.activeBuild(r.videoId).buildId,QString("b1"));
        QVERIFY(store.publishBuild(second,"b1"));
        VideoRAGStore reopened(db);QVERIFY(reopened.initialize());reopened.loadVideo(r.videoId);
        QCOMPARE(reopened.activeBuild(r.videoId).buildId,QString("b2"));QCOMPARE(reopened.listUnits("b2").size(),1);
        QCOMPARE(reopened.listChunks(VideoRAGStore::TextSegments,r.videoId).size(),1);
    }
    void strategyMapping() {
        VideoContentProfile p;AvailableCapabilities caps;
        QCOMPARE(VideoRAGStrategyRegistry::resolve(p,caps).strategyId,QString("generic_v1"));
        p.primaryType=VideoContentType::Meeting;auto plan=VideoRAGStrategyRegistry::resolve(p,caps);
        QVERIFY(plan.audioFirst);QVERIFY(plan.factKinds.contains("decision"));
        p.primaryType=VideoContentType::Interview;QVERIFY(VideoRAGStrategyRegistry::resolve(p,caps).factKinds.contains("answer"));
        p.primaryType=VideoContentType::Educational;QCOMPARE(VideoRAGStrategyRegistry::resolve(p,caps).unitKind,QString("concept"));
        p.primaryType=VideoContentType::Tutorial;QCOMPARE(VideoRAGStrategyRegistry::resolve(p,caps).unitKind,QString("step"));
    }
    void segmentationAndCoverage() {
        VideoRepresentation r;r.metadata.durationMs=240000;Scene shot;shot.id=0;shot.endMs=240000;r.scenes<<shot;
        QVector<VideoChunk> raw;
        for(int i=0;i<8;++i) {VideoChunk c;c.chunkId=QString("s%1").arg(i);c.videoId="v";c.startMs=i*30000;c.endMs=(i+1)*30000;c.chunkType=VideoChunk::SpeechSegment;c.textContent=i==4?QStringLiteral("接下来学习第二个主题"):QStringLiteral("解释概念和例子");raw<<c;}
        VideoContentProfile p;p.primaryType=VideoContentType::Educational;
        auto plan=VideoRAGStrategyRegistry::resolve(p,{});auto units=SemanticUnitBuilder::candidates(r,raw,plan,"b");
        QVERIFY(units.size()>1);QVERIFY(units.last().sourceChunkIds.contains("s7"));
        plan.evidencePageChars=768;raw[0].textContent=QString(5000,QChar(0x4E2D));
        auto pages=SemanticUnitBuilder::pages(units.first(),raw,plan);int covered=0;
        for(const auto& page:pages) for(auto ev:page.evidence) if(ev.toObject()["source_id"]=="s0") covered+=ev.toObject()["text"].toString().size();
        QCOMPARE(covered,5000);
        QVector<SemanticUnit> corrected;QString error;
        QJsonObject result{{"units",QJsonArray{QJsonObject{{"start_ms",0},{"end_ms",240000},{"source_chunk_ids",QJsonArray{"invented"}}}}}};
        QVERIFY(!SemanticUnitBuilder::correct(result,units,raw,plan,&corrected,&error));
        QJsonArray facts{QJsonObject{{"kind","concept"},{"text","虚构引用"},{"source_chunk_ids",QJsonArray{"invented"}}}};bool valid=true;
        QVERIFY(SemanticUnitBuilder::validatedFacts(facts,pages.first(),plan,&valid).isEmpty());QVERIFY(!valid);
        p.primaryType=VideoContentType::Tutorial;auto steps=SemanticUnitBuilder::candidates(r,raw,VideoRAGStrategyRegistry::resolve(p,{}),"t");
        QCOMPARE(steps.first().nextUnitId,steps[1].unitId);QCOMPARE(steps[1].previousUnitId,steps.first().unitId);
    }
};
QTEST_GUILESS_MAIN(VideoRAGTests)
#include "video_rag_tests.moc"
