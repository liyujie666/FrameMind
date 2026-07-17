#ifndef FRAMEMIND_VIDEORENDERWIDGET_H
#define FRAMEMIND_VIDEORENDERWIDGET_H

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLBuffer>
#include <QOpenGLVertexArrayObject>
#include <QImage>

class QOpenGLTexture;

/**
 * 视频渲染 widget —— 使用 QOpenGLWidget + 着色器把每帧 QImage 上传到 GPU 绘制。
 *
 * 行为与旧 QWidget 版本保持一致：
 *   - 30fps 节流，避免高帧率下过度重绘
 *   - 等比缩放、居中、剩余区域填充 m_bgColor（默认黑）
 *
 * 渲染策略：
 *   - 1 个 [-1,1]×[-1,1] 的 NDC 四边形，固定顶点 / 纹理坐标
 *   - 用 glViewport 把实际绘制区域限制到"等比居中"后的子矩形
 *     （黑边由 glClearColor 填充，无需在片段着色器中处理）
 *   - 每帧仅在 cacheKey 变化时把 QImage 转 RGBA8 + 上下翻转后 setData 到纹理
 */
class VideoRenderWidget : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
public:
    explicit VideoRenderWidget(QWidget* parent = nullptr);
    ~VideoRenderWidget() override;

    void setContainerRadius(int radius);
    void setContainerBgColor(const QColor& color);

public slots:
    void updateFrame(const QImage& frame);
    void clear();

protected:
    // QOpenGLWidget
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

private:
    void uploadFrame(const QImage& frame);
    void computeDrawRectNdc(QRectF& outTexRectNdc) const;

    // GL 资源（生命周期受 QOpenGLWidget 控制，destroyed 时随 context 释放）
    QOpenGLShaderProgram m_program;
    QOpenGLBuffer        m_vbo = QOpenGLBuffer(QOpenGLBuffer::VertexBuffer);
    QOpenGLVertexArrayObject m_vao;
    QOpenGLTexture*      m_tex = nullptr;   // 延迟到 initializeGL 创建

    // 帧缓冲
    QImage        m_currentFrame;          // 最近一帧（QImage 隐式共享）
    qint64        m_uploadedCacheKey = 0;  // 上一次上传帧的 cacheKey()，0 = 未上传

    QColor        m_bgColor = Qt::black;
    int           m_radius  = 8;           // 仅占位，OpenGL 不直接画圆角；由 RoundedVideoContainer 处理
};

/**
 * 带圆角的视频容器 widget（保持原实现，负责把 OpenGL widget 套在圆角蒙版里）
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
