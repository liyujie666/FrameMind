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
#include <QKeyEvent>
#include <QPropertyAnimation>
#include <QGraphicsOpacityEffect>
#include <QCursor>

PlayerView::PlayerView(QWidget* parent)
    : QWidget(parent)
    , m_mouseOver(false)
    , m_controlBarVisible(false)
    , m_isFullscreen(false)
{
    setAttribute(Qt::WA_StyledBackground, false);
    setAutoFillBackground(false);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(320, 180);

    // 视频容器直接用布局填满整个 PlayerView
    m_videoContainer = new RoundedVideoContainer(this);
    m_videoContainer->setRadius(10);
    m_videoContainer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    m_renderWidget = new VideoRenderWidget(m_videoContainer);
    m_renderWidget->setContainerBgColor(Qt::black);
    m_renderWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto* videoLayout = new QVBoxLayout(m_videoContainer);
    videoLayout->setContentsMargins(0, 0, 0, 0);
    videoLayout->setSpacing(0);
    videoLayout->addWidget(m_renderWidget, 1);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);
    rootLayout->addWidget(m_videoContainer, 1);

    // 控制栏容器：悬浮在 PlayerView 上，绝对定位，不参与布局
    // 不加 QGraphicsOpacityEffect（会破坏 QOpenGLWidget 渲染），
    // 改用 QPropertyAnimation 直接动画 windowOpacity——但 windowOpacity 只对顶层窗口有效。
    // 所以用最简单的方式：直接用 setVisible + 淡入淡出靠动画 styleSheet alpha 实现，
    // 或者只做 show/hide，不做渐变（最安全）。
    // 这里选择：控制栏始终存在但初始不可见，通过动画改变它的 maximumHeight 实现滑入效果。
    m_controlContainer = new QWidget(this);
    m_controlContainer->setStyleSheet(
        "background-color: rgba(15, 15, 20, 210); border-radius: 16px;");
    m_controlContainer->setFixedHeight(44);
    m_controlContainer->hide();   // 初始隐藏，鼠标进入时 show

    m_controlBar = new PlayerControlBar(m_controlContainer);
    auto* controlLayout = new QHBoxLayout(m_controlContainer);
    controlLayout->setContentsMargins(8, 0, 8, 0);
    controlLayout->setSpacing(0);
    controlLayout->addWidget(m_controlBar);

    // 隐藏定时器
    m_hideTimer.setSingleShot(true);
    m_hideTimer.setInterval(m_hideDelayMs);
    connect(&m_hideTimer, &QTimer::timeout, this, &PlayerView::hideControlBar);

    m_videoContainer->installEventFilter(this);

    connect(m_controlBar, &PlayerControlBar::fullscreenClicked,
            this, &PlayerView::toggleFullscreen);

    // 首次布局
    updateControlBarGeometry();
}

QSize PlayerView::sizeHint() const
{
    return QSize(720, 405);
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
    updateControlBarGeometry();
}

void PlayerView::updateControlBarGeometry()
{
    if (!m_controlContainer) return;

    // 全屏时基于全屏窗口尺寸，否则基于 PlayerView 自身尺寸
    QWidget* host = (m_isFullscreen && m_fullscreenWindow) ? m_fullscreenWindow : this;
    const int w = host->width();
    const int h = host->height();
    if (w <= 0 || h <= 0) return;

    const int barW = w * 2 / 3;
    const int barH = 44;
    const int barX = (w - barW) / 2;
    const int barY = h - barH - 12;

    m_controlContainer->setGeometry(barX, barY, barW, barH);
    m_controlContainer->raise();
}

