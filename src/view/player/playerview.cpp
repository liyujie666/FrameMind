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

namespace {
// 固定视频容器尺寸（16:9），不随窗口大小变化
constexpr int kVideoWidth  = 720;
constexpr int kVideoHeight = 405;   // 720 * 9 / 16
constexpr int kOuterMargin = 12;    // 视频容器四周留白
}

PlayerView::PlayerView(QWidget* parent)
    : QWidget(parent)
    , m_mouseOver(false)
    , m_controlBarVisible(false)
{
    // 顶层透明：底色由外层 ThemedPanel / 页面容器提供
    setAttribute(Qt::WA_StyledBackground, false);
    setAutoFillBackground(false);

    // 视频容器固定尺寸 → PlayerView 也给出等价 sizeHint（含外边距）
    setMinimumSize(kVideoWidth + 2 * kOuterMargin,
                   kVideoHeight + 2 * kOuterMargin);

    // 使用绝对定位（不用 layout，方便让视频容器居中同时控制栏浮在其上）
    // 通过 resizeEvent 手动更新位置

    // 圆角视频容器（固定尺寸）
    m_videoContainer = new RoundedVideoContainer(this);
    m_videoContainer->setRadius(12);
    m_videoContainer->setFixedSize(kVideoWidth, kVideoHeight);

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
    return QSize(kVideoWidth + 2 * kOuterMargin,
                 kVideoHeight + 2 * kOuterMargin);
}

void PlayerView::setThemeService(ThemeService* /*theme*/)
{
    // 视频渲染背景保持黑色（无论亮暗主题）；预留接口。
}

void PlayerView::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    layoutChildren();
}

void PlayerView::layoutChildren()
{
    if (!m_videoContainer) return;

    // 视频容器居中显示，尺寸固定
    const int cx = (width() - kVideoWidth) / 2;
    // 优先上对齐（更接近效果图），但至少保留 kOuterMargin 顶部
    const int cy = qMax(kOuterMargin, (height() - kVideoHeight) / 2);
    m_videoContainer->move(cx, cy);

    // 控制栏位置：视频底部内侧，宽度 = 视频宽 * 4/5，居中，距底 8px
    if (m_controlContainer) {
        const int controlWidth  = kVideoWidth * 4 / 5;
        const int controlHeight = 40;
        const int controlX = cx + (kVideoWidth - controlWidth) / 2;
        int controlY;
        if (m_controlBarVisible) {
            controlY = cy + kVideoHeight - controlHeight - 8;
        } else {
            // 隐藏态：让控制栏藏在视频区域外，动画显示时从下方滑入
            controlY = cy + kVideoHeight;
        }
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

    const int cx = m_videoContainer->x();
    const int cy = m_videoContainer->y();
    const int controlWidth  = kVideoWidth * 4 / 5;
    const int controlHeight = 40;
    const int targetX = cx + (kVideoWidth - controlWidth) / 2;
    const int targetY = cy + kVideoHeight - controlHeight - 8;
    const int startY  = cy + kVideoHeight;

    auto* posAnim = new QPropertyAnimation(m_controlContainer, "pos", this);
    posAnim->setDuration(250);
    posAnim->setStartValue(QPoint(targetX, startY));
    posAnim->setEndValue(QPoint(targetX, targetY));
    posAnim->setEasingCurve(QEasingCurve::OutCubic);

    auto* opacity = new QGraphicsOpacityEffect(m_controlContainer);
    m_controlContainer->setGraphicsEffect(opacity);
    opacity->setOpacity(0);

    auto* fadeAnim = new QPropertyAnimation(opacity, "opacity", this);
    fadeAnim->setDuration(250);
    fadeAnim->setStartValue(0.0);
    fadeAnim->setEndValue(1.0);
    fadeAnim->setEasingCurve(QEasingCurve::OutCubic);

    auto* group = new QParallelAnimationGroup(this);
    group->addAnimation(posAnim);
    group->addAnimation(fadeAnim);
    group->start(QAbstractAnimation::DeleteWhenStopped);
}

void PlayerView::hideControlBar()
{
    if (!m_controlContainer || !m_videoContainer) return;
    if (!m_controlBarVisible) return;
    m_controlBarVisible = false;

    const int cy = m_videoContainer->y();
    const int endY = cy + kVideoHeight;

    auto* posAnim = new QPropertyAnimation(m_controlContainer, "pos", this);
    posAnim->setDuration(250);
    posAnim->setStartValue(m_controlContainer->pos());
    posAnim->setEndValue(QPoint(m_controlContainer->x(), endY));
    posAnim->setEasingCurve(QEasingCurve::InCubic);

    auto* opacity = qobject_cast<QGraphicsOpacityEffect*>(
        m_controlContainer->graphicsEffect());
    if (!opacity) {
        opacity = new QGraphicsOpacityEffect(m_controlContainer);
        m_controlContainer->setGraphicsEffect(opacity);
    }

    auto* fadeAnim = new QPropertyAnimation(opacity, "opacity", this);
    fadeAnim->setDuration(250);
    fadeAnim->setStartValue(1.0);
    fadeAnim->setEndValue(0.0);
    fadeAnim->setEasingCurve(QEasingCurve::InCubic);

    auto* group = new QParallelAnimationGroup(this);
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

    m_controlBar->setVolumeDisplay(m_vm->volume());
}
