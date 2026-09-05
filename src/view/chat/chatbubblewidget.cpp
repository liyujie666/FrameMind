#include "view/chat/chatbubblewidget.h"

#include <QTextBrowser>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QToolButton>
#include <QPixmap>
#include <QPainter>
#include <QPainterPath>
#include <QClipboard>
#include <QGuiApplication>
#include <QDateTime>
#include <QEnterEvent>
#include <QTextOption>
#include <QTimer>
#include <QResizeEvent>
#include <QMovie>

#include "service/themeservice.h"
#include "service/markdownrenderer.h"

ChatBubbleWidget::ChatBubbleWidget(QWidget* parent)
    : QFrame(parent)
{
    setFrameShape(QFrame::NoFrame);
    setAttribute(Qt::WA_StyledBackground, false);
    setAutoFillBackground(false);

    // 气泡宽度自适应内容，最小200px
    // 不设置最大宽度，由父容器的布局控制
    setMinimumWidth(200);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);

    m_mainLayout = new QVBoxLayout(this);
    m_mainLayout->setContentsMargins(12, 10, 12, 10);
    m_mainLayout->setSpacing(8);

    // 帧缩略图横排
    m_thumbs = new QWidget(this);
    m_thumbs->setAttribute(Qt::WA_StyledBackground, false);
    m_thumbs->setStyleSheet(QStringLiteral("QWidget { background:transparent; }"));
    m_thumbsLayout = new QHBoxLayout(m_thumbs);
    m_thumbsLayout->setContentsMargins(0, 0, 0, 0);
    m_thumbsLayout->setSpacing(6);
    m_thumbs->setVisible(false);
    m_mainLayout->addWidget(m_thumbs);

    // 内容（HTML 富文本）
    m_content = new QTextBrowser(this);
    m_content->setFrameShape(QFrame::NoFrame);
    m_content->setOpenExternalLinks(false);
    m_content->setReadOnly(true);
    m_content->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_content->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_content->setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);
    m_content->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
    m_content->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    m_content->setStyleSheet(QStringLiteral(
        "QTextBrowser { background:transparent; border:none; padding:0; }"));
    connect(m_content, &QTextBrowser::anchorClicked,
            this, [this](const QUrl& url) {
                emit linkActivated(url.toString());
            });
    m_mainLayout->addWidget(m_content);

    // 不再在这里创建 header 和 actionBar

    // 初始默认颜色（暗色方案）
    m_bgColor = QColor("#252536");
    m_textColor = QColor("#E0E0E0");
    m_borderColor = QColor("#2D2D3D");
    updateColors();
}

void ChatBubbleWidget::createActionBar()
{
    m_actionBar = new QWidget(this);
    m_actionBar->setAttribute(Qt::WA_StyledBackground, false);
    m_actionBar->setStyleSheet(QStringLiteral("QWidget { background:transparent; }"));
    auto* actionLayout = new QHBoxLayout(m_actionBar);
    actionLayout->setContentsMargins(0, 8, 0, 0);
    actionLayout->setSpacing(8);

    // 复制按钮
    m_copyButton = new QToolButton(m_actionBar);
    m_copyButton->setText(QStringLiteral("📋"));
    m_copyButton->setToolTip(tr("复制"));
    m_copyButton->setCursor(Qt::PointingHandCursor);
    m_copyButton->setFixedSize(24, 24);
    m_copyButton->setStyleSheet(QStringLiteral(
        "QToolButton { background:transparent; border:1px solid #3A3A4A; "
        "color:#E0E0E0; border-radius:4px; font-size:14px; }"
        "QToolButton:hover { background:#2A2A3A; }"));
    connect(m_copyButton, &QToolButton::clicked, this, [this]() {
        QGuiApplication::clipboard()->setText(m_markdownContent);
        emit copyRequested(m_markdownContent);
    });
    actionLayout->addWidget(m_copyButton);

    // 重新生成按钮
    m_regenerateButton = new QToolButton(m_actionBar);
    m_regenerateButton->setText(QStringLiteral("🔄"));
    m_regenerateButton->setToolTip(tr("重新生成"));
    m_regenerateButton->setCursor(Qt::PointingHandCursor);
    m_regenerateButton->setFixedSize(24, 24);
    m_regenerateButton->setStyleSheet(QStringLiteral(
        "QToolButton { background:transparent; border:1px solid #3A3A4A; "
        "color:#E0E0E0; border-radius:4px; font-size:14px; }"
        "QToolButton:hover { background:#2A2A3A; }"));
    connect(m_regenerateButton, &QToolButton::clicked,
            this, &ChatBubbleWidget::regenerateRequested);
    actionLayout->addWidget(m_regenerateButton);

    actionLayout->addStretch(1);

    m_actionBar->setVisible(true);
    m_mainLayout->addWidget(m_actionBar);
}

void ChatBubbleWidget::setThemeService(ThemeService* theme)
{
    m_theme = theme;
    updateColors();
    updateHtml();
}

void ChatBubbleWidget::setMarkdownRenderer(MarkdownRenderer* renderer)
{
    m_renderer = renderer;
}

void ChatBubbleWidget::refreshColors()
{
    updateColors();
    updateHtml();
    update();
}

