#include "view/player/playerview.h"

#include "view/player/videorenderwidget.h"
#include "view/player/playercontrolbar.h"
#include "viewmodel/playerviewmodel.h"

#include <QVBoxLayout>

PlayerView::PlayerView(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_renderWidget = new VideoRenderWidget(this);
    m_controlBar   = new PlayerControlBar(this);

    layout->addWidget(m_renderWidget, 1);
    layout->addWidget(m_controlBar, 0);
}

void PlayerView::setViewModel(PlayerViewModel* vm)
{
    m_vm = vm;
    bindViewModel();
}

void PlayerView::bindViewModel()
{
    if (!m_vm) return;

    // ViewModel → View
    connect(m_vm, &PlayerViewModel::positionChanged,
            m_controlBar, &PlayerControlBar::setPosition);
    connect(m_vm, &PlayerViewModel::durationChanged,
            m_controlBar, &PlayerControlBar::setDuration);
    connect(m_vm, &PlayerViewModel::stateChanged,
            m_controlBar, &PlayerControlBar::setPlayState);
    connect(m_vm, &PlayerViewModel::volumeChanged,
            m_controlBar, &PlayerControlBar::setVolumeDisplay);
    connect(m_vm, &PlayerViewModel::frameReady,
            m_renderWidget, &VideoRenderWidget::updateFrame);

    // View → ViewModel
    connect(m_controlBar, &PlayerControlBar::seekRequested,
            m_vm, &PlayerViewModel::seek);
    connect(m_controlBar, &PlayerControlBar::playClicked,
            m_vm, &PlayerViewModel::togglePlay);
    connect(m_controlBar, &PlayerControlBar::volumeChanged,
            m_vm, &PlayerViewModel::setVolume);
    connect(m_controlBar, &PlayerControlBar::speedChanged,
            m_vm, &PlayerViewModel::setSpeed);
    connect(m_controlBar, &PlayerControlBar::muteClicked, m_vm,
            [this]() { m_vm->setMute(!m_vm->muted()); });

    // 初始化控件显示
    m_controlBar->setVolumeDisplay(m_vm->volume());
}
