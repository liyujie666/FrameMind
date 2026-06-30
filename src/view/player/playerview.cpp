#include "view/player/playerview.h"

#include "view/player/videorenderwidget.h"
#include "view/player/playercontrolbar.h"
#include "viewmodel/playerviewmodel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QStackedLayout>
#include <QEvent>
#include <QResizeEvent>
#include <QPropertyAnimation>
#include <QParallelAnimationGroup>
#include <QGraphicsOpacityEffect>

PlayerView::PlayerView(QWidget* parent)
    : QWidget(parent)
    , m_mouseOver(false)
    , m_controlBarVisible(false)
{
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, QColor("#0D1117"));
    setPalette(pal);

    // 使用绝对定位层叠布局
    setLayout(new QStackedLayout(this));

    // 圆角视频容器
    m_videoContainer = new RoundedVideoContainer(this);
    m_videoContainer->setRadius(12);
    m_videoContainer->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_videoContainer->lower();  // 放到最层

    m_renderWidget = new VideoRenderWidget(m_videoContainer);
    m_renderWidget->setContainerBgColor(Qt::black);

    // 控制栏容器（独立透明容器，浮在视频上方）
    auto* controlContainer = new QWidget(this);
    controlContainer->setStyleSheet("background-color: rgba(20, 20, 20, 180); border-radius: 20px;");
    controlContainer->raise();  // 放到最上层
    m_controlContainer = controlContainer;

    m_controlBar = new PlayerControlBar(controlContainer);

    auto* controlLayout = new QHBoxLayout(controlContainer);
    controlLayout->setContentsMargins(0, 0, 0, 0);
    controlLayout->setSpacing(0);
    controlLayout->addWidget(m_controlBar);
    controlContainer->setLayout(controlLayout);

    // 隐藏定时器：鼠标离开3秒后隐藏控制栏
    m_hideTimer.setSingleShot(true);
    m_hideTimer.setInterval(m_hideDelayMs);
    connect(&m_hideTimer, &QTimer::timeout, this, &PlayerView::hideControlBar);

    // 视频容器内部布局
    auto* videoLayout = new QVBoxLayout(m_videoContainer);
    videoLayout->setContentsMargins(0, 0, 0, 0);
    videoLayout->setSpacing(0);
    videoLayout->addWidget(m_renderWidget, 1);

    // 初始设置 16:9 比例
    updateVideoAspectRatio();

    m_videoContainer->installEventFilter(this);
    controlContainer->installEventFilter(this);

    // 初始化控制栏位置
    updateVideoAspectRatio();
}

void PlayerView::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    updateVideoAspectRatio();
}

void PlayerView::updateVideoAspectRatio()
{
    const int availableWidth = width() - 24;
    const int videoWidth = availableWidth;
    const int videoHeight = videoWidth * 9 / 16;

    m_videoContainer->setFixedSize(videoWidth, videoHeight);
    m_videoContainer->move(12, 12);

    // 控制栏：宽度为视频的 4/5，居中显示，距底部 12px
    if (m_controlContainer) {
        const int controlWidth = videoWidth * 4 / 5;
        const int controlHeight = 40;
        const int controlX = 12 + (videoWidth - controlWidth) / 2;
        const int controlY = 12 + videoHeight - controlHeight - 8;
        m_controlContainer->setFixedSize(controlWidth, controlHeight);
        m_controlContainer->move(controlX, controlY);
        m_controlBar->setFixedSize(controlWidth, controlHeight);
    }
}

bool PlayerView::eventFilter(QObject* obj, QEvent* event)
{
    if (event->type() == QEvent::Enter) {
        m_mouseOver = true;
        m_hideTimer.stop();
        showControlBar();
        return true;
    } else if (event->type() == QEvent::Leave) {
        m_mouseOver = false;
        m_hideTimer.start();
        return true;
    }
    return QWidget::eventFilter(obj, event);
}

void PlayerView::showControlBar()
{
    if (!m_controlContainer || !m_videoContainer) return;

    // 如果已经显示，不再重复动画
    if (m_controlBarVisible) return;
    m_controlBarVisible = true;

    // 目标位置（控制栏最终位置：视频容器底部内侧）
    const int targetX = 12 + (m_videoContainer->width() - m_controlContainer->width()) / 2;
    const int targetY = 12 + m_videoContainer->height() - m_controlContainer->height() - 8;
    const int startY = 12 + m_videoContainer->height();  // 从视频容器底部外开始

    QPropertyAnimation* posAnim = new QPropertyAnimation(m_controlContainer, "pos", this);
    posAnim->setDuration(250);
    posAnim->setStartValue(QPoint(m_controlContainer->x(), startY));
    posAnim->setEndValue(QPoint(targetX, targetY));
    posAnim->setEasingCurve(QEasingCurve::OutCubic);

    // 透明度动画
    QGraphicsOpacityEffect* opacity = new QGraphicsOpacityEffect(m_controlContainer);
    m_controlContainer->setGraphicsEffect(opacity);
    opacity->setOpacity(0);

    QPropertyAnimation* fadeAnim = new QPropertyAnimation(opacity, "opacity", this);
    fadeAnim->setDuration(250);
    fadeAnim->setStartValue(0.0);
    fadeAnim->setEndValue(1.0);
    fadeAnim->setEasingCurve(QEasingCurve::OutCubic);

    QParallelAnimationGroup* group = new QParallelAnimationGroup(this);
    group->addAnimation(posAnim);
    group->addAnimation(fadeAnim);
    group->start(QAbstractAnimation::DeleteWhenStopped);
}

void PlayerView::hideControlBar()
{
    if (!m_controlContainer || !m_videoContainer) return;

    // 如果已经隐藏，不再重复动画
    if (!m_controlBarVisible) return;
    m_controlBarVisible = false;

    // 隐藏到视频容器底部外
    const int endY = 12 + m_videoContainer->height();

    QPropertyAnimation* posAnim = new QPropertyAnimation(m_controlContainer, "pos", this);
    posAnim->setDuration(250);
    posAnim->setStartValue(m_controlContainer->pos());
    posAnim->setEndValue(QPoint(m_controlContainer->x(), endY));
    posAnim->setEasingCurve(QEasingCurve::InCubic);

    // 透明度动画
    QGraphicsOpacityEffect* opacity = qobject_cast<QGraphicsOpacityEffect*>(m_controlContainer->graphicsEffect());
    if (!opacity) {
        opacity = new QGraphicsOpacityEffect(m_controlContainer);
        m_controlContainer->setGraphicsEffect(opacity);
    }

    QPropertyAnimation* fadeAnim = new QPropertyAnimation(opacity, "opacity", this);
    fadeAnim->setDuration(250);
    fadeAnim->setStartValue(1.0);
    fadeAnim->setEndValue(0.0);
    fadeAnim->setEasingCurve(QEasingCurve::InCubic);

    QParallelAnimationGroup* group = new QParallelAnimationGroup(this);
    group->addAnimation(posAnim);
    group->addAnimation(fadeAnim);
    group->start(QAbstractAnimation::DeleteWhenStopped);
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
