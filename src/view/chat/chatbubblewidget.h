#ifndef FRAMEMIND_CHATBUBBLEWIDGET_H
#define FRAMEMIND_CHATBUBBLEWIDGET_H

#include <QFrame>
#include <QColor>

#include "model/chatmessage.h"

class QTextBrowser;
class QLabel;
class QHBoxLayout;
class QVBoxLayout;
class QToolButton;
class ThemeService;
class MarkdownRenderer;

/**
 * 单条消息气泡（增强版）。
 * 
 * 结构：
 * ├── 附件缩略图
 * ├── 消息头部：角色图标、时间戳
 * ├── QTextBrowser（富文本内容，HTML 渲染）
 * └── 操作栏：复制、重新生成、跳转（鼠标悬停显示）
 *
 * 根据 role 渲染左右不同背景；内容使用 Markdown → HTML 转换后显示。
 * 性能优化：使用 paintEvent 自绘圆角背景，避免 setStyleSheet 开销。
 */
class ChatBubbleWidget : public QFrame {
    Q_OBJECT
public:
    explicit ChatBubbleWidget(QWidget* parent = nullptr);

    void setMessage(const ChatMessage& msg);
    void updateContent(const QString& markdown);
    void refreshLayout();
    void setThemeService(ThemeService* theme);
    void setMarkdownRenderer(MarkdownRenderer* renderer);
    void refreshColors();

    ChatMessage::Role role() const { return m_role; }
    QString messageId() const { return m_messageId; }

signals:
    void linkActivated(const QString& href);
    void copyRequested(const QString& content);
    void regenerateRequested();

protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void updateColors();
    void updateHtml();
    void createActionBar();

    ChatMessage::Role m_role = ChatMessage::Assistant;
    QString           m_messageId;
    QString           m_markdownContent;
    QDateTime         m_timestamp;

    QVBoxLayout*      m_mainLayout = nullptr;
    QWidget*          m_header = nullptr;
    QLabel*           m_timeLabel = nullptr;
    QWidget*          m_thumbs = nullptr;
    QHBoxLayout*      m_thumbsLayout = nullptr;
    QTextBrowser*     m_content = nullptr;
    QWidget*          m_actionBar = nullptr;
    QToolButton*      m_copyButton = nullptr;
    QToolButton*      m_regenerateButton = nullptr;

    ThemeService*     m_theme = nullptr;
    MarkdownRenderer* m_renderer = nullptr;

    // 缓存颜色，避免每次 paintEvent 查询
    QColor m_bgColor;
    QColor m_textColor;
    QColor m_borderColor;
};

#endif // FRAMEMIND_CHATBUBBLEWIDGET_H
