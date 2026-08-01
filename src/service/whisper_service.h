#ifndef FRAMEMIND_WHISPER_SERVICE_H
#define FRAMEMIND_WHISPER_SERVICE_H

#include <QObject>
#include <QString>
#include <QVector>
#include <QFuture>
#include <cstdint>
#include <vector>

#include "model/speech_segment.h"

/**
 * Whisper.cpp 语音转写服务。
 *
 * 职责：
 *   - transcribe(): PCM 音频 → 分段文本（带时间戳）
 *   - 支持流式增量转写（拉流场景）
 *
 * 模型：ggml-small.bin (~466MB) 或 ggml-base.bin (~142MB)
 * 下载：https://huggingface.co/ggerganov/whisper.cpp
 *
 * 输入格式：
 *   - 16kHz, mono, float32 PCM
 *   - 可从 PlayerService 提取音频流，或从视频文件解码
 *
 * 注意：实际功能仅在 FRAMEMIND_HAS_WHISPER 宏定义时可用。
 */
class WhisperService : public QObject {
    Q_OBJECT
public:
    explicit WhisperService(QObject* parent = nullptr);
    ~WhisperService() override;

    WhisperService(const WhisperService&) = delete;
    WhisperService& operator=(const WhisperService&) = delete;

#ifdef FRAMEMIND_HAS_WHISPER
    /// 加载 ggml 模型
    /// @param modelPath ggml-small.bin / ggml-base.bin 路径
    bool initialize(const QString& modelPath);

    /// 是否已初始化
    bool isReady() const;

    /// 设置语言（默认中文 "zh"，设为 "auto" 自动检测）
    void setLanguage(const QString& lang);

    /// 设置采样策略
    /// @param greedy true=贪心(快), false=beam search(准)
    void setGreedySampling(bool greedy);

    /// 同步转写
    /// @param pcmSamples 16kHz mono float32 PCM 数据
    /// @param sampleRate 采样率（默认 16000）
    /// @return 转写分段列表
    QVector<SpeechSegment> transcribe(const std::vector<float>& pcmSamples,
                                      int sampleRate = 16000);

    /// 异步转写
    QFuture<QVector<SpeechSegment>> transcribeAsync(
        const std::vector<float>& pcmSamples, int sampleRate = 16000);

signals:
    /// 流式转写：每个分段完成时发出（用于实时显示进度）
    void segmentReady(const SpeechSegment& segment);

    /// 转写整体进度 (0~100)
    void progressChanged(int percent);

private:
    struct whisper_context* m_ctx = nullptr;
    QString                 m_language  = QStringLiteral("auto");
    bool                    m_greedy    = true;
    int                     m_nThreads  = 4;
#endif // FRAMEMIND_HAS_WHISPER
};

#endif // FRAMEMIND_WHISPER_SERVICE_H
