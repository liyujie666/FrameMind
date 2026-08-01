#include "view/player/videorenderwidget.h"

#include <QPainter>
#include <QPainterPath>
#include <QRegion>
#include <QMatrix4x4>
#include <QtMath>

// ===================================================================
//  Shader sources
// ===================================================================

static const char* kVertexShaderSrc = R"GLSL(#version 330 core
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_tex;
out vec2 v_tex;
void main() {
    v_tex = a_tex;
    gl_Position = vec4(a_pos, 0.0, 1.0);
}
)GLSL";

static const char* kYUV420PFragSrc = R"GLSL(#version 330 core
in vec2 v_tex;
uniform sampler2D u_texY;
uniform sampler2D u_texU;
uniform sampler2D u_texV;
out vec4 fragColor;
void main() {
    float y = texture(u_texY, v_tex).r;
    float u = texture(u_texU, v_tex).r - 0.5;
    float v = texture(u_texV, v_tex).r - 0.5;
    // BT.601 full-range
    float r = y + 1.402 * v;
    float g = y - 0.344136 * u - 0.714136 * v;
    float b = y + 1.772 * u;
    fragColor = vec4(r, g, b, 1.0);
}
)GLSL";

static const char* kNV12FragSrc = R"GLSL(#version 330 core
in vec2 v_tex;
uniform sampler2D u_texY;
uniform sampler2D u_texUV;
out vec4 fragColor;
void main() {
    float y = texture(u_texY, v_tex).r;
    vec2 uv = texture(u_texUV, v_tex).rg;
    float u = uv.r - 0.5;
    float v = uv.g - 0.5;
    float r = y + 1.402 * v;
    float g = y - 0.344136 * u - 0.714136 * v;
    float b = y + 1.772 * u;
    fragColor = vec4(r, g, b, 1.0);
}
)GLSL";

static const char* kRGBAFragSrc = R"GLSL(#version 330 core
in vec2 v_tex;
uniform sampler2D u_tex;
out vec4 fragColor;
void main() {
    fragColor = texture(u_tex, v_tex);
}
)GLSL";

static const float kQuadVerts[] = {
    // x,    y,     u,   v
    -1.0f, -1.0f,  0.0f, 1.0f,
     1.0f, -1.0f,  1.0f, 1.0f,
    -1.0f,  1.0f,  0.0f, 0.0f,
     1.0f,  1.0f,  1.0f, 0.0f,
};

// ===================================================================
//  VideoRenderWidget
// ===================================================================

VideoRenderWidget::VideoRenderWidget(QWidget* parent)
    : QOpenGLWidget(parent)
{
    setAutoFillBackground(false);
    setMinimumSize(320, 180);
}

VideoRenderWidget::~VideoRenderWidget()
{
    makeCurrent();
    destroyTextures();
    doneCurrent();
}

void VideoRenderWidget::setContainerRadius(int radius)
{
    m_radius = radius;
    update();
}

void VideoRenderWidget::setContainerBgColor(const QColor& color)
{
    m_bgColor = color;
    if (context()) {
        makeCurrent();
        glClearColor(float(color.redF()), float(color.greenF()),
                     float(color.blueF()), 1.0f);
        doneCurrent();
    }
    update();
}

void VideoRenderWidget::updateFrame(const VideoFrame& frame)
{
    if (frame.isNull()) return;
    m_currentVideoFrame = frame;
    m_currentQImage = QImage();

    switch (frame.format()) {
    case SP_FMT_YUV420P: m_currentFormat = FrameFormat::YUV420P; break;
    case SP_FMT_NV12:    m_currentFormat = FrameFormat::NV12; break;
    case SP_FMT_RGBA:
    case SP_FMT_BGRA:    m_currentFormat = FrameFormat::RGBA; break;
    default:             return;
    }
    m_frameWidth = frame.width();
    m_frameHeight = frame.height();
    m_needUpload = true;
    update();
}

void VideoRenderWidget::updateFrame(const QImage& frame)
{
    if (frame.isNull()) return;
    m_currentQImage = frame;
    m_currentVideoFrame = VideoFrame();
    m_currentFormat = FrameFormat::RGBA;
    m_frameWidth = frame.width();
    m_frameHeight = frame.height();
    m_needUpload = true;
    update();
}

void VideoRenderWidget::clear()
{
    m_currentVideoFrame = VideoFrame();
    m_currentQImage = QImage();
    m_currentFormat = FrameFormat::None;
    m_needUpload = false;
    m_frameWidth = 0;
    m_frameHeight = 0;
    // 设置标志，在下一次 paintGL（GL 上下文确保正确）里销毁纹理并清屏
    m_pendingClear = true;
    update();
}

