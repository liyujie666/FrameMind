#include "view/chat/chatmessagelist.h"

#include "view/chat/chatbubblewidget.h"
#include "viewmodel/chatmessagelistmodel.h"
#include "service/themeservice.h"

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
    setBackgroundRole(QPalette::NoRole);

    m_container = new QWidget(this);
    m_container->setObjectName(QStringLiteral("chatMessageContainer"));
    m_container->setAutoFillBackground(false);
    m_container->setAttribute(Qt::WA_StyledBackground, false);
    m_layout = new QVBoxLayout(m_container);
    m_layout->setContentsMargins(16, 16, 16, 16);
    m_layout->setSpacing(12);
    m_layout->addStretch(1);   // 末尾弹簧，气泡插入其前
    setWidget(m_container);
    
    // 初始应用默认背景色
    applyBackgroundColor();
}

void ChatMessageList::setThemeService(ThemeService* theme)
{
    m_theme = theme;
    applyBackgroundColor();
}

void ChatMessageList::applyBackgroundColor()
{
    // 根据主题设置不同的背景色
    QString bgColor;
    if (m_theme) {
        if (m_theme->isDark()) {
            bgColor = "#0d1117";  // 暗色模式
        } else {
            bgColor = "#f5f5f5";  // 亮色模式
        }
    } else {
        bgColor = "#0d1117";  // 默认暗色
    }
    
    // 获取滚动条样式（从 ThemeService）
    const QColor scrollThumb = m_theme
        ? m_theme->color(QStringLiteral("scrollThumb"))
        : QColor("#3A3A4A");
    const QString thumbHover = scrollThumb.lighter(130).name();
    const QString thumbPressed = scrollThumb.lighter(160).name();
    
    // 设置背景色的同时保留滚动条样式
    setStyleSheet(QString(
        "QScrollArea { background:%1; border:none; }"
        "QWidget#chatMessageContainer { background:%1; }"
        "QScrollBar:vertical {"
        "    background:transparent;"
        "    width:10px;"
        "    margin:4px 2px 4px 2px;"
        "    border:none;"
        "}"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background:transparent; }"
        "QScrollBar::handle:vertical {"
        "    background:%2;"
        "    border-radius:5px;"
        "    min-height:40px;"
        "    margin:0 2px;"
        "}"
        "QScrollBar::handle:vertical:hover { background:%3; }"
        "QScrollBar::handle:vertical:pressed { background:%4; }")
        .arg(bgColor, scrollThumb.name(), thumbHover, thumbPressed));
}

void ChatMessageList::refreshBubbleColors()
{
    // 批量刷新：暂停布局更新以减少重绘次数
    m_container->setUpdatesEnabled(false);
    for (ChatBubbleWidget* bubble : std::as_const(m_bubbles)) {
        bubble->setThemeService(m_theme);
        bubble->refreshColors();
    }
    m_container->setUpdatesEnabled(true);
    m_container->update();
    
    // 刷新背景色
    applyBackgroundColor();
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
    wrapper->setAttribute(Qt::WA_StyledBackground, false);
    auto* h = new QHBoxLayout(wrapper);
    h->setContentsMargins(0, 0, 0, 0);

    auto* bubble = new ChatBubbleWidget(wrapper);
    bubble->setThemeService(m_theme);
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
