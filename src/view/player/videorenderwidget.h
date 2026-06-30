#ifndef FRAMEMIND_VIDEORENDERWIDGET_H
#define FRAMEMIND_VIDEORENDERWIDGET_H

#include <QWidget>
#include <QImage>
#include <QElapsedTimer>

/**
 * 视频渲染 widget：保存当前帧，paintEvent 用 QPainter 按等比缩放绘制。
 * 帧刷新做 30fps 节流，避免高帧率下 UI 过度重绘。
 */
class VideoRenderWidget : public QWidget {
    Q_OBJECT
public:
    explicit VideoRenderWidget(QWidget* parent = nullptr);

    void setContainerRadius(int radius);
    void setContainerBgColor(const QColor& color);

public slots:
    void updateFrame(const QImage& frame);
    void clear();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QImage        m_currentFrame;
    QElapsedTimer m_throttle;
    int           m_radius = 8;
    QColor        m_bgColor = Qt::black;
    static constexpr int kMinIntervalMs = 33;  // ~30fps
};

/**
 * 带圆角的视频容器 widget
 */
class RoundedVideoContainer : public QWidget {
    Q_OBJECT
public:
    explicit RoundedVideoContainer(QWidget* parent = nullptr);

    void setRadius(int radius);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void updateMask();
    int m_radius = 12;
};

#endif // FRAMEMIND_VIDEORENDERWIDGET_H
