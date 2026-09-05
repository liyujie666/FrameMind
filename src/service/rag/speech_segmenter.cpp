#include "service/rag/speech_segmenter.h"

#include <QRegularExpression>

#include <algorithm>

namespace {

bool endsSentence(const QString& text)
{
    static const QRegularExpression terminal(
        QStringLiteral(R"([。！？!?；;]\s*$)"));
    return terminal.match(text.trimmed()).hasMatch();
}

QString appendText(const QString& current, const QString& next)
{
    if (current.isEmpty()) return next.trimmed();
    if (next.isEmpty()) return current;
    return current + QStringLiteral(" ") + next.trimmed();
}

} // namespace

QVector<SpeechSegment> SpeechSegmenter::buildSemanticSegments(
    const QVector<SpeechSegment>& original, const Options& options)
{
    QVector<SpeechSegment> source;
    source.reserve(original.size());
    for (const SpeechSegment& segment : original) {
        if (segment.isValid()) source.append(segment);
    }
    std::sort(source.begin(), source.end(), [](const SpeechSegment& left,
                                                const SpeechSegment& right) {
        return left.startMs < right.startMs;
    });

    QVector<SpeechSegment> output;
    if (source.isEmpty() || options.minDurationMs <= 0
        || options.maxDurationMs < options.minDurationMs) {
        return output;
    }

    SpeechSegment current;
    const auto flush = [&]() {
        if (current.isValid()) output.append(current);
        current = {};
    };

    for (const SpeechSegment& segment : source) {
        if (!current.isValid()) {
            current = segment;
            continue;
        }

        const int64_t gapMs = segment.startMs - current.endMs;
        const int64_t combinedEnd = qMax(current.endMs, segment.endMs);
        const int64_t combinedDuration = combinedEnd - current.startMs;
        const bool hasNaturalBoundary = current.durationMs() >= options.minDurationMs
                                        && endsSentence(current.text);
        if (gapMs > options.maxSilenceGapMs || combinedDuration > options.maxDurationMs
            || hasNaturalBoundary) {
            flush();
            current = segment;
            continue;
        }

        current.endMs = combinedEnd;
        current.text = appendText(current.text, segment.text);
    }
    flush();
    return output;
}
