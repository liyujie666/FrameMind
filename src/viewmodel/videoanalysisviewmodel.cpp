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
            if (!repr || (!m_currentPath.isEmpty() && repr->metadata.filePath!=m_currentPath)) return;
            m_repr = repr;
            if (level == 0) {
                m_scenes = repr->scenes;
                emit scenesReady(m_scenes);
            }
            if (level >= 1) {
                m_scenes         = repr->scenes;
                m_speechSegments = repr->speechSegments;
                emit scenesReady(m_scenes);
                emit speechSegmentsReady(m_speechSegments);
            }
        });

        connect(m_indexer, &VideoIndexer::indexError,
                this, [this](VideoIndexer::Stage, const QString& error) {
            m_isIndexing = false;
            emit indexingChanged(false);
            qWarning() << "[VideoAnalysisViewModel] 索引错误:" << error;
        });
    }

    if (m_analysis) {
        connect(m_analysis,&VideoAnalysisService::contentProfileReady,this,[this](const QString& path,const VideoContentProfile& p){
            if(path!=m_currentPath) return;m_profile=p;emit contentProfileReady(p);
        });
        connect(m_analysis,&VideoAnalysisService::semanticUnitsReady,this,[this](const QString& path,const QVector<SemanticUnit>& units){
            if(path!=m_currentPath) return;m_repr=m_analysis->representation(path);emit semanticUnitsReady(units);
        });
        connect(m_analysis,&VideoAnalysisService::buildFinished,this,[this](const VideoBuildManifest& manifest){
            if(manifest.filePath!=m_currentPath) return;m_isIndexing=false;emit indexingChanged(false);emit buildStateChanged(manifest);
        });
        connect(m_analysis, &VideoAnalysisService::analysisProgress,
                this, [this](int percent, const QString& msg) {
            m_indexPercent    = percent;
            m_indexStageLabel = msg;
            emit progressChanged(percent, msg);
        });

        connect(m_analysis, &VideoAnalysisService::sceneDescribed,
                this, [this](int sceneId, const QString& description) {
            emit sceneDescribed(sceneId, description);
        });

        connect(m_analysis, &VideoAnalysisService::summaryReady,
                this, [this](const QString& summary) {
            auto current=m_analysis->representation(m_currentPath);
            if(!current || current->videoSummary!=summary) return;
            m_videoSummary = summary;
            
            // 摘要生成完成，标记索引结束
            m_isIndexing = false;
            qDebug() << "[VideoAnalysisViewModel] 摘要已就绪，发送 indexingChanged(false)";
            emit indexingChanged(false);
            
            emit summaryReady(summary);
        });

        connect(m_analysis, &VideoAnalysisService::sceneFused,
                this, [this](int sceneId, const SceneFusion& fusion) {
            emit sceneFused(sceneId, fusion);
        });
    }
}

void VideoAnalysisViewModel::onVideoOpened(const QString& videoPath)
{
    if (m_currentPath == videoPath) return;
    m_currentPath = videoPath;

    m_scenes.clear();
    m_profile={};emit contentProfileReady(m_profile);emit semanticUnitsReady({});
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
            m_profile=repr->build.profile;emit contentProfileReady(m_profile);emit semanticUnitsReady(repr->semanticUnits);
            m_indexPercent   = 100;
            m_indexStageLabel = tr("已加载持久化视频索引");
            m_isIndexing     = false;
            
            emit scenesReady(m_scenes);
            emit speechSegmentsReady(m_speechSegments);
            
            // 重放场景描述信号，让时间线和总结 UI 显示已缓存的内容
            for (auto it = repr->sceneDescriptions.constBegin();
                 it != repr->sceneDescriptions.constEnd(); ++it) {
                emit sceneDescribed(it.key(), it.value());
            }
            
            if (!m_videoSummary.isEmpty()) emit summaryReady(m_videoSummary);
            emit progressChanged(m_indexPercent, m_indexStageLabel);
            emit indexingChanged(false);
        }
    }
}

void VideoAnalysisViewModel::changeType(VideoContentType type) {
    if(!m_analysis || m_currentPath.isEmpty()) return;m_isIndexing=true;emit indexingChanged(true);m_analysis->changeType(m_currentPath,type);
}
void VideoAnalysisViewModel::rebuild() {if(m_analysis && !m_currentPath.isEmpty()) {m_isIndexing=true;emit indexingChanged(true);m_analysis->analyzeVideo(m_currentPath);}}
void VideoAnalysisViewModel::cancelBuild() {if(m_analysis) m_analysis->cancelBuild();}

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
