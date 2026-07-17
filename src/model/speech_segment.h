#ifndef FRAMEMIND_SPEECH_SEGMENT_H
#define FRAMEMIND_SPEECH_SEGMENT_H

#include <QString>
#include <QMetaType>
#include <cstdint>
#include <vector>

/**
 * 语音转写的一段文本。
 *
 * 由 WhisperService 产出，包含起止时间戳和转写文本。
 * 后续文本内容会被 EmbeddingService 编码为向量入库。
 */
struct SpeechSegment {
    int64_t  startMs = 0;    // 段起始时间
    int64_t  endMs   = 0;    // 段结束时间
    QString  text;            // 转写文本

    int64_t durationMs() const { return endMs - startMs; }
    bool isValid() const { return endMs > startMs && !text.isEmpty(); }
};

Q_DECLARE_METATYPE(SpeechSegment)

#endif // FRAMEMIND_SPEECH_SEGMENT_H
