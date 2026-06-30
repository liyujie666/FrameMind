#include "view/player/videorenderwidget.h"

#include <QPainter>

VideoRenderWidget::VideoRenderWidget(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, Qt::black);
    setPalette(pal);
    setMinimumSize(320, 180);
    m_throttle.start();
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
    painter.fillRect(rect(), Qt::black);

    if (m_currentFrame.isNull()) {
        return;
    }

    // 等比缩放并居中绘制（KeepAspectRatio）
    const QSize imgSize = m_currentFrame.size().scaled(size(), Qt::KeepAspectRatio);
    const QRect target(QPoint((width() - imgSize.width()) / 2,
                              (height() - imgSize.height()) / 2),
                       imgSize);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(target, m_currentFrame);
}
