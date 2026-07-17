#ifndef FRAMEMIND_PLAYERVIEW_H
#define FRAMEMIND_PLAYERVIEW_H

#include <QWidget>
#include <QEvent>
#include <QResizeEvent>
#include <QKeyEvent>
#include <QTimer>


class VideoRenderWidget;
class PlayerControlBar;
class PlayerViewModel;
class RoundedVideoContainer;
class ThemeService;

/**
 * 播放器视图：组合 VideoRenderWidget + PlayerControlBar。
 * 视频容器固定尺寸（720×405，16:9），不随窗口大小变化；外围留白由父容器提供。
 * 支持全屏模式：双击视频区域或点击全屏按钮进入全屏，ESC 退出。
 */
class PlayerView : public QWidget {
    Q_OBJECT
public:
    explicit PlayerView(QWidget* parent = nullptr);

    void setViewModel(PlayerViewModel* vm);
    void setThemeService(ThemeService* theme);

    QSize sizeHint() const override;
    bool isFullscreen() const { return m_isFullscreen; }

signals:
    void requestShowControlBar(bool show);
    void fullscreenChanged(bool fullscreen);

public slots:
    void showControlBar();
    void hideControlBar();
    void toggleFullscreen();
    void exitFullscreen();

private:
    void bindViewModel();
    void enterFullscreen();
    void updateControlBarGeometry();
    bool eventFilter(QObject* obj, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

    RoundedVideoContainer* m_videoContainer = nullptr;
    QWidget*               m_controlContainer = nullptr;
    VideoRenderWidget*     m_renderWidget = nullptr;
    PlayerControlBar*      m_controlBar = nullptr;
    PlayerViewModel*       m_vm = nullptr;
    bool                   m_mouseOver = false;
    bool                   m_controlBarVisible = false;
    bool                   m_isFullscreen = false;
    QTimer                 m_hideTimer;
    int                    m_hideDelayMs = 3000;

    QWidget*               m_fullscreenWindow = nullptr;
};

#endif // FRAMEMIND_PLAYERVIEW_H