void VideoRenderWidget::initializeGL()
{
    initializeOpenGLFunctions();
    glClearColor(float(m_bgColor.redF()), float(m_bgColor.greenF()),
                 float(m_bgColor.blueF()), 1.0f);

    // Rebuild VAO/VBO on re-init (reparent can trigger this)
    if (m_vao.isCreated()) m_vao.destroy();
    if (m_vbo.isCreated()) m_vbo.destroy();
    destroyTextures();

    initShaders();

    m_vao.create();
    m_vao.bind();
    m_vbo.create();
    m_vbo.bind();
    m_vbo.setUsagePattern(QOpenGLBuffer::StaticDraw);
    m_vbo.allocate(kQuadVerts, sizeof(kQuadVerts));

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));

    m_vao.release();
    m_vbo.release();

    m_glInitialized = true;
    m_needUpload = true;
}

void VideoRenderWidget::initShaders()
{
    m_yuvProgram.removeAllShaders();
    m_nv12Program.removeAllShaders();
    m_rgbaProgram.removeAllShaders();

    m_yuvProgram.addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShaderSrc);
    m_yuvProgram.addShaderFromSourceCode(QOpenGLShader::Fragment, kYUV420PFragSrc);
    m_yuvProgram.link();

    m_nv12Program.addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShaderSrc);
    m_nv12Program.addShaderFromSourceCode(QOpenGLShader::Fragment, kNV12FragSrc);
    m_nv12Program.link();

    m_rgbaProgram.addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShaderSrc);
    m_rgbaProgram.addShaderFromSourceCode(QOpenGLShader::Fragment, kRGBAFragSrc);
    m_rgbaProgram.link();
}

void VideoRenderWidget::resizeGL(int w, int h)
{
    glViewport(0, 0, w, h);
}

void VideoRenderWidget::paintGL()
{
    if (!isVisible()) return;
    if (!context() || !context()->isValid()) return;

    if (m_pendingClear) {
        m_pendingClear = false;
        destroyTextures();
    }

    glClear(GL_COLOR_BUFFER_BIT);

    if (m_currentFormat == FrameFormat::None) return;
    if (m_frameWidth <= 0 || m_frameHeight <= 0) return;

    // Upload data if needed
    if (m_needUpload) {
        m_needUpload = false;
        if (!m_currentVideoFrame.isNull()) {
            switch (m_currentFormat) {
            case FrameFormat::YUV420P: uploadYUV420P(m_currentVideoFrame); break;
            case FrameFormat::NV12:    uploadNV12(m_currentVideoFrame); break;
            case FrameFormat::RGBA:    uploadRGBA(m_currentVideoFrame); break;
            default: break;
            }
        } else if (!m_currentQImage.isNull()) {
            uploadQImage(m_currentQImage);
        }
    }

    // Compute aspect-fit viewport
    const QSize viewSize = size();
    if (viewSize.isEmpty()) return;
    QSize imgSize(m_frameWidth, m_frameHeight);
    imgSize.scale(viewSize, Qt::KeepAspectRatio);
    const int x = (viewSize.width() - imgSize.width()) / 2;
    const int y = (viewSize.height() - imgSize.height()) / 2;

    glViewport(x, y, imgSize.width(), imgSize.height());

    m_vao.bind();

    switch (m_currentFormat) {
    case FrameFormat::YUV420P: {
        if (!m_texY || !m_texU || !m_texV) break;
        m_yuvProgram.bind();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_texY);
        m_yuvProgram.setUniformValue("u_texY", 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_texU);
        m_yuvProgram.setUniformValue("u_texU", 1);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, m_texV);
        m_yuvProgram.setUniformValue("u_texV", 2);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        m_yuvProgram.release();
        break;
    }
    case FrameFormat::NV12: {
        if (!m_texNV12_Y || !m_texNV12_UV) break;
        m_nv12Program.bind();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_texNV12_Y);
        m_nv12Program.setUniformValue("u_texY", 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_texNV12_UV);
        m_nv12Program.setUniformValue("u_texUV", 1);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        m_nv12Program.release();
        break;
    }
    case FrameFormat::RGBA: {
        if (!m_texRGBA) break;
        m_rgbaProgram.bind();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_texRGBA);
        m_rgbaProgram.setUniformValue("u_tex", 0);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        m_rgbaProgram.release();
        break;
    }
    default: break;
    }

    m_vao.release();
    glViewport(0, 0, viewSize.width(), viewSize.height());
}

