#include "view/player/videorenderwidget.h"

#include <QPainter>
#include <QPainterPath>
#include <QRegion>

VideoRenderWidget::VideoRenderWidget(QWidget* parent)
    : QWidget(parent)
    , m_radius(8)
{
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAutoFillBackground(false);
    setMinimumSize(320, 180);
    m_throttle.start();
}

void VideoRenderWidget::setContainerRadius(int radius)
{
    m_radius = radius;
    update();
}

void VideoRenderWidget::setContainerBgColor(const QColor& color)
{
    m_bgColor = color;
    update();
}

void VideoRenderWidget::updateFrame(const QImage& frame)
{
    if (frame.isNull()) return;
    m_currentFrame = frame;  // QImage 隐式共享，无深拷贝
    // 30fps 节流：距上次重绘不足 kMinIntervalMs 则跳过本次 update
    if (m_throttle.elapsed() >= kMinIntervalMs) {
        m_throttle.restart();
        update();
    }
}

void VideoRenderWidget::clear()
{
    m_currentFrame = QImage();
    update();
}

void VideoRenderWidget::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const QRect rect = this->rect();

    // 填充黑色背景
    painter.fillRect(rect, m_bgColor);

    if (m_currentFrame.isNull()) {
        return;
    }

    // 等比缩放并居中绘制（KeepAspectRatio）
    const QSize imgSize = m_currentFrame.size().scaled(rect.size(), Qt::KeepAspectRatio);
    const QRect target(QPoint((rect.width() - imgSize.width()) / 2,
                              (rect.height() - imgSize.height()) / 2),
                       imgSize);

    painter.drawImage(target, m_currentFrame);
}

// ========== RoundedVideoContainer ==========

RoundedVideoContainer::RoundedVideoContainer(QWidget* parent)
    : QWidget(parent)
    , m_radius(12)
{
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAutoFillBackground(false);
}

void RoundedVideoContainer::setRadius(int radius)
{
    m_radius = radius;
    updateMask();
}

void RoundedVideoContainer::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    updateMask();
}

void RoundedVideoContainer::updateMask()
{
    QRegion region(rect());
    QRegion rounded = region.subtracted(QRegion(rect().adjusted(m_radius, 0, -m_radius, 0)));
    rounded = rounded.subtracted(QRegion(rect().adjusted(0, m_radius, 0, -m_radius)));

    QPainterPath path;
    path.addRoundedRect(rect(), m_radius, m_radius);
    setMask(QRegion(path.toFillPolygon().toPolygon()));
}

void RoundedVideoContainer::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRect rect = this->rect();

    // 绘制圆角矩形背景
    QPainterPath path;
    path.addRoundedRect(QRectF(rect), m_radius, m_radius);
    painter.fillPath(path, QColor("#1A1A1A"));
}
