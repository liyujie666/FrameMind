#include "view/chat/chatmessagelist.h"

#include "view/chat/chatbubblewidget.h"
#include "viewmodel/chatmessagelistmodel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollBar>
#include <QTimer>
#include <utility>

ChatMessageList::ChatMessageList(QWidget* parent)
    : QScrollArea(parent)
{
    setWidgetResizable(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setFrameShape(QFrame::NoFrame);

    m_container = new QWidget(this);
    m_layout = new QVBoxLayout(m_container);
    m_layout->setContentsMargins(12, 12, 12, 12);
    m_layout->setSpacing(10);
    m_layout->addStretch(1);   // 末尾弹簧，气泡插入其前
    setWidget(m_container);
}

void ChatMessageList::setModel(ChatMessageListModel* model)
{
    if (m_model) m_model->disconnect(this);
    m_model = model;
    if (!m_model) return;

    connect(m_model, &QAbstractItemModel::modelReset,
            this, &ChatMessageList::rebuildAll);
    connect(m_model, &QAbstractItemModel::rowsInserted, this,
            [this](const QModelIndex&, int first, int last) {
                for (int r = first; r <= last; ++r) appendRow(r);
            });
    connect(m_model, &QAbstractItemModel::dataChanged, this,
            [this](const QModelIndex& tl, const QModelIndex& br,
                   const QList<int>&) {
                for (int r = tl.row(); r <= br.row(); ++r) updateRow(r);
            });

    rebuildAll();
}

bool ChatMessageList::isAtBottom() const
{
    auto* bar = verticalScrollBar();
    return bar->value() >= bar->maximum() - 8;
}

void ChatMessageList::scrollToBottom()
{
    // 延迟到布局完成后再滚动，确保 maximum 已更新
    QTimer::singleShot(0, this, [this]() {
        verticalScrollBar()->setValue(verticalScrollBar()->maximum());
    });
}

void ChatMessageList::rebuildAll()
{
    // 清空现有气泡（保留末尾 stretch）
    for (ChatBubbleWidget* b : std::as_const(m_bubbles)) {
        if (auto* wrapper = b->parentWidget()) delete wrapper;
    }
    m_bubbles.clear();

    if (!m_model) return;
    for (int r = 0; r < m_model->rowCount(); ++r) appendRow(r);
    scrollToBottom();
}

void ChatMessageList::appendRow(int row)
{
    if (!m_model) return;
    const ChatMessage msg = m_model->messageAt(row);

    auto* wrapper = new QWidget(m_container);
    auto* h = new QHBoxLayout(wrapper);
    h->setContentsMargins(0, 0, 0, 0);

    auto* bubble = new ChatBubbleWidget(wrapper);
    bubble->setMessage(msg);
    connect(bubble, &ChatBubbleWidget::linkActivated,
            this, &ChatMessageList::linkActivated);

    if (msg.role == ChatMessage::User) {
        h->addStretch(1);
        h->addWidget(bubble);
    } else {
        h->addWidget(bubble);
        h->addStretch(1);
    }

    const bool atBottom = isAtBottom();
    // 插入到末尾 stretch 之前
    m_layout->insertWidget(m_layout->count() - 1, wrapper);
    m_bubbles.append(bubble);

    if (atBottom) scrollToBottom();
}

void ChatMessageList::updateRow(int row)
{
    if (row < 0 || row >= m_bubbles.size() || !m_model) return;
    const bool atBottom = isAtBottom();
    m_bubbles[row]->updateContent(m_model->messageAt(row).content);
    if (atBottom) scrollToBottom();
}
