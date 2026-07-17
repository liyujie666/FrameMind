#include "service/whisper_service.h"

#include <whisper.h>
#include <QDebug>
#include <QtConcurrent/QtConcurrent>
#include <cstring>

// ---------------------------------------------------------------------------
// whisper.cpp 集成说明
//
// 依赖：
//   third_party/whisper.cpp/whisper.h
//   third_party/whisper.cpp/libwhisper.lib (或通过 add_subdirectory 构建)
//
// 模型下载：
//   https://huggingface.co/ggerganov/whisper.cpp
//   推荐先用 ggml-small.bin (466MB, 中文效果好)
//
// CMake 集成：
//   set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
//   set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
//   add_subdirectory(third_party/whisper.cpp)
//   target_link_libraries(FrameMind PRIVATE whisper)
// ---------------------------------------------------------------------------

WhisperService::WhisperService(QObject* parent)
    : QObject(parent)
{
}

WhisperService::~WhisperService()
{
    if (m_ctx) {
        whisper_free(m_ctx);
        m_ctx = nullptr;
    }
}

bool WhisperService::initialize(const QString& modelPath)
{
    if (m_ctx) {
        whisper_free(m_ctx);
        m_ctx = nullptr;
    }

    m_ctx = whisper_init_from_file(modelPath.toUtf8().constData());
    if (!m_ctx) {
        qWarning() << "[WhisperService] 模型加载失败:" << modelPath;
        return false;
    }

    qDebug() << "[WhisperService] 模型加载成功:" << modelPath;
    return true;
}

bool WhisperService::isReady() const
{
    return m_ctx != nullptr;
}

void WhisperService::setLanguage(const QString& lang)
{
    m_language = lang;
}

void WhisperService::setGreedySampling(bool greedy)
{
    m_greedy = greedy;
}

QVector<SpeechSegment> WhisperService::transcribe(
    const std::vector<float>& pcmSamples, int sampleRate)
{
    if (!m_ctx || pcmSamples.empty()) {
        return {};
    }

    // whisper.cpp 要求 16kHz；如果采样率不匹配，调用方需先重采样
    if (sampleRate != 16000) {
        qWarning() << "[WhisperService] 采样率应为 16000，实际为" << sampleRate
                    << "请先重采样";
        return {};
    }

    // 配置推理参数
    whisper_full_params params = whisper_full_default_params(
        m_greedy ? WHISPER_SAMPLING_GREEDY : WHISPER_SAMPLING_BEAM_SEARCH);

    params.print_realtime       = false;
    params.print_progress       = false;
    params.print_timestamps     = false;
    params.print_special_tokens = false;
    params.translate            = false;
    params.language             = m_language.toUtf8().constData();
    params.n_threads            = m_nThreads;
    params.no_context           = true;   // 不使用上一次的上下文
    params.single_segment       = false;

    // 执行推理
    int ret = whisper_full(m_ctx, params,
                           pcmSamples.data(),
                           static_cast<int>(pcmSamples.size()));
    if (ret != 0) {
        qWarning() << "[WhisperService] 推理失败, ret =" << ret;
        return {};
    }

    // 提取分段结果
    QVector<SpeechSegment> segments;
    int nSegments = whisper_full_n_segments(m_ctx);

    for (int i = 0; i < nSegments; ++i) {
        SpeechSegment seg;
        // whisper.cpp 时间戳单位是 10ms（centisecond）
        seg.startMs = whisper_full_get_segment_t0(m_ctx, i) * 10;
        seg.endMs   = whisper_full_get_segment_t1(m_ctx, i) * 10;
        seg.text    = QString::fromUtf8(
            whisper_full_get_segment_text(m_ctx, i)).trimmed();

        if (!seg.text.isEmpty()) {
            segments.append(seg);
            emit segmentReady(seg);
        }
    }

    qDebug() << "[WhisperService] 转写完成，共" << segments.size() << "段";

    return segments;
}

QFuture<QVector<SpeechSegment>> WhisperService::transcribeAsync(
    const std::vector<float>& pcmSamples, int sampleRate)
{
    return QtConcurrent::run([this, pcmSamples, sampleRate]() {
        return this->transcribe(pcmSamples, sampleRate);
    });
}
