#ifndef FRAMEMIND_VIDEOANALYSISVIEWMODEL_H
#define FRAMEMIND_VIDEOANALYSISVIEWMODEL_H

#include <QObject>
#include <QVector>
#include <QString>
#include <QSharedPointer>
#include <optional>

#include "model/scene.h"
#include "model/speech_segment.h"
#include "model/video_representation.h"
#include "model/audio_visual_relation.h"

class VideoAnalysisService;
class VideoIndexer;
class QTimer;

/**
 * 视频分析视图模型。
 *
 * 桥接 VideoAnalysisService / VideoIndexer 与三个分析 tab（时间线/总结/字幕）。
 * 持有当前视频的 scenes、speechSegments、videoSummary 副本，
 * 随索引进度逐步更新，通过信号通知各 tab widget 刷新。
 */
class VideoAnalysisViewModel : public QObject {
    Q_OBJECT

public:
    explicit VideoAnalysisViewModel(VideoAnalysisService* analysisService,
                                    VideoIndexer*         indexer,
                                    QObject*              parent = nullptr);

    // ---- 数据访问 ----
    QVector<Scene>         scenes()          const { return m_scenes; }
    QVector<SpeechSegment> speechSegments()  const { return m_speechSegments; }
    QString                videoSummary()    const { return m_videoSummary; }
    int                    indexPercent()    const { return m_indexPercent; }
    QString                indexStageLabel() const { return m_indexStageLabel; }
    bool                   isIndexing()      const { return m_isIndexing; }
    QString runningBuildId() const { return m_runningBuild ? m_runningBuild->buildId : QString{}; }
    QString displayedBuildId() const { return m_repr ? m_repr->build.buildId : QString{}; }
    QString displayedVideoId() const { return m_repr ? m_repr->videoId : QString{}; }
    QString currentVideoPath() const { return m_currentPath; }
    VideoBuildManifest buildState() const { return m_buildState; }
    qint64 buildElapsedMs() const;
    bool refreshActiveRepresentation();
    bool isPreview() const { return m_repr && m_repr->build.artifacts["display_preview"].toBool(); }
    bool isDisplayedIndexReady() const {
        return m_repr && !isPreview() && !displayedBuildId().isEmpty() &&
            (m_repr->build.state == ArtifactState::Ready || m_repr->build.state == ArtifactState::Partial);
    }
    bool isShowingPreviousContent() const { return !displayedBuildId().isEmpty() && !m_overviewTargetBuildId.isEmpty() && displayedBuildId() != m_overviewTargetBuildId; }
    VideoBuildManifest displayedBuild() const { return m_repr ? m_repr->build : VideoBuildManifest{}; }
    VideoPresentation presentation() const { return m_repr ? m_repr->build.presentation : VideoPresentation{}; }
    QVector<VideoChapter> chapters() const { return presentation().chapters; }
    bool needsContentRebuild() const { return !m_repr || m_repr->build.artifacts["content_rebuild_required"].toBool() || !m_repr->build.presentation.hasCompletedSections(); }
    bool isShowingPreviousOverview() const {
        return !m_videoSummary.trimmed().isEmpty() && !m_overviewTargetBuildId.isEmpty() &&
            displayedBuildId() != m_overviewTargetBuildId;
    }
    QVector<SemanticUnit> semanticUnits() const {return m_repr?m_repr->semanticUnits:QVector<SemanticUnit>{};}
    VideoContentProfile contentProfile() const {return m_profile;}
    VideoContentProfile displayedContentProfile() const {return m_repr ? m_repr->build.profile : m_profile;}
    void changeType(VideoContentType type);
    void rebuild(bool automatic=false);
    void cancelBuild();

    /// 获取场景的 VLM 描述（若已生成），否则返回空
    QString sceneDescription(int sceneId) const;
    SceneFusion sceneFusion(int sceneId) const;

public slots:
    /// 视频打开时由外部（MainWindow/ChatViewModel）驱动
    void onVideoOpened(const QString& videoPath);

signals:
    void presentationReady(const QString& videoId, const QString& buildId, const VideoPresentation&);
    void semanticUnitsReady(const QVector<SemanticUnit>&);
    void contentProfileReady(const VideoContentProfile&);
    void buildStateChanged(const VideoBuildManifest&);
    /// 场景列表已更新（Level 0 完成）
    void scenesReady(const QVector<Scene>& scenes);

    /// 某个场景的 VLM 描述就绪
    void sceneDescribed(int sceneId, const QString& description);

    /// 某个场景的音视频融合证据就绪
    void sceneFused(int sceneId, const SceneFusion& fusion);

    /// 语音转写段已更新（Level 1 完成）
    void speechSegmentsReady(const QVector<SpeechSegment>& segments);

    /// 全视频摘要就绪（Level 2 完成）
    void summaryReady(const QString& summary);

    /// 索引进度（0~100）
    void progressChanged(int percent, const QString& stageLabel);

    /// 索引状态变化
    void indexingChanged(bool isIndexing);

private:
    void connectServices();
    bool matchesRun(const VideoBuildContext &) const;
    void startRun(const VideoBuildContext &);
    void applyRepresentation(const VideoRepresentation &);
    void queuePreview(const VideoBuildContext&, const VideoRepresentation&);
    void flushPreview();

    VideoAnalysisService*              m_analysis = nullptr;
    VideoIndexer*                      m_indexer  = nullptr;
    QString                            m_currentPath;
    QString m_overviewTargetBuildId;
    std::optional<VideoBuildContext> m_runningBuild;
    quint64 m_lastGeneration = 0;
    VideoContentProfile m_profile;
    VideoBuildManifest m_buildState;

    QVector<Scene>                     m_scenes;
    QVector<SpeechSegment>             m_speechSegments;
    QString                            m_videoSummary;
    QSharedPointer<VideoRepresentation> m_repr;
    QSharedPointer<VideoRepresentation> m_activeRepresentation, m_pendingPreview;
    std::optional<VideoBuildContext> m_previewContext;
    QTimer* m_previewTimer = nullptr;

    int     m_indexPercent   = 0;
    QString m_indexStageLabel;
    bool    m_isIndexing     = false;
};

#endif // FRAMEMIND_VIDEOANALYSISVIEWMODEL_H
