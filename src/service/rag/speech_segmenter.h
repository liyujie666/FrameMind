#ifndef FRAMEMIND_SPEECH_SEGMENTER_H
#define FRAMEMIND_SPEECH_SEGMENTER_H

#include <QVector>

#include "model/speech_segment.h"

/**
 * 将 Whisper 原始段合并为仅供语义检索使用的时间窗口。
 * 原始 SpeechSegment 不被替换，字幕、精确引用和音画对齐继续使用原始粒度。
 */
class SpeechSegmenter final
{
public:
    struct Options {
        int64_t minDurationMs = 12000;
        int64_t maxDurationMs = 30000;
        int64_t maxSilenceGapMs = 1200;
    };

    static QVector<SpeechSegment> buildSemanticSegments(
        const QVector<SpeechSegment>& original, const Options& options = {});
};

#endif // FRAMEMIND_SPEECH_SEGMENTER_H