void VideoRenderWidget::createTexture(GLuint& tex, int w, int h,
                                      GLenum internalFmt, GLenum fmt,
                                      const void* data)
{
    if (tex) glDeleteTextures(1, &tex);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, internalFmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, data);
}

void VideoRenderWidget::updateTexture(GLuint tex, int w, int h,
                                      GLenum fmt, GLenum type, const void* data)
{
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, fmt, type, data);
}

void VideoRenderWidget::uploadYUV420P(const VideoFrame& frame)
{
    const int w = frame.width();
    const int h = frame.height();
    const uint8_t* raw = frame.data();
    const uint8_t* yData = raw;
    const uint8_t* uData = raw + w * h;
    const uint8_t* vData = uData + (w / 2) * (h / 2);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    if (!m_texY || m_frameWidth != w || m_frameHeight != h) {
        createTexture(m_texY, w, h, GL_RED, GL_RED, yData);
        createTexture(m_texU, w / 2, h / 2, GL_RED, GL_RED, uData);
        createTexture(m_texV, w / 2, h / 2, GL_RED, GL_RED, vData);
    } else {
        updateTexture(m_texY, w, h, GL_RED, GL_UNSIGNED_BYTE, yData);
        updateTexture(m_texU, w / 2, h / 2, GL_RED, GL_UNSIGNED_BYTE, uData);
        updateTexture(m_texV, w / 2, h / 2, GL_RED, GL_UNSIGNED_BYTE, vData);
    }
}

void VideoRenderWidget::uploadNV12(const VideoFrame& frame)
{
    const int w = frame.width();
    const int h = frame.height();
    const uint8_t* raw = frame.data();
    const uint8_t* yData = raw;
    const uint8_t* uvData = raw + w * h;

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    if (!m_texNV12_Y || m_frameWidth != w || m_frameHeight != h) {
        createTexture(m_texNV12_Y, w, h, GL_RED, GL_RED, yData);
        createTexture(m_texNV12_UV, w / 2, h / 2, GL_RG, GL_RG, uvData);
    } else {
        updateTexture(m_texNV12_Y, w, h, GL_RED, GL_UNSIGNED_BYTE, yData);
        updateTexture(m_texNV12_UV, w / 2, h / 2, GL_RG, GL_UNSIGNED_BYTE, uvData);
    }
}

void VideoRenderWidget::uploadRGBA(const VideoFrame& frame)
{
    const int w = frame.width();
    const int h = frame.height();
    const uint8_t* raw = frame.data();

    GLenum fmt = (frame.format() == SP_FMT_BGRA) ? GL_BGRA : GL_RGBA;

    if (!m_texRGBA || m_frameWidth != w || m_frameHeight != h) {
        createTexture(m_texRGBA, w, h, GL_RGBA, fmt, raw);
    } else {
        glBindTexture(GL_TEXTURE_2D, m_texRGBA);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, fmt, GL_UNSIGNED_BYTE, raw);
    }
}

void VideoRenderWidget::uploadQImage(const QImage& frame)
{
    QImage img = frame.convertToFormat(QImage::Format_RGBA8888);
    const int w = img.width();
    const int h = img.height();

    if (!m_texRGBA || m_frameWidth != w || m_frameHeight != h) {
        createTexture(m_texRGBA, w, h, GL_RGBA, GL_RGBA, img.constBits());
    } else {
        glBindTexture(GL_TEXTURE_2D, m_texRGBA);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, img.constBits());
    }
}

void VideoRenderWidget::destroyTextures()
{
    if (m_texY) { glDeleteTextures(1, &m_texY); m_texY = 0; }
    if (m_texU) { glDeleteTextures(1, &m_texU); m_texU = 0; }
    if (m_texV) { glDeleteTextures(1, &m_texV); m_texV = 0; }
    if (m_texNV12_Y) { glDeleteTextures(1, &m_texNV12_Y); m_texNV12_Y = 0; }
    if (m_texNV12_UV) { glDeleteTextures(1, &m_texNV12_UV); m_texNV12_UV = 0; }
    if (m_texRGBA) { glDeleteTextures(1, &m_texRGBA); m_texRGBA = 0; }
}

// ===================================================================
//  RoundedVideoContainer
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
    QPainterPath path;
    path.addRoundedRect(rect(), m_radius, m_radius);
    setMask(QRegion(path.toFillPolygon().toPolygon()));
}

void RoundedVideoContainer::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath path;
    path.addRoundedRect(QRectF(rect()), m_radius, m_radius);
    painter.fillPath(path, QColor("#1A1A1A"));
}
