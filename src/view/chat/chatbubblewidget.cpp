#include "view/chat/chatbubblewidget.h"

#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPixmap>
#include <QPainter>
#include <QPainterPath>

#include "service/themeservice.h"

ChatBubbleWidget::ChatBubbleWidget(QWidget* parent)
    : QFrame(parent)
{
    // 禁用 QFrame 默认绘制，完全自绘
    setFrameShape(QFrame::NoFrame);
    setAttribute(Qt::WA_StyledBackground, false);
    setAutoFillBackground(false);
    // 去掉继承的全局 QSS 影响
    setStyleSheet(QStringLiteral("ChatBubbleWidget { background:transparent; border:none; }"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 8, 12, 8);
    layout->setSpacing(6);

    // 帧缩略图横排
    m_thumbs = new QWidget(this);
    m_thumbs->setAttribute(Qt::WA_StyledBackground, false);
    m_thumbsLayout = new QHBoxLayout(m_thumbs);
    m_thumbsLayout->setContentsMargins(0, 0, 0, 0);
    m_thumbsLayout->setSpacing(6);
    m_thumbs->setVisible(false);
    layout->addWidget(m_thumbs);

    // 内容（Markdown）
    m_content = new QLabel(this);
    m_content->setTextFormat(Qt::MarkdownText);
    m_content->setWordWrap(true);
    m_content->setTextInteractionFlags(Qt::TextSelectableByMouse
                                       | Qt::LinksAccessibleByMouse);
    m_content->setOpenExternalLinks(false);
    // 使用透明背景，让自绘的圆角底色透出来
    m_content->setStyleSheet(QStringLiteral("QLabel { background:transparent; border:none; }"));
    connect(m_content, &QLabel::linkActivated,
            this, &ChatBubbleWidget::linkActivated);
    layout->addWidget(m_content);

    setMaximumWidth(560);

    // 初始默认颜色（暗色方案）
    m_bgColor = QColor("#252536");
    m_textColor = QColor("#E0E0E0");
    updateColors();
}

void ChatBubbleWidget::setThemeService(ThemeService* theme)
{
    m_theme = theme;
    updateColors();
}

void ChatBubbleWidget::refreshColors()
{
    updateColors();
    update();
}

void ChatBubbleWidget::updateColors()
{
    if (m_theme) {
        if (m_role == ChatMessage::User) {
            m_bgColor = m_theme->color(QStringLiteral("userBubble"));
            m_textColor = m_theme->color(QStringLiteral("userBubbleText"));
        } else {
            m_bgColor = m_theme->color(QStringLiteral("aiBubble"));
            m_textColor = m_theme->color(QStringLiteral("aiBubbleText"));
        }
    } else {
        if (m_role == ChatMessage::User) {
            m_bgColor = QColor("#2979FF");
            m_textColor = QColor("#FFFFFF");
        } else {
            m_bgColor = QColor("#252536");
            m_textColor = QColor("#E0E0E0");
        }
    }

    // 仅更新文字颜色（通过 QPalette，不触发样式重算）
    QPalette pal = m_content->palette();
    pal.setColor(QPalette::WindowText, m_textColor);
    pal.setColor(QPalette::Text, m_textColor);
    m_content->setPalette(pal);

    // 链接颜色
    const QColor linkColor = m_role == ChatMessage::User
        ? QColor("#B3D9FF")   // 亮蓝对比白色背景
        : (m_theme ? m_theme->color(QStringLiteral("primary")) : QColor("#2979FF"));
    QPalette linkPal = m_content->palette();
    linkPal.setColor(QPalette::Link, linkColor);
    m_content->setPalette(linkPal);
}

void ChatBubbleWidget::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    QPainterPath path;
    path.addRoundedRect(QRectF(rect()), 12, 12);
    painter.fillPath(path, m_bgColor);
}

void ChatBubbleWidget::setMessage(const ChatMessage& msg)
{
    m_role = msg.role;
    updateColors();

    // 缩略图
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
    m_content->setText(markdown.isEmpty() ? QStringLiteral("…") : markdown);
}
