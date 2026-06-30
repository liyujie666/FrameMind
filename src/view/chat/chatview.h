#ifndef FRAMEMIND_CHATVIEW_H
#define FRAMEMIND_CHATVIEW_H

#include <QWidget>

class ChatMessageList;
class ChatInputWidget;
class ChatViewModel;
class QToolButton;
class QLabel;

/**
 * AI 对话面板（M2-T6/T8）：顶部会话切换 + 消息列表 + 输入区。
 */
class ChatView : public QWidget {
    Q_OBJECT
public:
    explicit ChatView(QWidget* parent = nullptr);

    void setViewModel(ChatViewModel* vm);

private:
    void refreshHeader();
    void showConversationMenu();
    void onLinkActivated(const QString& href);

    ChatMessageList* m_messageList = nullptr;
    ChatInputWidget* m_inputWidget = nullptr;
    QToolButton*     m_convButton = nullptr;
    QToolButton*     m_newButton = nullptr;
    QLabel*          m_titleLabel = nullptr;
    ChatViewModel*   m_vm = nullptr;
};

#endif // FRAMEMIND_CHATVIEW_H
