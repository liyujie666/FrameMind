#include "view/chat/chatbubblewidget.h"

#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPixmap>

ChatBubbleWidget::ChatBubbleWidget(QWidget* parent)
    : QFrame(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 8, 12, 8);
    layout->setSpacing(6);

    // 帧缩略图横排
    m_thumbs = new QWidget(this);
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
    connect(m_content, &QLabel::linkActivated,
            this, &ChatBubbleWidget::linkActivated);
    layout->addWidget(m_content);

    setMaximumWidth(560);
    applyStyle();
}

void ChatBubbleWidget::applyStyle()
{
    if (m_role == ChatMessage::User) {
        setStyleSheet(QStringLiteral(
            "QFrame { background:#2979FF; border-radius:12px; }"
            "QLabel { color:#FFFFFF; background:transparent; }"));
    } else {
        setStyleSheet(QStringLiteral(
            "QFrame { background:#252536; border-radius:12px; }"
            "QLabel { color:#E0E0E0; background:transparent; }"));
    }
}

void ChatBubbleWidget::setMessage(const ChatMessage& msg)
{
    m_role = msg.role;
    applyStyle();

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
            thumb->setStyleSheet(QStringLiteral("border-radius:4px;"));
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
    // 流式初期可能为空，给一个占位避免气泡塌陷
    m_content->setText(markdown.isEmpty() ? QStringLiteral("…") : markdown);
}
