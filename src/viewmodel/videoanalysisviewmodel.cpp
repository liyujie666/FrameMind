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
            m_repr           = repr;
            m_isIndexing     = false;
            m_indexPercent   = 100;
            m_indexStageLabel = tr("索引完成");
            emit progressChanged(100, m_indexStageLabel);
            emit indexingChanged(false);
        });
    }

    if (m_analysis) {
        connect(m_analysis, &VideoAnalysisService::sceneDescribed,
                this, [this](int sceneId, const QString& description) {
            if (m_repr) {
                // 描述已写入 repr->sceneDescriptions，直接转发信号
            }
            emit sceneDescribed(sceneId, description);
        });

        connect(m_analysis, &VideoAnalysisService::summaryReady,
                this, [this](const QString& summary) {
            m_videoSummary = summary;
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

    // 重置状态
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

    // 若索引已经完成（同一视频缓存命中），立即拉取结果
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
