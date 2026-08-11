#include "service/rag/audio_visual_aligner.h"

#ifdef FRAMEMIND_HAS_ONNXRUNTIME
#  include "service/embedding_service.h"
#  include "service/rag/video_rag_store.h"
#endif

#include <QRegularExpression>
#include <QSet>
#include <algorithm>

namespace {

/// 两个时间区间的重叠毫秒数
int64_t overlapMs(int64_t aStart, int64_t aEnd, int64_t bStart, int64_t bEnd)
{
    return std::max<int64_t>(0, std::min(aEnd, bEnd) - std::max(aStart, bStart));
}

/// 中文停用词 + 高频虚词，避免关键词交集被"的/了/是"之类拉高
const QSet<QString>& stopwords()
{
    static const QSet<QString> kStop = {
        QStringLiteral("的"), QStringLiteral("了"), QStringLiteral("是"),
        QStringLiteral("在"), QStringLiteral("和"), QStringLiteral("与"),
        QStringLiteral("这"), QStringLiteral("那"), QStringLiteral("我"),
        QStringLiteral("你"), QStringLiteral("他"), QStringLiteral("她"),
        QStringLiteral("们"), QStringLiteral("一个"), QStringLiteral("正在"),
        QStringLiteral("有"), QStringLiteral("不"), QStringLiteral("也"),
        QStringLiteral("就"), QStringLiteral("都"), QStringLiteral("而"),
        QStringLiteral("画面"), QStringLiteral("场景"), QStringLiteral("镜头"),
        QStringLiteral("显示"), QStringLiteral("可以"), QStringLiteral("看到"),
        QStringLiteral("the"), QStringLiteral("and"), QStringLiteral("that"),
        QStringLiteral("this"), QStringLiteral("with"), QStringLiteral("for")
    };
    return kStop;
}

} // namespace

AudioVisualAligner::AudioVisualAligner(QObject* parent)
    : QObject(parent)
{
}

// ============================================================
// 时间对齐
// ============================================================

QVector<SpeechSegment> AudioVisualAligner::overlappingSpeechSegments(
    const Scene& scene,
    const QVector<SpeechSegment>& segments,
    const Limits& limits) const
{
    QVector<SpeechSegment> out;
    if (!scene.isValid() || segments.isEmpty()) return out;

    // 先收集所有重叠段，并按"重叠时长 / 语音段时长"排序，
    // 优先保留主要落在本场景内的段，避免跨场景长段挤占配额
    struct Candidate {
        SpeechSegment seg;
        float ratio = 0.0f;
    };
    QVector<Candidate> candidates;
    candidates.reserve(segments.size());

    for (const SpeechSegment& seg : segments) {
        if (seg.endMs <= seg.startMs) continue;
        if (scene.startMs >= seg.endMs || seg.startMs >= scene.endMs) continue;

        const int64_t ov = overlapMs(scene.startMs, scene.endMs,
                                     seg.startMs, seg.endMs);
        if (ov <= 0) continue;

        const float ratio = static_cast<float>(ov)
                            / static_cast<float>(seg.durationMs());
        if (ratio < limits.minOverlapRatio) continue;

        candidates.append({ seg, ratio });
    }
    if (candidates.isEmpty()) return out;

    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& a, const Candidate& b) {
                         return a.ratio > b.ratio;
                     });

    int usedChars = 0;
    for (const Candidate& c : candidates) {
        if (out.size() >= limits.maxSegments) break;
        if (usedChars + c.seg.text.size() > limits.maxChars) {
            // 已经收到内容就停；一段都没收到时至少截断保留一段
            if (!out.isEmpty()) break;
            SpeechSegment truncated = c.seg;
            truncated.text = c.seg.text.left(limits.maxChars);
            out.append(truncated);
            break;
        }
        usedChars += c.seg.text.size();
        out.append(c.seg);
    }

    // 恢复时间升序，保留原始时间范围
    std::sort(out.begin(), out.end(),
              [](const SpeechSegment& a, const SpeechSegment& b) {
                  return a.startMs < b.startMs;
              });
    return out;
}