void ChatBubbleWidget::updateColors()
{
    if (m_theme) {
        if (m_role == ChatMessage::User) {
            m_bgColor = m_theme->color(QStringLiteral("userBubble"));
            m_textColor = m_theme->color(QStringLiteral("userBubbleText"));
            m_borderColor = m_theme->color(QStringLiteral("userBubble")).darker(110);
        } else {
            m_bgColor = m_theme->color(QStringLiteral("aiBubble"));
            m_textColor = m_theme->color(QStringLiteral("aiBubbleText"));
            m_borderColor = m_theme->color(QStringLiteral("border"));
        }
    } else {
        if (m_role == ChatMessage::User) {
            m_bgColor = QColor("#2979FF");
            m_textColor = QColor("#FFFFFF");
            m_borderColor = QColor("#1565C0");
        } else {
            m_bgColor = QColor("#252536");
            m_textColor = QColor("#E0E0E0");
            m_borderColor = QColor("#2D2D3D");
        }
    }
}

void ChatBubbleWidget::updateHtml()
{
    if (!m_renderer) return;

    const bool isDark = !m_theme || m_theme->isDark();
    const QString html = m_renderer->toHtml(m_markdownContent, isDark);
    m_content->setHtml(html);
    
    // 计算内容的理想宽度
    QTextDocument* doc = m_content->document();
    
    // 先不设置宽度限制，让文档计算理想尺寸（单行宽度）
    doc->setTextWidth(-1);
    const qreal idealWidth = doc->idealWidth();
    
    const int maxAllowedWidth = qMax(200, maximumWidth() - 24);
    const int minWidth = 200;

    int targetWidth;
    if (m_role == ChatMessage::Assistant) {
        // AI 回复应保持稳定的阅读宽度，避免流式 Markdown 的 idealWidth
        // 随内容变化导致气泡在生成过程中突然收缩。
        targetWidth = maxAllowedWidth;
    } else {
        // 用户消息保留紧凑的内容自适应宽度。
        const bool needsMultiLine = idealWidth > maxAllowedWidth;
        targetWidth = needsMultiLine
            ? maxAllowedWidth
            : qBound(minWidth, static_cast<int>(idealWidth) + 20, maxAllowedWidth);
    }

    doc->setTextWidth(targetWidth);
    
    // 调整高度以适应内容
    const int docHeight = doc->size().toSize().height();
    m_content->setFixedHeight(docHeight + 10);
    m_content->setMinimumWidth(targetWidth);
    m_content->setMaximumWidth(targetWidth);
    
    // 设置气泡的首选宽度（加上 padding）
    // 使用 setMinimumWidth 和 setMaximumWidth 而不是 setFixedWidth
    // 这样当容器宽度不足时，气泡会自动收缩
    const int bubbleWidth = targetWidth + 24;
    setMinimumWidth(qMin(bubbleWidth, minWidth + 24));
    setMaximumWidth(bubbleWidth);
    
    updateGeometry();
}

void ChatBubbleWidget::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // 绘制整个气泡的背景
    QPainterPath path;
    const qreal radius = m_role == ChatMessage::User ? 16.0 : 12.0;
    path.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    painter.fillPath(path, m_bgColor);

    // 绘制边框（仅 AI 消息）
    if (m_role == ChatMessage::Assistant) {
        QPen pen(m_borderColor);
        pen.setWidth(1);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
    }
}

void ChatBubbleWidget::enterEvent(QEnterEvent* event)
{
    QFrame::enterEvent(event);
    // 操作栏始终显示，不需要悬停逻辑
}

void ChatBubbleWidget::leaveEvent(QEvent* event)
{
    QFrame::leaveEvent(event);
    // 操作栏始终显示，不需要悬停逻辑
}

void ChatBubbleWidget::resizeEvent(QResizeEvent* event)
{
    QFrame::resizeEvent(event);
    
    // 当气泡宽度被父容器限制时，需要重新计算文本布局
    if (m_content && event->size().width() != event->oldSize().width()) {
        QTimer::singleShot(0, this, [this]() {
            if (m_content && m_content->document()) {
                // 根据当前实际宽度重新布局文本
                const int availableWidth = qMax(200, width() - 24);
                m_content->document()->setTextWidth(availableWidth);
                const int docHeight = m_content->document()->size().toSize().height();
                m_content->setFixedHeight(docHeight + 10);
            }
        });
    }
}

void ChatBubbleWidget::setMessage(const ChatMessage& msg)
{
    m_role = msg.role;
    m_messageId = msg.id;
    m_timestamp = msg.timestamp;
    m_markdownContent = msg.content;
    
    updateColors();

    // 更新缩略图
    QLayoutItem* item = nullptr;
    while ((item = m_thumbsLayout->takeAt(0)) != nullptr) {
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }
    if (!msg.attachedFrames.isEmpty()) {
        for (const QImage& img : msg.attachedFrames) {
            auto* thumb = new QLabel(m_thumbs);
            thumb->setPixmap(QPixmap::fromImage(img).scaled(
                96, 54, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            thumb->setStyleSheet(QStringLiteral("border-radius:4px; background:transparent;"));
            m_thumbsLayout->addWidget(thumb);
        }
        m_thumbsLayout->addStretch(1);
        m_thumbs->setVisible(true);
    } else {
        m_thumbs->setVisible(false);
    }

    updateContent(msg.content);
}

void ChatBubbleWidget::updateContent(const QString& markdown)
{
    m_markdownContent = markdown;
    updateHtml();
}

void ChatBubbleWidget::refreshLayout()
{
    updateHtml();
}
