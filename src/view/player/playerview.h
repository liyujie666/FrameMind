#ifndef FRAMEMIND_PLAYERVIEW_H
#define FRAMEMIND_PLAYERVIEW_H

#include <QWidget>
#include <QEvent>
#include <QResizeEvent>
#include <QTimer>

class VideoRenderWidget;
class PlayerControlBar;
class PlayerViewModel;
class RoundedVideoContainer;
class ThemeService;

/**
 * 播放器视图：组合 VideoRenderWidget + PlayerControlBar。
 * 视频容器固定尺寸（720×405，16:9），不随窗口大小变化；外围留白由父容器提供。
 */
class PlayerView : public QWidget {
    Q_OBJECT
public:
    explicit PlayerView(QWidget* parent = nullptr);

    void setViewModel(PlayerViewModel* vm);
    void setThemeService(ThemeService* theme);   // 预留：主题变更时可调整外壳配色

    QSize sizeHint() const override;

signals:
    void requestShowControlBar(bool show);

public slots:
    void showControlBar();
    void hideControlBar();

private:
    void bindViewModel();
    void layoutChildren();
    bool eventFilter(QObject* obj, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

    RoundedVideoContainer* m_videoContainer = nullptr;
    QWidget*               m_controlContainer = nullptr;
    VideoRenderWidget*     m_renderWidget = nullptr;
    PlayerControlBar*      m_controlBar = nullptr;
    PlayerViewModel*       m_vm = nullptr;
    bool                   m_mouseOver = false;
    bool                   m_controlBarVisible = false;
    QTimer                 m_hideTimer;
    int                    m_hideDelayMs = 3000;
};

#endif // FRAMEMIND_PLAYERVIEW_H