// ============================================================
// 转写文本格式化
// ============================================================

QString AudioVisualAligner::formatTranscript(const QVector<SpeechSegment>& segments)
{
    const auto msToTime = [](int64_t ms) {
        const int m = static_cast<int>(ms / 60000);
        const int s = static_cast<int>((ms % 60000) / 1000);
        return QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QChar('0'));
    };

    QString out;
    for (const SpeechSegment& seg : segments) {
        const QString text = seg.text.trimmed();
        if (text.isEmpty()) continue;
        out += QStringLiteral("[%1-%2] %3\n")
                   .arg(msToTime(seg.startMs), msToTime(seg.endMs), text);
    }
    return out;
}

QString AudioVisualAligner::plainTranscript(const QVector<SpeechSegment>& segments)
{
    QStringList parts;
    parts.reserve(segments.size());
    for (const SpeechSegment& seg : segments) {
        const QString text = seg.text.trimmed();
        if (!text.isEmpty()) parts << text;
    }
    return parts.join(QStringLiteral(" "));
}

// ============================================================
// 关键词提取
// ============================================================

QStringList AudioVisualAligner::extractKeywords(const QString& text)
{
    // 中文没有空格分词，这里用 2-gram 近似；英文/数字按连续字母数字切词。
    // 目的只是估算"人物/物体/地点/动作"层面的字面交集，不需要真正的分词器。
    QStringList out;
    QString cjkRun;
    QString latinRun;

    const auto flushCjk = [&]() {
        if (cjkRun.size() >= 2) {
            for (int i = 0; i + 1 < cjkRun.size(); ++i) {
                const QString gram = cjkRun.mid(i, 2);
                if (!stopwords().contains(gram)) out << gram;
            }
        } else if (cjkRun.size() == 1 && !stopwords().contains(cjkRun)) {
            out << cjkRun;
        }
        cjkRun.clear();
    };
    const auto flushLatin = [&]() {
        if (latinRun.size() >= 2) {
            const QString lower = latinRun.toLower();
            if (!stopwords().contains(lower)) out << lower;
        }
        latinRun.clear();
    };

    for (const QChar ch : text) {
        const ushort code = ch.unicode();
        const bool isCjk = (code >= 0x4E00 && code <= 0x9FFF);
        if (isCjk) {
            flushLatin();
            cjkRun.append(ch);
        } else if (ch.isLetterOrNumber()) {
            flushCjk();
            latinRun.append(ch);
        } else {
            flushCjk();
            flushLatin();
        }
    }
    flushCjk();
    flushLatin();
    return out;
}

// ============================================================
// 语义门控
// ============================================================

