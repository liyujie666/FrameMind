#ifndef FRAMEMIND_TIMELINETABWIDGET_H
#define FRAMEMIND_TIMELINETABWIDGET_H

#include <QWidget>
#include <QVector>

#include "model/scene.h"
#include "model/audio_visual_relation.h"
#include "model/video_presentation_types.h"
#include <QHash>
#include <QPointer>

class ThemeService;
class VideoAnalysisViewModel;
class QScrollArea;
class QVBoxLayout;
class QLabel;
class QEvent;
class QTimer;
class QComboBox;

class TimelineTabWidget : public QWidget {
    Q_OBJECT
public:
    explicit TimelineTabWidget(QWidget* parent = nullptr);

    void setThemeService(ThemeService* theme);
    void setViewModel(VideoAnalysisViewModel* vm);
    void showInteractionMessage(const QString&);

public slots:
    void onPositionChanged(int64_t posMs);

signals:
    void seekRequested(const QString& videoId, int64_t posMs);
    void chapterRequested(const QString& videoId, const QString& buildId, const QString& chapterId);

private slots:
    void onScenesReady(const QVector<Scene>& scenes);
    void onSceneDescribed(int sceneId, const QString& description);
    void onSceneFused(int sceneId, const SceneFusion& fusion);
    void onThemeChanged();

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    void buildCards();
    void refreshTimeline();
    void clearCards();
    void updateHighlight(int64_t posMs, bool followPlayback = true);
    void applyScrollStyle();
    QWidget* makeChapterCard(const VideoChapter&, int ordinal);
    QWidget* makeSceneCard(const Scene& scene, int64_t totalDurationMs);
    static QString formatMs(int64_t ms);

    ThemeService* m_theme = nullptr;
    QPointer<VideoAnalysisViewModel> m_vm;
    QScrollArea* m_scroll = nullptr;
    QWidget* m_container = nullptr;
    QVBoxLayout* m_cardLayout = nullptr;
    QVector<Scene> m_scenes;
    QVector<Scene> m_shots;
    QVector<VideoChapter> m_chapters;
    QVector<QPair<qint64, qint64>> m_cardRanges;
    QString m_videoId, m_buildId;
    QComboBox* m_mode=nullptr;
    QLabel* m_timelineMeta = nullptr;
    QLabel* m_feedback = nullptr;
    int64_t m_totalDurationMs = 0;
    int64_t m_currentPosMs = 0;
    QVector<QWidget*> m_cards;
    QHash<int, QLabel*> m_descLabels;
    quint64 m_themeRevision = 0, m_refreshRevision = 0;
    bool m_userScrolling = false;
    QTimer* m_scrollResetTimer = nullptr;
};

#endif // FRAMEMIND_TIMELINETABWIDGET_H
