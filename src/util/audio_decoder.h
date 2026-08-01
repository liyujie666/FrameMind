#ifndef FRAMEMIND_AUDIO_DECODER_H
#define FRAMEMIND_AUDIO_DECODER_H

#include <QString>
#include <vector>
#include <cstdint>
#include <functional>

/**
 * 音频解码器：从视频/音频文件中抽取 PCM 数据。
 *
 * 输出格式固定为 Whisper 要求的：16kHz, mono, float32。
 * 内部使用 FFmpeg C API（avformat + avcodec + swresample）。
 *
 * 使用方式：
 *   AudioDecoder decoder;
 *   auto pcm = decoder.decodeToFloat32(videoPath);
 *   // pcm 是完整音轨的 16kHz mono float32 采样
 *
 * 也支持解码指定时间区间（减少内存占用）。
 */
class AudioDecoder {
public:
    AudioDecoder() = default;
    ~AudioDecoder() = default;

    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;

    /// 解码完整音轨为 16kHz mono float32 PCM
    /// @return 采样数据；失败返回空 vector
    std::vector<float> decodeToFloat32(const QString& filePath);

    /// 解码指定时间区间
    /// @param startMs 起始时间（ms），-1 表示从头
    /// @param endMs   结束时间（ms），-1 表示到尾
    std::vector<float> decodeToFloat32(const QString& filePath,
                                       int64_t startMs, int64_t endMs);

    /// 进度回调（0~100），可选设置
    using ProgressCallback = std::function<void(int percent)>;
    void setProgressCallback(ProgressCallback cb) { m_progress = std::move(cb); }

    /// 取消标记
    void cancel() { m_cancelled = true; }
    void resetCancel() { m_cancelled = false; }

    static constexpr int TARGET_SAMPLE_RATE = 16000;

private:
    ProgressCallback m_progress;
    bool m_cancelled = false;
};

#endif // FRAMEMIND_AUDIO_DECODER_H
