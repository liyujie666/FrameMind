#include "view/chat/chatinputwidget.h"

#include <QTextEdit>
#include <QToolButton>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QKeyEvent>

#include "service/themeservice.h"

ChatInputWidget::ChatInputWidget(QWidget* parent)
    : QWidget(parent)
    , m_streaming(false)
{
    // 背景透明，跟随外层 ChatView 圆角卡片底色
    setAutoFillBackground(false);
    setAttribute(Qt::WA_StyledBackground, false);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(8);

    // 快捷操作行
    auto* topRow = new QHBoxLayout();
    topRow->setSpacing(8);

    m_frameBtn = new QToolButton(this);
    m_frameBtn->setText(QStringLiteral("[📷] 当前帧"));
    m_frameBtn->setCheckable(true);
    m_frameBtn->setToolTip(tr("附带当前播放画面一起提问"));
    topRow->addWidget(m_frameBtn);

    topRow->addStretch(1);
    layout->addLayout(topRow);

    // 输入框
    m_edit = new QTextEdit(this);
    m_edit->setPlaceholderText(tr("输入问题…（Enter 发送，Shift+Enter 换行）"));
    m_edit->setFixedHeight(72);
    m_edit->installEventFilter(this);
    layout->addWidget(m_edit);

    // 发送/停止
    auto* bottomRow = new QHBoxLayout();
    bottomRow->addStretch(1);

    m_sendBtn = new QPushButton(tr("发送"), this);
    m_sendBtn->setMinimumWidth(72);
    m_sendBtn->setCursor(Qt::PointingHandCursor);
    connect(m_sendBtn, &QPushButton::clicked, this, [this]() {
        if (m_streaming) {
            emit stopRequested();
        } else {
            triggerSend();
        }
    });
    bottomRow->addWidget(m_sendBtn);
    layout->addLayout(bottomRow);

    // 应用默认颜色
    applyColors();
}

void ChatInputWidget::setThemeService(ThemeService* theme)
{
    m_theme = theme;
    applyColors();
}

void ChatInputWidget::applyColors()
{
    // 从 ThemeService 获取颜色，若无则使用暗色默认值
    const QColor border = m_theme
        ? m_theme->color(QStringLiteral("border"))
        : QColor("#2D2D3D");
    const QColor surface = m_theme
        ? m_theme->color(QStringLiteral("surfaceVariant"))
        : QColor("#252538");
    const QColor textSecondary = m_theme
        ? m_theme->color(QStringLiteral("textSecondary"))
        : QColor("#8B8B8B");
    const QColor textPrimary = m_theme
        ? m_theme->color(QStringLiteral("textPrimary"))
        : QColor("#E0E0E0");
    const QColor primary = m_theme
        ? m_theme->color(QStringLiteral("primary"))
        : QColor("#2979FF");
    const QColor primaryHover = m_theme
        ? m_theme->color(QStringLiteral("primaryHover"))
        : QColor("#448AFF");
    const QColor primaryPressed = m_theme
        ? m_theme->color(QStringLiteral("primaryPressed"))
        : QColor("#1565C0");
    const QColor inputBg = m_theme
        ? m_theme->color(QStringLiteral("inputBg"))
        : QColor("#1A1A2A");

    m_frameBtn->setStyleSheet(QString(
        "QToolButton { border:1px solid %1; background:%2; color:%3; "
        "padding:4px 10px; border-radius:14px; font-size:12px; }"
        "QToolButton:hover { border-color:%4; color:%5; }"
        "QToolButton:checked { border-color:%4; background:%4; color:#FFFFFF; }")
        .arg(border.name(), surface.name(), textSecondary.name(),
             primary.name(), textPrimary.name()));

    m_edit->setStyleSheet(QString(
        "QTextEdit { background:%1; border:1px solid %2; border-radius:8px; "
        "color:%3; padding:8px 10px; font-size:13px; selection-background-color:%4; }"
        "QTextEdit:focus { border-color:%4; }"
        "QTextEdit:disabled { background:%5; color:%6; }")
        .arg(inputBg.name(), border.name(), textPrimary.name(),
             primary.name(), surface.name(), textSecondary.name()));

    m_sendBtn->setStyleSheet(QString(
        "QPushButton { background:%1; color:#FFFFFF; border:none; border-radius:6px; "
        "padding:8px 20px; font-size:13px; font-weight:500; }"
        "QPushButton:hover { background:%2; }"
        "QPushButton:pressed { background:%3; }"
        "QPushButton:disabled { background:%4; color:#8B8B8B; }")
        .arg(primary.name(), primaryHover.name(), primaryPressed.name(),
             primary.name() + "66"));
}

void ChatInputWidget::setStreaming(bool streaming)
{
    m_streaming = streaming;
    m_sendBtn->setText(streaming ? tr("停止") : tr("发送"));
    m_edit->setEnabled(!streaming);
    m_frameBtn->setEnabled(!streaming);
}

void ChatInputWidget::triggerSend()
{
    const QString text = m_edit->toPlainText().trimmed();
    const bool withFrame = m_frameBtn->isChecked();
    if (text.isEmpty() && !withFrame) return;
    emit sendRequested(text, withFrame);
    m_edit->clear();
    m_frameBtn->setChecked(false);
}

bool ChatInputWidget::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == m_edit && event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if ((ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter)
            && !(ke->modifiers() & Qt::ShiftModifier)) {
            if (!m_streaming) triggerSend();
            return true;  // 拦截换行
        }
    }
    return QWidget::eventFilter(obj, event);
}
