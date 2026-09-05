#ifndef FRAMEMIND_CHATINPUTWIDGET_H
#define FRAMEMIND_CHATINPUTWIDGET_H

#include <QWidget>
#include <QImage>
#include <QList>
#include <cstdint>

class QTextEdit;
class QToolButton;
class QPushButton;
class QLabel;
class QHBoxLayout;
class QMenu;
class ThemeService;

class ChatInputWidget : public QWidget {
    Q_OBJECT
public:
    explicit ChatInputWidget(QWidget* parent = nullptr);

    void setStreaming(bool streaming);
    void setThemeService(ThemeService* theme);
    void addFrame(const QImage& frame, int64_t timestampMs);
    void clearAllFrames();
    void setCurrentTimestamp(int64_t timestampMs);
    void setVideoDuration(int64_t durationMs);

signals:
    void sendRequested(const QString& text, bool withFrames);
    void stopRequested();
    void currentFrameRequested();
    void timeRangeRequested(int64_t startMs, int64_t endMs);
    void frameRemoved(int index);
    void allFramesCleared();

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    struct FrameItem {
        QImage image;
        int64_t timestampMs = 0;
        QWidget* widget = nullptr;
        QLabel* label = nullptr;
        QToolButton* deleteButton = nullptr;
    };

    void triggerSend();
    void applyColors();
    void createFramePreview(int index);
    void removeFrame(int index);
    void showFramePreview(int index);
    void showTemplateMenu();
    void showTimeRangeDialog();
    void insertTemplate(const QString& templateText);
    QString formatTimestamp(int64_t timestampMs) const;

    QTextEdit* m_edit = nullptr;
    QToolButton* m_frameButton = nullptr;
    QToolButton* m_templateButton = nullptr;
    QToolButton* m_timeRangeButton = nullptr;
    QPushButton* m_sendButton = nullptr;
    QWidget* m_framesContainer = nullptr;
    QHBoxLayout* m_framesLayout = nullptr;
    QMenu* m_templateMenu = nullptr;
    ThemeService* m_theme = nullptr;
    bool m_streaming = false;
    QList<FrameItem> m_frames;
    int64_t m_currentTimestampMs = 0;
    int64_t m_videoDurationMs = 0;
};

#endif // FRAMEMIND_CHATINPUTWIDGET_H
