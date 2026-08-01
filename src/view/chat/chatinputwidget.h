#ifndef FRAMEMIND_CHATINPUTWIDGET_H
#define FRAMEMIND_CHATINPUTWIDGET_H

#include <QWidget>
#include <QImage>
#include <QList>

class QTextEdit;
class QToolButton;
class QPushButton;
class QLabel;
class QHBoxLayout;
class ThemeService;

/**
 * 对话输入区（M2-T6）：多行输入 + 「📷 当前帧」开关 + 发送/停止按钮。
 * Enter 发送，Shift+Enter 换行。
 * 支持多张图片的截取和发送。
 */
class ChatInputWidget : public QWidget {
    Q_OBJECT
public:
    explicit ChatInputWidget(QWidget* parent = nullptr);

    void setStreaming(bool streaming);
    void setThemeService(ThemeService* theme);
    void addFrame(const QImage& frame, int64_t timestampMs);
    void clearAllFrames();

signals:
    void sendRequested(const QString& text, bool withFrames);
    void stopRequested();
    void currentFrameRequested();

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    struct FrameItem {
        QImage image;
        int64_t timestamp;
        QWidget* widget = nullptr;
        QLabel* label = nullptr;
        QToolButton* deleteBtn = nullptr;
    };

    void triggerSend();
    void applyColors();
    void createFramePreview(int index);
    void removeFrame(int index);
    void showFramePreview(int index);
    void hideFramesPreview();
    void onViewFrame(int index);
    QString formatTimestamp(int64_t ms) const;
    void updateFramesLayout();

    QTextEdit*   m_edit = nullptr;
    QToolButton* m_frameBtn = nullptr;
    QPushButton* m_sendBtn = nullptr;
    QWidget*     m_framesContainer = nullptr;
    QHBoxLayout* m_framesLayout = nullptr;
    
    ThemeService* m_theme = nullptr;
    bool         m_streaming = false;
    QList<FrameItem> m_frames;
};

#endif // FRAMEMIND_CHATINPUTWIDGET_H
