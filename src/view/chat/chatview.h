#ifndef FRAMEMIND_CHATVIEW_H
#define FRAMEMIND_CHATVIEW_H

#include <QWidget>
#include <QColor>

class ChatMessageList;
class ChatInputWidget;
class ChatViewModel;
class ThemeService;
class QToolButton;
class QLabel;

/**
 * AI 对话面板：顶部标题栏 + 消息列表 + 输入区。
 * 整体是一个圆角卡片（配合外层 layout 的留白），随主题切换配色。
 * 头部：左「AI Chat」标题 + 会话下拉小箭头，右「+ 新建」、「× 折叠」。
 */
class ChatView : public QWidget {
    Q_OBJECT
public:
    explicit ChatView(QWidget* parent = nullptr);

    void setViewModel(ChatViewModel* vm);
    void setThemeService(ThemeService* theme);
    void addFramePreview(const QImage& frame, int64_t timestampMs);

signals:
    /// 用户点击 × 关闭 / 折叠对话面板
    void collapseRequested();
    /// 用户请求添加当前帧
    void currentFrameRequested();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void refreshHeader();
    void showConversationMenu();
    void onLinkActivated(const QString& href);
    void applyThemeColors();
    void onThemeChanged();

    ChatMessageList* m_messageList = nullptr;
    ChatInputWidget* m_inputWidget = nullptr;
    QWidget*         m_header = nullptr;
    QToolButton*     m_convButton = nullptr;
    QToolButton*     m_newButton = nullptr;
    QLabel*          m_titleLabel = nullptr;
    ChatViewModel*   m_vm = nullptr;
    ThemeService*    m_theme = nullptr;

    // 主题相关颜色（缓存）
    QColor m_bgColor;
    QColor m_borderColor;
    QColor m_headerDividerColor;
    QColor m_textPrimary;
    QColor m_textSecondary;
    QColor m_surfaceVariant;
    QColor m_primary;
};

#endif // FRAMEMIND_CHATVIEW_H
