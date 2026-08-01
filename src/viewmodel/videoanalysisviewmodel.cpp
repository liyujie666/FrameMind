#include "viewmodel/videoanalysisviewmodel.h"

#include "service/agent/video_analysis_service.h"
#include "service/agent/video_indexer.h"

#include <QDebug>

VideoAnalysisViewModel::VideoAnalysisViewModel(VideoAnalysisService* analysisService,
                                               VideoIndexer*         indexer,
                                               QObject*              parent)
    : QObject(parent)
    , m_analysis(analysisService)
    , m_indexer(indexer)
{
    connectServices();
}

void VideoAnalysisViewModel::connectServices()
{
    if (m_indexer) {
        connect(m_indexer, &VideoIndexer::progress,
                this, [this](int percent, VideoIndexer::Stage stage, const QString& msg) {
            m_indexPercent    = percent;
            m_indexStageLabel = msg;
            m_isIndexing      = (stage != VideoIndexer::StageDone
                                  && stage != VideoIndexer::StageIdle);
            emit progressChanged(percent, msg);
            emit indexingChanged(m_isIndexing);
        });

        connect(m_indexer, &VideoIndexer::levelReady,
                this, [this](int level, QSharedPointer<VideoRepresentation> repr) {
            if (!repr || repr->metadata.filePath != m_currentPath) return;
            m_repr = repr;

            if (level >= 0) {
                m_scenes = repr->scenes;
                emit scenesReady(m_scenes);
            }
            if (level >= 1) {
                m_speechSegments = repr->speechSegments;
                emit speechSegmentsReady(m_speechSegments);
            }
        });

        connect(m_indexer, &VideoIndexer::indexCompleted,
                this, [this](QSharedPointer<VideoRepresentation> repr) {
            if (!repr || repr->metadata.filePath != m_currentPath) return;
            m_repr = repr;
        });
    }

    if (m_analysis) {
        connect(m_analysis, &VideoAnalysisService::sceneDescribed,
                this, [this](int sceneId, const QString& description) {
            emit sceneDescribed(sceneId, description);
        });

        connect(m_analysis, &VideoAnalysisService::sceneFused,
                this, [this](int sceneId, const SceneFusion& fusion) {
            if (sceneId >= 0 && sceneId < m_scenes.size()) {
                Scene& scene = m_scenes[sceneId];
                scene.visualDescription = fusion.visualDescription;
                scene.audioSummary = fusion.audioSummary;
                scene.fusedDescription = fusion.fusedDescription;
                scene.description = fusion.fusedDescription;
                scene.audioRelation = fusion.relation;
                scene.audioRelationConfidence = fusion.confidence;
                scene.audioType = fusion.audioType;
            }
            emit sceneFused(sceneId, fusion);
        });

        connect(m_analysis, &VideoAnalysisService::summaryReady,
                this, [this](const QString& summary) {
            m_videoSummary = summary;
            m_isIndexing = false;
            m_indexPercent = 100;
            m_indexStageLabel = tr("分析完成");
            emit progressChanged(100, m_indexStageLabel);
            emit indexingChanged(false);
            emit summaryReady(summary);
        });

        connect(m_analysis, &VideoAnalysisService::analysisProgress,
                this, [this](int percent, const QString& stage) {
            m_indexPercent    = percent;
            m_indexStageLabel = stage;
            emit progressChanged(percent, stage);
        });
    }
}

void VideoAnalysisViewModel::onVideoOpened(const QString& videoPath)
{
    if (m_currentPath == videoPath) return;
    m_currentPath = videoPath;

    m_scenes.clear();
    m_speechSegments.clear();
    m_videoSummary.clear();
    m_repr.reset();
    m_indexPercent   = 0;
    m_indexStageLabel = tr("准备中...");
    m_isIndexing     = true;

    emit scenesReady(m_scenes);
    emit speechSegmentsReady(m_speechSegments);
    emit progressChanged(0, m_indexStageLabel);
    emit indexingChanged(true);

    if (m_indexer) {
        auto repr = m_indexer->representation(videoPath);
        if (repr && repr->level >= VideoRepresentation::Level0) {
            m_repr           = repr;
            m_scenes         = repr->scenes;
            m_speechSegments = repr->speechSegments;
            m_videoSummary   = repr->videoSummary;
            emit scenesReady(m_scenes);
            emit speechSegmentsReady(m_speechSegments);
            if (!m_videoSummary.isEmpty()) emit summaryReady(m_videoSummary);
        }
    }
}

QString VideoAnalysisViewModel::sceneDescription(int sceneId) const
{
    if (!m_repr) return {};
    return m_repr->sceneDescriptions.value(sceneId);
}

SceneFusion VideoAnalysisViewModel::sceneFusion(int sceneId) const
{
    if (!m_repr) return {};
    return m_repr->sceneFusions.value(sceneId);
}
