#ifndef FRAMEMIND_CHATBUBBLEWIDGET_H
#define FRAMEMIND_CHATBUBBLEWIDGET_H

#include <QFrame>
#include <QColor>

#include "model/chatmessage.h"

class QLabel;
class QHBoxLayout;
class ThemeService;

/**
 * 单条消息气泡（M2-T6）。
 * 根据 role 渲染左右不同背景；内容用 QLabel 的 Markdown 文本格式渲染；
 * 顶部横排展示附带帧缩略图。时间戳点击链路在 M3-T6 接入（linkActivated）。
 *
 * 性能优化：使用 paintEvent 自绘圆角背景，避免 setStyleSheet 开销。
 */
class ChatBubbleWidget : public QFrame {
    Q_OBJECT
public:
    explicit ChatBubbleWidget(QWidget* parent = nullptr);

    void setMessage(const ChatMessage& msg);
    void updateContent(const QString& markdown);
    void setThemeService(ThemeService* theme);
    void refreshColors();

    ChatMessage::Role role() const { return m_role; }

signals:
    void linkActivated(const QString& href);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void updateColors();

    ChatMessage::Role m_role = ChatMessage::Assistant;
    QLabel*           m_content = nullptr;
    QWidget*          m_thumbs = nullptr;
    QHBoxLayout*      m_thumbsLayout = nullptr;
    ThemeService*     m_theme = nullptr;

    // 缓存颜色，避免每次 paintEvent 查询
    QColor m_bgColor;
    QColor m_textColor;
};

#endif // FRAMEMIND_CHATBUBBLEWIDGET_H
