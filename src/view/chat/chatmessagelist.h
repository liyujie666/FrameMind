#ifndef FRAMEMIND_CHATMESSAGELIST_H
#define FRAMEMIND_CHATMESSAGELIST_H

#include <QScrollArea>
#include <QList>

class QVBoxLayout;
class ChatBubbleWidget;
class ChatMessageListModel;
class ThemeService;
class MarkdownRenderer;

/**
 * 聊天消息列表视图（增强版）。
 * 使用 QScrollArea + 垂直布局管理气泡 widget。
 * 数据从 ChatMessageListModel 读取；流式更新时通过 model 信号刷新。
 * 支持 Markdown → HTML 渲染。
 */
class ChatMessageList : public QScrollArea {
    Q_OBJECT
public:
    explicit ChatMessageList(QWidget* parent = nullptr);

    void setModel(ChatMessageListModel* model);
    void setThemeService(ThemeService* theme);
    void setMarkdownRenderer(MarkdownRenderer* renderer);
    void refreshBubbleColors();

signals:
    void linkActivated(const QString& href);
    void copyRequested(const QString& content);
    void regenerateRequested();

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void rebuildAll();
    void appendRow(int row);
    void updateRow(int row);
    bool isAtBottom() const;
    void scrollToBottom();
    void updateElapsedTime(int row);

    QWidget*                   m_container = nullptr;
    QVBoxLayout*               m_layout = nullptr;
    ChatMessageListModel*      m_model = nullptr;
    ThemeService*              m_theme = nullptr;
    MarkdownRenderer*          m_renderer = nullptr;
    QList<ChatBubbleWidget*>   m_bubbles;
    QMap<int, QTimer*>         m_elapsedTimers;  // 每行的计时器
};

#endif // FRAMEMIND_CHATMESSAGELIST_H
