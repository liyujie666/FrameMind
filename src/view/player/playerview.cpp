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
#include <QMouseEvent>
#include <QPropertyAnimation>
#include <QParallelAnimationGroup>
#include <QGraphicsOpacityEffect>
#include <QApplication>
#include <QScreen>
#include <QtMath>

namespace {
constexpr int kOuterMargin = 0;
}

PlayerView::PlayerView(QWidget* parent)
    : QWidget(parent)
    , m_mouseOver(false)
    , m_controlBarVisible(false)
    , m_isFullscreen(false)
{
    setAttribute(Qt::WA_StyledBackground, false);
    setAutoFillBackground(false);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    // 允许接收键盘事件（全屏 ESC 退出）
    setFocusPolicy(Qt::StrongFocus);

    // 圆角视频容器
    m_videoContainer = new RoundedVideoContainer(this);
    m_videoContainer->setRadius(12);

    m_renderWidget = new VideoRenderWidget(m_videoContainer);
    m_renderWidget->setContainerBgColor(Qt::black);

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

    // 初始化透明度效果
    auto* opacity = new QGraphicsOpacityEffect(controlContainer);
    controlContainer->setGraphicsEffect(opacity);
    opacity->setOpacity(0);

    // 保证控制栏浮在最上层
    m_videoContainer->stackUnder(controlContainer);

    // 隐藏定时器
    m_hideTimer.setSingleShot(true);
    m_hideTimer.setInterval(m_hideDelayMs);
    connect(&m_hideTimer, &QTimer::timeout, this, &PlayerView::hideControlBar);

    // 安装事件过滤器以追踪鼠标进入/离开
    m_videoContainer->installEventFilter(this);
    controlContainer->installEventFilter(this);

    // 连接全屏按钮信号
    connect(m_controlBar, &PlayerControlBar::fullscreenClicked,
            this, &PlayerView::toggleFullscreen);

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

    constexpr double kVideoAspect = 16.0 / 9.0;
    const int availW = width();
    const int availH = height();

    int videoW, videoH;
    if (availW * 1.0 / availH > kVideoAspect) {
        videoH = availH;
        videoW = qRound(videoH * kVideoAspect);
    } else {
        videoW = availW;
        videoH = qRound(videoW / kVideoAspect);
    }

    const int cx = (width() - videoW) / 2;
    const int cy = (height() - videoH) / 2;
    m_videoContainer->setFixedSize(videoW, videoH);
    m_videoContainer->move(cx, cy);

    // 全屏时去除圆角
    if (m_isFullscreen) {
        m_videoContainer->setRadius(0);
    } else {
        m_videoContainer->setRadius(12);
    }

    // 控制栏位置：视频底部内侧
    if (m_controlContainer) {
        const int controlWidth  = m_isFullscreen ? qMin(videoW * 4 / 5, 900) : videoW * 4 / 5;
        const int controlHeight = 40;
        const int controlX = cx + (videoW - controlWidth) / 2;
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
        // 返回 false，不吞噬事件，让子控件也能收到 hover 效果
        return false;
    } else if (event->type() == QEvent::Leave) {
        m_mouseOver = false;
        m_hideTimer.start();
        return false;
    }
    return QWidget::eventFilter(obj, event);
}

void PlayerView::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape && m_isFullscreen) {
        exitFullscreen();
        event->accept();
        return;
    }
    // F11 也可切换全屏
    if (event->key() == Qt::Key_F11) {
        toggleFullscreen();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void PlayerView::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        toggleFullscreen();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void PlayerView::toggleFullscreen()
{
    if (m_isFullscreen) {
        exitFullscreen();
    } else {
        enterFullscreen();
    }
}

void PlayerView::enterFullscreen()
{
    if (m_isFullscreen) return;

    // 保存当前状态
    m_originalParent = parentWidget();
    m_originalPos = pos();
    m_originalSize = size();
    m_originalFlags = windowFlags();

    m_isFullscreen = true;

    // 脱离父窗口，变为独立顶层窗口
    setParent(nullptr);
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    showFullScreen();

    // 确保焦点在此控件上以接收键盘事件
    setFocus();
    activateWindow();

    // 更新控制栏全屏状态
    m_controlBar->setFullscreen(true);
    layoutChildren();

    emit fullscreenChanged(true);
}

void PlayerView::exitFullscreen()
{
    if (!m_isFullscreen) return;

    m_isFullscreen = false;

    // 恢复为子窗口
    setWindowFlags(m_originalFlags);
    if (m_originalParent) {
        setParent(m_originalParent);
        move(m_originalPos);
        resize(m_originalSize);
        show();

        // 重新将自己放入父布局（如果布局还存在）
        if (m_originalParent->layout()) {
            // 不需要手动 addWidget，setParent 后 show 即可（布局自动管理）
        }
    } else {
        show();
    }

    // 更新控制栏全屏状态
    m_controlBar->setFullscreen(false);
    layoutChildren();

    emit fullscreenChanged(false);
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
