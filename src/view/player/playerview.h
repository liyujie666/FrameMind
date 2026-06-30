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

/**
 * 播放器视图：组合 VideoRenderWidget + PlayerControlBar，
 * 通过 setViewModel 完成与 PlayerViewModel 的双向绑定。
 */
class PlayerView : public QWidget {
    Q_OBJECT
public:
    explicit PlayerView(QWidget* parent = nullptr);

    void setViewModel(PlayerViewModel* vm);

signals:
    void requestShowControlBar(bool show);

public slots:
    void showControlBar();
    void hideControlBar();

private:
    void bindViewModel();
    void updateVideoAspectRatio();
    bool eventFilter(QObject* obj, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

    RoundedVideoContainer* m_videoContainer = nullptr;
    QWidget*               m_controlContainer = nullptr;
    VideoRenderWidget* m_renderWidget = nullptr;
    PlayerControlBar*  m_controlBar = nullptr;
    PlayerViewModel*   m_vm = nullptr;
    bool               m_mouseOver = false;
    bool               m_controlBarVisible = false;
    QTimer             m_hideTimer;
    int                m_hideDelayMs = 3000;
};

#endif // FRAMEMIND_PLAYERVIEW_H
