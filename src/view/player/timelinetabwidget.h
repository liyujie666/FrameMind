#ifndef FRAMEMIND_TIMELINETABWIDGET_H
#define FRAMEMIND_TIMELINETABWIDGET_H

#include <QWidget>
#include <QVector>

#include "model/scene.h"

class ThemeService;
class VideoAnalysisViewModel;
class QScrollArea;
class QVBoxLayout;
class QLabel;

/**
 * 时间线 Tab：以卡片列表展示各场景。
 *
 * 每个场景卡片显示：
 *   - 时间区间（"00:01:23 - 00:02:45"格式）
 *   - 场景缩略标题 / VLM 描述摘要（若已就绪）
 *   - 进度条标注该场景在整个视频中的相对位置
 *
 * 点击任意卡片发出 seekRequested(posMs) 信号，由外部连接到 PlayerViewModel::seek。
 * 当前播放位置对应的场景卡片会高亮显示。
 */
class TimelineTabWidget : public QWidget {
    Q_OBJECT
public:
    explicit TimelineTabWidget(QWidget* parent = nullptr);

    void setThemeService(ThemeService* theme);
    void setViewModel(VideoAnalysisViewModel* vm);

public slots:
    /// 由 PlayerViewModel::positionChanged 驱动，高亮当前场景
    void onPositionChanged(int64_t posMs);

signals:
    void seekRequested(int64_t posMs);

private slots:
    void onScenesReady(const QVector<Scene>& scenes);
    void onSceneDescribed(int sceneId, const QString& description);
    void onThemeChanged();

private:
    void buildCards();
    void clearCards();
    void updateHighlight(int64_t posMs);
    void applyScrollStyle();
    QWidget* makeSceneCard(const Scene& scene, int64_t totalDurationMs);

    static QString formatMs(int64_t ms);

    ThemeService*           m_theme      = nullptr;
    VideoAnalysisViewModel* m_vm         = nullptr;
    QScrollArea*            m_scroll     = nullptr;
    QWidget*                m_container  = nullptr;
    QVBoxLayout*            m_cardLayout = nullptr;

    QVector<Scene>   m_scenes;
    int64_t          m_totalDurationMs = 0;
    int64_t          m_currentPosMs    = 0;

    // 每张卡片的 widget 指针（按 sceneId 索引以支持描述更新）
    QVector<QWidget*>  m_cards;
    // 每张卡片的描述 label（用于动态更新 VLM 描述）
    QVector<QLabel*>   m_descLabels;
};

#endif // FRAMEMIND_TIMELINETABWIDGET_H