bool PlayerView::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == m_videoContainer) {
        if (event->type() == QEvent::Enter) {
            m_mouseOver = true;
            m_hideTimer.stop();
            showControlBar();
            return false;
        } else if (event->type() == QEvent::Leave) {
            // 检查鼠标是否移到了控制栏上，是的话不隐藏
            const QPoint globalPos = QCursor::pos();
            if (m_controlContainer && m_controlContainer->isVisible()) {
                const QRect controlGlobal = QRect(
                    m_controlContainer->mapToGlobal(QPoint(0,0)),
                    m_controlContainer->size());
                if (controlGlobal.contains(globalPos)) return false;
            }
            m_mouseOver = false;
            m_hideTimer.start();
            return false;
        }
    }
    if (obj == m_fullscreenWindow) {
        if (event->type() == QEvent::KeyPress) {
            auto* ke = static_cast<QKeyEvent*>(event);
            if (ke->key() == Qt::Key_Escape) {
                exitFullscreen();
                return true;
            }
            if (ke->key() == Qt::Key_F11) {
                toggleFullscreen();
                return true;
            }
        }
        if (event->type() == QEvent::MouseMove) {
            m_hideTimer.stop();
            showControlBar();
            m_hideTimer.start();
            return false;
        }
        if (event->type() == QEvent::Resize) {
            updateControlBarGeometry();
            return false;
        }
        if (event->type() == QEvent::MouseButtonDblClick) {
            exitFullscreen();
            return true;
        }
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
    m_isFullscreen = true;

    if (!m_fullscreenWindow) {
        m_fullscreenWindow = new QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint);
        m_fullscreenWindow->setStyleSheet("background: black;");
        m_fullscreenWindow->installEventFilter(this);
        m_fullscreenWindow->setMouseTracking(true);

        auto* fsLayout = new QVBoxLayout(m_fullscreenWindow);
        fsLayout->setContentsMargins(0, 0, 0, 0);
        fsLayout->setSpacing(0);
    }

    auto* fsLayout = qobject_cast<QVBoxLayout*>(m_fullscreenWindow->layout());
    if (fsLayout) {
        fsLayout->addWidget(m_videoContainer);
    }
    m_videoContainer->setRadius(0);
    m_videoContainer->setMouseTracking(true);
    m_videoContainer->show();

    // 控制栏移入全屏窗口，绝对定位，初始隐藏
    m_controlContainer->setParent(m_fullscreenWindow);
    m_controlContainer->hide();
    m_controlBarVisible = false;

    m_fullscreenWindow->showFullScreen();
    m_fullscreenWindow->setFocus();
    m_fullscreenWindow->activateWindow();

    // 更新控制栏位置（基于全屏窗口尺寸）
    updateControlBarGeometry();

    m_videoContainer->installEventFilter(this);

    m_controlBar->setFullscreen(true);
    emit fullscreenChanged(true);
}

void PlayerView::exitFullscreen()
{
    if (!m_isFullscreen) return;
    m_isFullscreen = false;

    // 把视频容器移回 PlayerView 的布局
    auto* fsLayout = m_fullscreenWindow ? m_fullscreenWindow->layout() : nullptr;
    if (fsLayout) fsLayout->removeWidget(m_videoContainer);

    m_videoContainer->setParent(this);
    m_videoContainer->setRadius(10);

    auto* rootLayout = qobject_cast<QVBoxLayout*>(layout());
    if (rootLayout) rootLayout->addWidget(m_videoContainer, 1);
    m_videoContainer->show();

    // 控制栏容器移回 PlayerView
    m_controlContainer->setParent(this);
    m_controlContainer->hide();
    m_controlBarVisible = false;

    if (m_fullscreenWindow) m_fullscreenWindow->hide();

    m_controlBar->setFullscreen(false);
    updateControlBarGeometry();
    emit fullscreenChanged(false);
}

void PlayerView::showControlBar()
{
    if (m_controlBarVisible) return;
    m_controlBarVisible = true;
    if (m_controlContainer) {
        updateControlBarGeometry();
        m_controlContainer->show();
        m_controlContainer->raise();
    }
}

void PlayerView::hideControlBar()
{
    if (!m_controlBarVisible) return;
    m_controlBarVisible = false;
    if (m_controlContainer) {
        m_controlContainer->hide();
    }
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
    connect(m_vm, &PlayerViewModel::rawFrameReady,
            m_renderWidget, qOverload<const VideoFrame&>(&VideoRenderWidget::updateFrame));

    // 切换新文件时立即清空渲染器，防止旧帧画面在新视频首帧到来前残留
    connect(m_vm, &PlayerViewModel::videoFileChanging,
            m_renderWidget, &VideoRenderWidget::clear);

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
