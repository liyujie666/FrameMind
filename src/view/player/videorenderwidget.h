#ifndef FRAMEMIND_VIDEORENDERWIDGET_H
#define FRAMEMIND_VIDEORENDERWIDGET_H

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLBuffer>
#include <QOpenGLVertexArrayObject>
#include <QImage>

#include "model/videoframe.h"

/**
 * 视频渲染 widget —— 使用 OpenGL shader 直接渲染 YUV420P/NV12/RGBA 原始帧数据。
 *
 * 渲染策略：
 *   - YUV420P: 3 个单通道纹理 (Y/U/V)，fragment shader 做 BT.601 矩阵转换
 *   - NV12:    2 个纹理 (Y + UV interleaved)，fragment shader 转换
 *   - RGBA/BGRA: 单个 RGBA 纹理直接采样
 *   - 等比缩放、居中、剩余区域填充黑色
 */
class VideoRenderWidget : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
public:
    explicit VideoRenderWidget(QWidget* parent = nullptr);
    ~VideoRenderWidget() override;

    void setContainerRadius(int radius);
    void setContainerBgColor(const QColor& color);

public slots:
    void updateFrame(const VideoFrame& frame);
    void updateFrame(const QImage& frame);
    void clear();

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

private:
    enum class FrameFormat { None, YUV420P, NV12, RGBA };

    void initShaders();
    void destroyTextures();
    void uploadYUV420P(const VideoFrame& frame);
    void uploadNV12(const VideoFrame& frame);
    void uploadRGBA(const VideoFrame& frame);
    void uploadQImage(const QImage& frame);
    void createTexture(GLuint& tex, int w, int h, GLenum internalFmt, GLenum fmt, const void* data);
    void updateTexture(GLuint tex, int w, int h, GLenum fmt, GLenum type, const void* data);

    // Shader programs for different formats
    QOpenGLShaderProgram m_yuvProgram;
    QOpenGLShaderProgram m_nv12Program;
    QOpenGLShaderProgram m_rgbaProgram;

    QOpenGLBuffer m_vbo{QOpenGLBuffer::VertexBuffer};
    QOpenGLVertexArrayObject m_vao;

    // YUV420P textures
    GLuint m_texY = 0;
    GLuint m_texU = 0;
    GLuint m_texV = 0;

    // NV12 textures
    GLuint m_texNV12_Y = 0;
    GLuint m_texNV12_UV = 0;

    // RGBA texture
    GLuint m_texRGBA = 0;

    // Current frame state
    VideoFrame m_currentVideoFrame;
    QImage m_currentQImage;
    FrameFormat m_currentFormat = FrameFormat::None;
    int m_frameWidth = 0;
    int m_frameHeight = 0;
    bool m_needUpload = false;

    QColor m_bgColor = Qt::black;
    int m_radius = 8;
    bool m_glInitialized = false;
    bool m_pendingClear = false;
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
