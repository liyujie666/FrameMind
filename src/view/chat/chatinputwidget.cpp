#include "view/chat/chatinputwidget.h"

#include <QTextEdit>
#include <QToolButton>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QKeyEvent>

ChatInputWidget::ChatInputWidget(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    // 快捷操作行
    auto* topRow = new QHBoxLayout();
    m_frameBtn = new QToolButton(this);
    m_frameBtn->setText(QStringLiteral("📷 当前帧"));
    m_frameBtn->setCheckable(true);
    m_frameBtn->setToolTip(tr("附带当前播放画面一起提问"));
    topRow->addWidget(m_frameBtn);
    topRow->addStretch(1);
    layout->addLayout(topRow);

    // 输入框
    m_edit = new QTextEdit(this);
    m_edit->setPlaceholderText(tr("输入问题…（Enter 发送，Shift+Enter 换行）"));
    m_edit->setFixedHeight(80);
    m_edit->installEventFilter(this);
    layout->addWidget(m_edit);

    // 发送/停止
    auto* bottomRow = new QHBoxLayout();
    bottomRow->addStretch(1);
    m_sendBtn = new QPushButton(tr("发送"), this);
    m_sendBtn->setMinimumWidth(80);
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
