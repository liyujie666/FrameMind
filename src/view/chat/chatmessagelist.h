#ifndef FRAMEMIND_CHATMESSAGELIST_H
#define FRAMEMIND_CHATMESSAGELIST_H

#include <QScrollArea>
#include <QList>

class ChatMessageListModel;
class ChatBubbleWidget;
class ThemeService;
class QVBoxLayout;
class QWidget;

/**
 * 可滚动消息列表（M2-T6）。模型驱动：监听 ChatMessageListModel 的
 * rowsInserted / dataChanged / modelReset 维护气泡 widget。
 * 仅在已贴底时自动滚动到底部。
 */
class ChatMessageList : public QScrollArea {
    Q_OBJECT
public:
    explicit ChatMessageList(QWidget* parent = nullptr);

    void setModel(ChatMessageListModel* model);
    void setThemeService(ThemeService* theme);

    /// 主题变更时批量刷新所有气泡颜色（不重建 widget）
    void refreshBubbleColors();

signals:
    void linkActivated(const QString& href);

private:
    void rebuildAll();
    void appendRow(int row);
    void updateRow(int row);
    bool isAtBottom() const;
    void scrollToBottom();
    void applyBackgroundColor();

    ChatMessageListModel*     m_model = nullptr;
    ThemeService*             m_theme = nullptr;
    QWidget*                  m_container = nullptr;
    QVBoxLayout*              m_layout = nullptr;
    QList<ChatBubbleWidget*>  m_bubbles;
};

#endif // FRAMEMIND_CHATMESSAGELIST_H
