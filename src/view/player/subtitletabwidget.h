#ifndef FRAMEMIND_SUBTITLETABWIDGET_H
#define FRAMEMIND_SUBTITLETABWIDGET_H

#include <QWidget>
#include <QVector>
#include "model/speech_segment.h"

class ThemeService;
class VideoAnalysisViewModel;
class QScrollArea;
class QVBoxLayout;
class QLabel;
class QEvent;
class QTimer;

class SubtitleTabWidget : public QWidget {
    Q_OBJECT
public:
    explicit SubtitleTabWidget(QWidget* parent = nullptr);
    void setThemeService(ThemeService* theme);
    void setViewModel(VideoAnalysisViewModel* vm);

public slots:
    void onPositionChanged(int64_t posMs);

signals:
    void seekRequested(int64_t posMs);

private slots:
    void onSpeechSegmentsReady(const QVector<SpeechSegment>& segments);
    void onThemeChanged();

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    void buildRows();
    void clearRows();
    void updateHighlight(int64_t posMs);
    void applyScrollStyle();
    QWidget* makeSubtitleRow(const SpeechSegment& seg);
    static QString formatMs(int64_t ms);

    ThemeService* m_theme = nullptr;
    VideoAnalysisViewModel* m_vm = nullptr;
    QScrollArea* m_scroll = nullptr;
    QWidget* m_container = nullptr;
    QVBoxLayout* m_rowLayout = nullptr;
    QVector<SpeechSegment> m_segments;
    int64_t m_currentPosMs = 0;
    int m_currentRow = -1;
    QVector<QWidget*> m_rows;
    bool m_userScrolling = false;
    QTimer* m_scrollResetTimer = nullptr;
};

#endif // FRAMEMIND_SUBTITLETABWIDGET_H
