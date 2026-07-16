#include "view/player/videorenderwidget.h"

#include <QPainter>
#include <QPainterPath>
#include <QRegion>
#include <QOpenGLTexture>
#include <QMatrix4x4>
#include <QVector2D>
#include <QtMath>

// ===================================================================
//  VideoRenderWidget —— OpenGL 实现
// ===================================================================

static const char* kVertexShader = R"GLSL(#version 330 core
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_tex;
uniform mat4 u_mvp;
out vec2 v_tex;
void main() {
    v_tex = a_tex;
    gl_Position = u_mvp * vec4(a_pos, 0.0, 1.0);
}
)GLSL";

static const char* kFragmentShader = R"GLSL(#version 330 core
in vec2 v_tex;
uniform sampler2D u_tex;
out vec4 fragColor;
void main() {
    fragColor = texture(u_tex, v_tex);
}
)GLSL";

// 全屏四边形顶点：xy NDC, uv
// 我们会绘制一个 [-1,1]² 的"画布矩形"，然后用 u_mvp 把它压缩/平移到目标位置
// 这样 viewport 之外（黑边）的部分不会着色。
static const float kQuadVerts[] = {
    // x,    y,   u,   v
    -1.0f, -1.0f, 0.0f, 1.0f,
     1.0f, -1.0f, 1.0f, 1.0f,
    -1.0f,  1.0f, 0.0f, 0.0f,
     1.0f,  1.0f, 1.0f, 0.0f,
};

VideoRenderWidget::VideoRenderWidget(QWidget* parent)
    : QOpenGLWidget(parent)
{
    // 关闭 Qt 的自动背景填充（接管绘制）
    setAutoFillBackground(false);
    setMinimumSize(320, 180);
    m_throttle.start();
}

VideoRenderWidget::~VideoRenderWidget()
{
    if (m_tex) {
        delete m_tex;
        m_tex = nullptr;
    }
}

void VideoRenderWidget::setContainerRadius(int radius)
{
    m_radius = radius;  // 圆角由 RoundedVideoContainer 负责，这里只是占位参数
    update();
}

void VideoRenderWidget::setContainerBgColor(const QColor& color)
{
    m_bgColor = color;
    if (context()) {
        QOpenGLFunctions::glClearColor(
            float(color.redF()), float(color.greenF()), float(color.blueF()), 1.0f);
    }
    update();
}

void VideoRenderWidget::updateFrame(const QImage& frame)
{
    if (frame.isNull()) return;
    m_currentFrame = frame;  // 隐式共享

    // 30fps 节流
    if (m_throttle.elapsed() >= kMinIntervalMs) {
        m_throttle.restart();
        update();
    }
}

void VideoRenderWidget::clear()
{
    m_currentFrame = QImage();
    m_uploadedCacheKey = 0;
    update();
}

void VideoRenderWidget::initializeGL()
{
    QOpenGLFunctions::initializeOpenGLFunctions();

    // 背景色由 m_bgColor 同步
    QOpenGLFunctions::glClearColor(
        float(m_bgColor.redF()), float(m_bgColor.greenF()), float(m_bgColor.blueF()), 1.0f);

    // Qt 在 reparent / show-hide 切换时可能再次回调 initializeGL。
    // 此前 addShaderFromSourceCode 不带去重，第二次进入会把同一个 shader 源重复 add 到 program，
    // 触发 "function 'main' is already defined" 链接错误，随后在 paintGL 中访问无效 program 即崩溃。
    if (m_vao.isCreated()) {
        m_vao.destroy();
    }
    if (m_vbo.isCreated()) {
        m_vbo.destroy();
    }
    // QOpenGLShaderProgram 禁止拷贝/赋值，复用同一对象并清空 shader 即可
    m_program.removeAllShaders();

    // 着色器
    if (!m_program.addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader)) {
        qWarning("VideoRenderWidget: vertex shader compile failed: %s",
                 qPrintable(m_program.log()));
        return;  // 失败则早退，避免后续在坏 program 上操作
    }
    if (!m_program.addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader)) {
        qWarning("VideoRenderWidget: fragment shader compile failed: %s",
                 qPrintable(m_program.log()));
        return;
    }
    if (!m_program.link()) {
        qWarning("VideoRenderWidget: program link failed: %s",
                 qPrintable(m_program.log()));
        return;
    }

    // VAO + VBO
    m_vao.create();
    m_vao.bind();

    m_vbo.create();
    m_vbo.bind();
    m_vbo.setUsagePattern(QOpenGLBuffer::StaticDraw);
    m_vbo.allocate(kQuadVerts, sizeof(kQuadVerts));

    m_program.enableAttributeArray(0);
    m_program.setAttributeBuffer(0, GL_FLOAT, 0, 2, 4 * sizeof(float));
    m_program.enableAttributeArray(1);
    m_program.setAttributeBuffer(1, GL_FLOAT, 2 * sizeof(float), 2, 4 * sizeof(float));

    m_vao.release();
    m_vbo.release();

    // 旧 texture 绑定到旧 GL context；context 重建后句柄失效，必须重建。
    if (m_tex) {
        delete m_tex;
        m_tex = nullptr;
    }
    m_tex = new QOpenGLTexture(QOpenGLTexture::Target2D);
    m_tex->setMinificationFilter(QOpenGLTexture::Linear);
    m_tex->setMagnificationFilter(QOpenGLTexture::Linear);
    m_tex->setWrapMode(QOpenGLTexture::ClampToEdge);
    m_tex->setFormat(QOpenGLTexture::RGBAFormat);
    m_tex->setSize(1, 1);
    m_tex->allocateStorage();

    // 强制下帧重新上传，避免跨 context 的 cacheKey 误命中
    m_uploadedCacheKey = 0;
}

