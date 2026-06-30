#include "view/chat/chatview.h"

#include "view/chat/chatmessagelist.h"
#include "view/chat/chatinputwidget.h"
#include "viewmodel/chatviewmodel.h"
#include "viewmodel/chatmessagelistmodel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QToolButton>
#include <QLabel>
#include <QMenu>
#include <QRegularExpression>

ChatView::ChatView(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // 顶部 header
    auto* header = new QWidget(this);
    header->setFixedHeight(44);
    auto* hl = new QHBoxLayout(header);
    hl->setContentsMargins(10, 4, 10, 4);

    m_convButton = new QToolButton(header);
    m_convButton->setText(tr("会话"));
    m_convButton->setPopupMode(QToolButton::InstantPopup);
    m_convButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    connect(m_convButton, &QToolButton::clicked,
            this, &ChatView::showConversationMenu);

    m_titleLabel = new QLabel(tr("AI 助手"), header);
    m_titleLabel->setStyleSheet(QStringLiteral("font-weight:600;"));

    m_newButton = new QToolButton(header);
    m_newButton->setText(QStringLiteral("＋"));
    m_newButton->setToolTip(tr("新建对话"));

    hl->addWidget(m_convButton);
    hl->addWidget(m_titleLabel, 1);
    hl->addWidget(m_newButton);
    layout->addWidget(header);

    // 消息列表
    m_messageList = new ChatMessageList(this);
    layout->addWidget(m_messageList, 1);
    connect(m_messageList, &ChatMessageList::linkActivated,
            this, &ChatView::onLinkActivated);

    // 输入区
    m_inputWidget = new ChatInputWidget(this);
    layout->addWidget(m_inputWidget);
}

void ChatView::setViewModel(ChatViewModel* vm)
{
    m_vm = vm;
    if (!m_vm) return;

    m_messageList->setModel(m_vm->messageModel());

    connect(m_inputWidget, &ChatInputWidget::sendRequested, this,
            [this](const QString& text, bool withFrame) {
                if (withFrame) m_vm->sendMessageWithCurrentFrame(text);
                else           m_vm->sendMessage(text);
            });
    connect(m_inputWidget, &ChatInputWidget::stopRequested,
            m_vm, &ChatViewModel::stopGeneration);

    connect(m_vm, &ChatViewModel::streamingChanged,
            m_inputWidget, &ChatInputWidget::setStreaming);
    connect(m_vm, &ChatViewModel::conversationsChanged,
            this, &ChatView::refreshHeader);
    connect(m_vm, &ChatViewModel::conversationChanged,
            this, [this](const QString&) { refreshHeader(); });

    connect(m_newButton, &QToolButton::clicked,
            m_vm, &ChatViewModel::createNewConversation);

    refreshHeader();
}

void ChatView::refreshHeader()
{
    if (!m_vm) return;
    const QString curId = m_vm->currentConversationId();
    QString title = tr("AI 助手");
    const auto convs = m_vm->conversations();
    for (const auto& c : convs) {
        if (c.id == curId) { title = c.title; break; }
    }
    m_titleLabel->setText(title);
}

void ChatView::showConversationMenu()
{
    if (!m_vm) return;
    QMenu menu(this);
    const auto convs = m_vm->conversations();
    const QString curId = m_vm->currentConversationId();

    if (convs.isEmpty()) {
        menu.addAction(tr("（暂无历史会话）"))->setEnabled(false);
    } else {
        for (const auto& c : convs) {
            QAction* act = menu.addAction(c.title.isEmpty() ? tr("新对话") : c.title);
            act->setCheckable(true);
            act->setChecked(c.id == curId);
            const QString id = c.id;
            connect(act, &QAction::triggered, this,
                    [this, id]() { m_vm->switchConversation(id); });
        }
    }
    menu.addSeparator();
    connect(menu.addAction(tr("➕ 新建对话")), &QAction::triggered,
            m_vm, &ChatViewModel::createNewConversation);
    if (!curId.isEmpty()) {
        connect(menu.addAction(tr("🗑 删除当前对话")), &QAction::triggered, this,
                [this, curId]() { m_vm->deleteConversation(curId); });
    }
    menu.exec(m_convButton->mapToGlobal(QPoint(0, m_convButton->height())));
}

void ChatView::onLinkActivated(const QString& href)
{
    // M2：识别 ts://<ms> 形式的时间戳链接（M3-T6 会真正生成此类链接）
    static const QRegularExpression re(QStringLiteral("^ts://(\\d+)$"));
    const auto m = re.match(href);
    if (m.hasMatch() && m_vm) {
        m_vm->onTimestampClicked(m.captured(1).toLongLong());
    }
}