AudioVisualGate AudioVisualAligner::gate(const QString& visualDescription,
                                          const QVector<SpeechSegment>& sceneSegments,
                                          const Scene& scene,
                                          const QVector<Scene>& allScenes) const
{
    AudioVisualGate g;

    if (sceneSegments.isEmpty()) {
        g.candidate = AudioVisualRelation::Unknown;
        g.candidateConfidence = 0.0f;
        return g;
    }

    const QString transcript = plainTranscript(sceneSegments);
    g.transcriptChars = transcript.size();

    // ---- 信号 1：时间覆盖率 ----
    const int64_t sceneDuration = std::max<int64_t>(1, scene.durationMs());
    int64_t covered = 0;
    for (const SpeechSegment& seg : sceneSegments) {
        covered += overlapMs(scene.startMs, scene.endMs, seg.startMs, seg.endMs);
    }
    g.timeCoverage = std::min(
        1.0f, static_cast<float>(covered) / static_cast<float>(sceneDuration));

    // ---- 信号 2：跨场景跨度 ----
    // 语音段横跨的场景数占总场景数的比例；长解说会横跨很多场景
    if (!allScenes.isEmpty()) {
        int spanned = 0;
        for (const Scene& s : allScenes) {
            for (const SpeechSegment& seg : sceneSegments) {
                if (overlapMs(s.startMs, s.endMs, seg.startMs, seg.endMs) > 0) {
                    ++spanned;
                    break;
                }
            }
        }
        g.crossSceneSpan = static_cast<float>(std::max(0, spanned - 1))
                           / static_cast<float>(allScenes.size());
    }

    // ---- 信号 3：关键词交集 ----
    const QStringList visualKeys = extractKeywords(visualDescription);
    const QStringList audioKeys  = extractKeywords(transcript);
    if (!visualKeys.isEmpty() && !audioKeys.isEmpty()) {
        const QSet<QString> visualSet(visualKeys.begin(), visualKeys.end());
        const QSet<QString> audioSet(audioKeys.begin(), audioKeys.end());
        QSet<QString> shared = visualSet;
        shared.intersect(audioSet);
        g.sharedKeywords = QStringList(shared.begin(), shared.end());
        std::sort(g.sharedKeywords.begin(), g.sharedKeywords.end());
        if (g.sharedKeywords.size() > 12) g.sharedKeywords.resize(12);

        const int denom = std::min(visualSet.size(), audioSet.size());
        if (denom > 0) {
            g.keywordOverlap = static_cast<float>(shared.size())
                               / static_cast<float>(denom);
        }
    }

    // ---- 信号 4：语义相似度 ----
#ifdef FRAMEMIND_HAS_ONNXRUNTIME
    if (m_embedder && m_embedder->isReady()
        && !visualDescription.isEmpty() && !transcript.isEmpty()) {
        const auto visualEmb = m_embedder->embedPassage(visualDescription);
        const auto audioEmb  = m_embedder->embedPassage(transcript);
        if (!visualEmb.empty() && visualEmb.size() == audioEmb.size()) {
            g.semanticSimilarity =
                VideoRAGStore::cosineSimilarity(visualEmb, audioEmb);
        }
    }
#endif

    // ---- 综合打分 ----
    // 相似度未计算时（无 ONNX）退化为关键词 + 覆盖率驱动，
    // 此时倾向给出 Contextual/Unknown 而不是 Strong，保持保守。
    const bool hasSim = g.hasSemanticSimilarity();

    // BGE 相似度在同主题短文本上普遍偏高，映射到 [0,1] 时抬高下限阈值
    const float simScore = hasSim
        ? std::clamp((g.semanticSimilarity - 0.35f) / 0.35f, 0.0f, 1.0f)
        : 0.0f;
    const float keyScore = std::clamp(g.keywordOverlap / 0.25f, 0.0f, 1.0f);
    const float covScore = std::clamp(g.timeCoverage / 0.4f, 0.0f, 1.0f);
    const float spanPenalty = std::clamp(g.crossSceneSpan / 0.3f, 0.0f, 1.0f);

    float relevance = hasSim
        ? (0.45f * simScore + 0.30f * keyScore + 0.25f * covScore)
        : (0.55f * keyScore + 0.45f * covScore);
    relevance *= (1.0f - 0.35f * spanPenalty);
    relevance = std::clamp(relevance, 0.0f, 1.0f);

    // 转写内容过短时信息量不足，不足以支撑强关联判断
    const bool tooShort = g.transcriptChars < 6;

    if (tooShort) {
        g.candidate = AudioVisualRelation::Unknown;
        g.candidateConfidence = 0.3f;
    } else if (relevance >= 0.6f) {
        g.candidate = AudioVisualRelation::Strong;
        g.candidateConfidence = 0.55f + 0.4f * (relevance - 0.6f) / 0.4f;
    } else if (relevance >= 0.3f) {
        g.candidate = AudioVisualRelation::Contextual;
        g.candidateConfidence = 0.45f + 0.25f * (relevance - 0.3f) / 0.3f;
    } else if (relevance >= 0.12f) {
        g.candidate = AudioVisualRelation::Unknown;
        g.candidateConfidence = 0.35f;
    } else {
        g.candidate = AudioVisualRelation::Independent;
        g.candidateConfidence = 0.5f + 0.3f * (0.12f - relevance) / 0.12f;
    }
    g.candidateConfidence = std::clamp(g.candidateConfidence, 0.0f, 0.95f);

    return g;
}
