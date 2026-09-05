#ifndef FRAMEMIND_TIMELINETABWIDGET_H
#define FRAMEMIND_TIMELINETABWIDGET_H

#include <QWidget>
#include <QVector>

#include "model/scene.h"
#include "model/audio_visual_relation.h"

class ThemeService;
class VideoAnalysisViewModel;
class QScrollArea;
class QVBoxLayout;
class QLabel;
class QEvent;
class QTimer;

class TimelineTabWidget : public QWidget {
    Q_OBJECT
public:
    explicit TimelineTabWidget(QWidget* parent = nullptr);

    void setThemeService(ThemeService* theme);
    void setViewModel(VideoAnalysisViewModel* vm);

public slots:
    void onPositionChanged(int64_t posMs);

signals:
    void seekRequested(int64_t posMs);

private slots:
    void onScenesReady(const QVector<Scene>& scenes);
    void onSceneDescribed(int sceneId, const QString& description);
    void onSceneFused(int sceneId, const SceneFusion& fusion);
    void onThemeChanged();

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    void buildCards();
    void clearCards();
    void updateHighlight(int64_t posMs);
    void applyScrollStyle();
    QWidget* makeSceneCard(const Scene& scene, int64_t totalDurationMs);
    static QString formatMs(int64_t ms);

    ThemeService* m_theme = nullptr;
    VideoAnalysisViewModel* m_vm = nullptr;
    QScrollArea* m_scroll = nullptr;
    QWidget* m_container = nullptr;
    QVBoxLayout* m_cardLayout = nullptr;
    QVector<Scene> m_scenes;
    int64_t m_totalDurationMs = 0;
    int64_t m_currentPosMs = 0;
    QVector<QWidget*> m_cards;
    QVector<QLabel*> m_descLabels;
    bool m_userScrolling = false;
    QTimer* m_scrollResetTimer = nullptr;
};

#endif // FRAMEMIND_TIMELINETABWIDGET_H
