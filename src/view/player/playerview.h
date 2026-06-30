#ifndef FRAMEMIND_PLAYERVIEW_H
#define FRAMEMIND_PLAYERVIEW_H

#include <QWidget>

class VideoRenderWidget;
class PlayerControlBar;
class PlayerViewModel;

/**
 * 播放器视图：组合 VideoRenderWidget + PlayerControlBar，
 * 通过 setViewModel 完成与 PlayerViewModel 的双向绑定。
 */
class PlayerView : public QWidget {
    Q_OBJECT
public:
    explicit PlayerView(QWidget* parent = nullptr);

    void setViewModel(PlayerViewModel* vm);

private:
    void bindViewModel();

    VideoRenderWidget* m_renderWidget = nullptr;
    PlayerControlBar*  m_controlBar = nullptr;
    PlayerViewModel*   m_vm = nullptr;
};

#endif // FRAMEMIND_PLAYERVIEW_H
