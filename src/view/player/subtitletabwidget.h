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

/**
 * 字幕 Tab：展示 Whisper ASR 转写的语音段列表。
 *
 * 每行显示：时间戳 + 转写文本
 * 当前播放位置对应的字幕行高亮，并自动滚动到可视区域。
 * 点击任意行发出 seekRequested(posMs) 信号跳转播放。
 */
class SubtitleTabWidget : public QWidget {
    Q_OBJECT
public:
    explicit SubtitleTabWidget(QWidget* parent = nullptr);

    void setThemeService(ThemeService* theme);
    void setViewModel(VideoAnalysisViewModel* vm);

public slots:
    /// 由 PlayerViewModel::positionChanged 驱动，高亮当前字幕行
    void onPositionChanged(int64_t posMs);

signals:
    void seekRequested(int64_t posMs);

private slots:
    void onSpeechSegmentsReady(const QVector<SpeechSegment>& segments);
    void onThemeChanged();

private:
    void buildRows();
    void clearRows();
    void updateHighlight(int64_t posMs);
    void applyScrollStyle();
    QWidget* makeSubtitleRow(const SpeechSegment& seg);

    static QString formatMs(int64_t ms);

    ThemeService*           m_theme      = nullptr;
    VideoAnalysisViewModel* m_vm         = nullptr;
    QScrollArea*            m_scroll     = nullptr;
    QWidget*                m_container  = nullptr;
    QVBoxLayout*            m_rowLayout  = nullptr;

    QVector<SpeechSegment>  m_segments;
    int64_t                 m_currentPosMs = 0;
    int                     m_currentRow   = -1;

    QVector<QWidget*> m_rows;
};

#endif // FRAMEMIND_SUBTITLETABWIDGET_H
