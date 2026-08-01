#ifndef FRAMEMIND_VIDEOANALYSISVIEWMODEL_H
#define FRAMEMIND_VIDEOANALYSISVIEWMODEL_H

#include <QObject>
#include <QVector>
#include <QString>
#include <QSharedPointer>

#include "model/scene.h"
#include "model/speech_segment.h"
#include "model/video_representation.h"
#include "model/audio_visual_relation.h"

class VideoAnalysisService;
class VideoIndexer;

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

    /// 获取场景的 VLM 描述（若已生成），否则返回空
    QString sceneDescription(int sceneId) const;
    SceneFusion sceneFusion(int sceneId) const;

public slots:
    /// 视频打开时由外部（MainWindow/ChatViewModel）驱动
    void onVideoOpened(const QString& videoPath);

signals:
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

    VideoAnalysisService*              m_analysis = nullptr;
    VideoIndexer*                      m_indexer  = nullptr;
    QString                            m_currentPath;

    QVector<Scene>                     m_scenes;
    QVector<SpeechSegment>             m_speechSegments;
    QString                            m_videoSummary;
    QSharedPointer<VideoRepresentation> m_repr;

    int     m_indexPercent   = 0;
    QString m_indexStageLabel;
    bool    m_isIndexing     = false;
};

#endif // FRAMEMIND_VIDEOANALYSISVIEWMODEL_H
