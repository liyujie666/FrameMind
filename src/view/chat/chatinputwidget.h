#ifndef FRAMEMIND_CHATINPUTWIDGET_H
#define FRAMEMIND_CHATINPUTWIDGET_H

#include <QWidget>

class QTextEdit;
class QToolButton;
class QPushButton;
class ThemeService;

/**
 * 对话输入区（M2-T6）：多行输入 + 「📷 当前帧」开关 + 发送/停止按钮。
 * Enter 发送，Shift+Enter 换行。
 */
class ChatInputWidget : public QWidget {
    Q_OBJECT
public:
    explicit ChatInputWidget(QWidget* parent = nullptr);

    void setStreaming(bool streaming);
    void setThemeService(ThemeService* theme);

signals:
    void sendRequested(const QString& text, bool withCurrentFrame);
    void stopRequested();

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    void triggerSend();
    void applyColors();

    QTextEdit*   m_edit = nullptr;
    QToolButton* m_frameBtn = nullptr;
    QPushButton* m_sendBtn = nullptr;
    ThemeService* m_theme = nullptr;
    bool         m_streaming = false;
};

#endif // FRAMEMIND_CHATINPUTWIDGET_H