void VideoRenderWidget::resizeGL(int w, int h)
{
    Q_UNUSED(w);
    Q_UNUSED(h);
    // 我们在 paintGL 里用 viewport + mvp 计算，resizeGL 不必做事
    glViewport(0, 0, w, h);
}

void VideoRenderWidget::paintGL()
{
    // widget 隐藏 / GL context 失效时不要访问任何 GL 资源，避免 reparent 期间崩溃
    if (!isVisibleTo(QWidget::parentWidget()) && !isVisible()) return;
    if (!context() || !context()->isValid()) return;
    if (!m_tex) return;

    QOpenGLFunctions::glClear(GL_COLOR_BUFFER_BIT);

    // Shader 链接失败时直接返回，避免在无效 program 上调用 glDrawArrays 崩溃
    if (!m_program.isLinked()) return;

    if (m_currentFrame.isNull()) return;

    // 计算等比居中后的子矩形（viewport 像素坐标）
    const QSize view = size();
    if (view.isEmpty()) return;

    QSize imgSize = m_currentFrame.size().scaled(view, Qt::KeepAspectRatio);
    if (imgSize.isEmpty()) return;
    const int x = (view.width()  - imgSize.width())  / 2;
    const int y = (view.height() - imgSize.height()) / 2;

    // QOpenGLWidget 会在 paintGL 前 makeCurrent 并设 viewport 到整个 widget；
    // 先清掉整屏（黑边），再把 viewport 缩到子矩形内画视频帧
    const int w = view.width();
    const int h = view.height();
    glViewport(0, 0, w, h);

    // 上传纹理（仅当帧内容变化时上传）
    const qint64 ck = m_currentFrame.cacheKey();
    if (ck != m_uploadedCacheKey) {
        uploadFrame(m_currentFrame);
    }

    glViewport(x, y, imgSize.width(), imgSize.height());

    m_program.bind();
    m_vao.bind();

    QMatrix4x4 mvp;
    m_program.setUniformValue("u_mvp", mvp);

    m_program.setUniformValue("u_tex", 0);
    m_tex->bind(0);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    m_tex->release(0);
    m_vao.release();
    m_program.release();

    // 还原 viewport
    glViewport(0, 0, w, h);
}

void VideoRenderWidget::uploadFrame(const QImage& frame)
{
    if (frame.isNull()) return;

    // 转成 GL 友好的格式（RGBA8）
    QImage tex;
    switch (frame.format()) {
    case QImage::Format_RGB32:
    case QImage::Format_ARGB32:
    case QImage::Format_ARGB32_Premultiplied:
    case QImage::Format_RGBA8888:
    case QImage::Format_RGBA8888_Premultiplied:
        tex = frame.convertToFormat(QImage::Format_RGBA8888);
        break;
    case QImage::Format_RGB888:
        tex = frame.convertToFormat(QImage::Format_RGBA8888);
        break;
    default:
        tex = frame.convertToFormat(QImage::Format_RGBA8888);
        break;
    }
    // QImage 内存布局是 top-row first，OpenGL 纹理 V=0 也对应 top row；
    // 顶点数据 UV 顺序（左下 V=1，左上 V=0）正好让屏幕上半部分采到图像顶部，
    // 因此这里**不再**做 mirrored(false, true)，否则会被翻两次导致画面颠倒。

    // 尺寸变化时重新分配纹理存储；否则直接 setData 覆盖
    if (m_tex->width()  != tex.width() ||
        m_tex->height() != tex.height())
    {
        m_tex->destroy();
        m_tex->create();
        m_tex->setFormat(QOpenGLTexture::RGBAFormat);
        m_tex->setSize(tex.width(), tex.height());
        m_tex->setMinificationFilter(QOpenGLTexture::Linear);
        m_tex->setMagnificationFilter(QOpenGLTexture::Linear);
        m_tex->setWrapMode(QOpenGLTexture::ClampToEdge);
        m_tex->allocateStorage();
    }
    m_tex->setData(tex, QOpenGLTexture::DontGenerateMipMaps);

    m_uploadedCacheKey = frame.cacheKey();
}

void VideoRenderWidget::computeDrawRectNdc(QRectF& outTexRectNdc) const
{
    // reserved：保留此函数以便后续把"等比居中"完全搬到 shader 里
    const QSize view = size();
    if (view.isEmpty()) {
        outTexRectNdc = QRectF(-1, -1, 2, 2);
        return;
    }
    QSize imgSize = m_currentFrame.size().scaled(view, Qt::KeepAspectRatio);
    const float wRatio = float(imgSize.width())  / float(view.width());
    const float hRatio = float(imgSize.height()) / float(view.height());
    outTexRectNdc = QRectF(-wRatio, -hRatio, 2 * wRatio, 2 * hRatio);
}

// ===================================================================
//  RoundedVideoContainer —— 与原实现保持一致
// ===================================================================

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

    QPainterPath path;
    path.addRoundedRect(QRectF(rect), m_radius, m_radius);
    painter.fillPath(path, QColor("#1A1A1A"));
}
