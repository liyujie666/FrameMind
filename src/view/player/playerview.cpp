#include "view/player/playerview.h"

#include "view/player/videorenderwidget.h"
#include "view/player/playercontrolbar.h"
#include "viewmodel/playerviewmodel.h"
#include "service/themeservice.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QStackedLayout>
#include <QEvent>
#include <QResizeEvent>
#include <QPropertyAnimation>
#include <QParallelAnimationGroup>
#include <QGraphicsOpacityEffect>
#include <QtMath>

namespace {
// 视频与外层容器间距由 playerPanelLayout->setContentsMargins 控制，此处为 0
constexpr int kOuterMargin = 0;
}

PlayerView::PlayerView(QWidget* parent)
    : QWidget(parent)
    , m_mouseOver(false)
    , m_controlBarVisible(false)
{
    // 顶层透明：底色由外层 ThemedPanel / 页面容器提供
    setAttribute(Qt::WA_StyledBackground, false);
    setAutoFillBackground(false);

    // 视频容器自适应外层尺寸，保持 16:9
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    // 圆角视频容器（尺寸由 layoutChildren 动态计算）
    m_videoContainer = new RoundedVideoContainer(this);
    m_videoContainer->setRadius(12);

    m_renderWidget = new VideoRenderWidget(m_videoContainer);
    m_renderWidget->setContainerBgColor(Qt::black);

    // 视频容器内部布局（占满容器）
    auto* videoLayout = new QVBoxLayout(m_videoContainer);
    videoLayout->setContentsMargins(0, 0, 0, 0);
    videoLayout->setSpacing(0);
    videoLayout->addWidget(m_renderWidget, 1);

    // 控制栏容器（独立透明浮层，浮在视频上方）
    auto* controlContainer = new QWidget(this);
    controlContainer->setStyleSheet(
        "background-color: rgba(20, 20, 20, 180); border-radius: 20px;");
    m_controlContainer = controlContainer;

    m_controlBar = new PlayerControlBar(controlContainer);
    auto* controlLayout = new QHBoxLayout(controlContainer);
    controlLayout->setContentsMargins(0, 0, 0, 0);
    controlLayout->setSpacing(0);
    controlLayout->addWidget(m_controlBar);

    // 初始化透明度效果（控制栏始终保持在正确位置，初始隐藏）
    auto* opacity = new QGraphicsOpacityEffect(controlContainer);
    controlContainer->setGraphicsEffect(opacity);
    opacity->setOpacity(0);

    // 保证控制栏浮在最上层
    m_videoContainer->stackUnder(controlContainer);

    // 隐藏定时器：鼠标离开 3s 后隐藏控制栏
    m_hideTimer.setSingleShot(true);
    m_hideTimer.setInterval(m_hideDelayMs);
    connect(&m_hideTimer, &QTimer::timeout, this, &PlayerView::hideControlBar);

    m_videoContainer->installEventFilter(this);
    controlContainer->installEventFilter(this);

    // 初始位置摆放
    layoutChildren();
}

QSize PlayerView::sizeHint() const
{
    constexpr double kAspect = 16.0 / 9.0;
    constexpr int kBaseW = 720;
    return QSize(kBaseW, qRound(kBaseW / kAspect));
}

void PlayerView::setThemeService(ThemeService* theme)
{
    // 视频渲染背景保持黑色（无论亮暗主题）；预留接口。
    if (m_controlBar) {
        m_controlBar->setThemeService(theme);
    }
}

void PlayerView::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    layoutChildren();
}

void PlayerView::layoutChildren()
{
    if (!m_videoContainer) return;

    // 根据 PlayerView 当前尺寸计算 16:9 视频区域
    // 外层间距由 playerPanelLayout->setContentsMargins(16,16,16,16) 控制
    constexpr double kVideoAspect = 16.0 / 9.0;
    const int availW = width();
    const int availH = height();

    int videoW, videoH;
    if (availW * 1.0 / availH > kVideoAspect) {
        // 高度受限
        videoH = availH;
        videoW = qRound(videoH * kVideoAspect);
    } else {
        // 宽度受限
        videoW = availW;
        videoH = qRound(videoW / kVideoAspect);
    }

    const int cx = (width() - videoW) / 2;
    const int cy = (height() - videoH) / 2;
    m_videoContainer->setFixedSize(videoW, videoH);
    m_videoContainer->move(cx, cy);

    // 控制栏位置：视频底部内侧，宽度 = 视频宽 * 4/5，居中，距底 8px
    // 注意：控制栏始终保持在视频内，通过透明度控制可见性
    if (m_controlContainer) {
        const int controlWidth  = videoW * 4 / 5;
        const int controlHeight = 40;
        const int controlX = cx + (videoW - controlWidth) / 2;
        // 控制栏始终保持在视频底部内侧
        const int controlY = cy + videoH - controlHeight - 8;
        m_controlContainer->setFixedSize(controlWidth, controlHeight);
        m_controlContainer->move(controlX, controlY);
        m_controlBar->setFixedSize(controlWidth, controlHeight);
        m_controlContainer->raise();
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
    if (m_controlBarVisible) return;
    m_controlBarVisible = true;

    auto* opacity = qobject_cast<QGraphicsOpacityEffect*>(
        m_controlContainer->graphicsEffect());
    if (!opacity) {
        opacity = new QGraphicsOpacityEffect(m_controlContainer);
        m_controlContainer->setGraphicsEffect(opacity);
    }

    auto* fadeAnim = new QPropertyAnimation(opacity, "opacity", this);
    fadeAnim->setDuration(200);
    fadeAnim->setStartValue(0.0);
    fadeAnim->setEndValue(1.0);
    fadeAnim->setEasingCurve(QEasingCurve::OutCubic);
    fadeAnim->start(QAbstractAnimation::DeleteWhenStopped);
}

void PlayerView::hideControlBar()
{
    if (!m_controlContainer || !m_videoContainer) return;
    if (!m_controlBarVisible) return;
    m_controlBarVisible = false;

    auto* opacity = qobject_cast<QGraphicsOpacityEffect*>(
        m_controlContainer->graphicsEffect());
    if (!opacity) {
        opacity = new QGraphicsOpacityEffect(m_controlContainer);
        m_controlContainer->setGraphicsEffect(opacity);
    }

    auto* fadeAnim = new QPropertyAnimation(opacity, "opacity", this);
    fadeAnim->setDuration(200);
    fadeAnim->setStartValue(1.0);
    fadeAnim->setEndValue(0.0);
    fadeAnim->setEasingCurve(QEasingCurve::InCubic);
    fadeAnim->start(QAbstractAnimation::DeleteWhenStopped);
}

void PlayerView::setViewModel(PlayerViewModel* vm)
{
    m_vm = vm;
    bindViewModel();
}

void PlayerView::bindViewModel()
{
    if (!m_vm) return;

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

    // 连接静音状态变化以更新图标
    connect(m_vm, &PlayerViewModel::mutedChanged,
            m_controlBar, &PlayerControlBar::setMuted);

    m_controlBar->setVolumeDisplay(m_vm->volume());
}
