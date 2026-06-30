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

public slots:
    void updateFrame(const QImage& frame);
    void clear();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QImage        m_currentFrame;
    QElapsedTimer m_throttle;
    static constexpr int kMinIntervalMs = 33;  // ~30fps
};

#endif // FRAMEMIND_VIDEORENDERWIDGET_H
