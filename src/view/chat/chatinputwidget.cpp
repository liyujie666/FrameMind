#include "view/chat/chatinputwidget.h"

#include <QTextEdit>
#include <QToolButton>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QKeyEvent>

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
    m_frameBtn->setStyleSheet(QStringLiteral(
        "QToolButton { border:1px solid #2D2D3D; background:#252538; color:#8B8B8B; "
        "padding:4px 10px; border-radius:14px; font-size:12px; }"
        "QToolButton:hover { border-color:#2979FF; color:#E0E0E0; }"
        "QToolButton:checked { border-color:#2979FF; background:#2979FF; color:#FFFFFF; }"));
    topRow->addWidget(m_frameBtn);

    topRow->addStretch(1);
    layout->addLayout(topRow);

    // 输入框
    m_edit = new QTextEdit(this);
    m_edit->setPlaceholderText(tr("输入问题…（Enter 发送，Shift+Enter 换行）"));
    m_edit->setFixedHeight(72);
    m_edit->installEventFilter(this);
    m_edit->setStyleSheet(QStringLiteral(
        "QTextEdit { background:#1A1A2A; border:1px solid #2D2D3D; border-radius:8px; "
        "color:#E0E0E0; padding:8px 10px; font-size:13px; selection-background-color:#2979FF; }"
        "QTextEdit:focus { border-color:#2979FF; }"
        "QTextEdit:disabled { background:#161622; color:#5A5A5A; }"));
    layout->addWidget(m_edit);

    // 发送/停止
    auto* bottomRow = new QHBoxLayout();
    bottomRow->addStretch(1);

    m_sendBtn = new QPushButton(tr("发送"), this);
    m_sendBtn->setMinimumWidth(72);
    m_sendBtn->setCursor(Qt::PointingHandCursor);
    m_sendBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background:#2979FF; color:#FFFFFF; border:none; border-radius:6px; "
        "padding:8px 20px; font-size:13px; font-weight:500; }"
        "QPushButton:hover { background:#448AFF; }"
        "QPushButton:pressed { background:#1565C0; }"
        "QPushButton:disabled { background:#2979FF66; color:#8B8B8B; }"));
    connect(m_sendBtn, &QPushButton::clicked, this, [this]() {
        if (m_streaming) {
            emit stopRequested();
        } else {
            triggerSend();
        }
    });
    bottomRow->addWidget(m_sendBtn);
    layout->addLayout(bottomRow);
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
