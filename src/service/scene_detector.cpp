#include "service/scene_detector.h"

#ifdef FRAMEMIND_HAS_ONNXRUNTIME
#include "infrastructure/onnx_runtime_engine.h"
#endif

#include <QtConcurrent/QtConcurrent>
#include <QDebug>
#include <cmath>
#include <algorithm>

// ---------------------------------------------------------------------------
// SceneDetector 实现
//
// 直方图差异算法（默认）：
//   1. 将两帧降采样到 m_downscaleSize x m_downscaleSize
//   2. 计算每帧的 RGB 颜色直方图
//   3. 用 Bhattacharyya 距离度量直方图差异
//   4. 差异 > threshold → 场景切换
//
// 性能（1min 视频, 每 0.5s 采一帧 = 120 帧）：
//   120 次 computeDifference() < 50ms (纯 Qt, 64x64 降采样)
//
// 可选 OpenCV 加速：
//   若项目已链接 OpenCV，可替换 histogramDifference() 内部实现
//   使用 cv::calcHist + cv::compareHist，精度相同但更快
// ---------------------------------------------------------------------------

SceneDetector::SceneDetector(QObject* parent)
    : QObject(parent)
{
}

SceneDetector::~SceneDetector() = default;

// =========================================================================
// 配置
// =========================================================================

bool SceneDetector::loadTransNetV2(const QString& modelPath)
{
#ifdef FRAMEMIND_HAS_ONNXRUNTIME
    m_transnetEngine = std::make_unique<OnnxRuntimeEngine>(false);
    if (m_transnetEngine->loadModel(modelPath)) {
        m_useTransNet = true;
        qDebug() << "[SceneDetector] TransNetV2 加载成功，切换到深度学习模式"
                 << "| input: [1,100,27,48,3]";
        return true;
    }
    m_transnetEngine.reset();
    qWarning() << "[SceneDetector] TransNetV2 加载失败，保持直方图模式";
    return false;
#else
    Q_UNUSED(modelPath)
    qWarning() << "[SceneDetector] ONNX Runtime 未启用，无法加载 TransNetV2";
    return false;
#endif
}

// =========================================================================
// 逐帧差异计算
// =========================================================================

float SceneDetector::computeDifference(const QImage& current,
                                        const QImage& previous)
{
    if (current.isNull() || previous.isNull()) {
        return 1.0f;
    }
    return histogramDifference(current, previous);
}

bool SceneDetector::isSceneChange(const QImage& current,
                                   const QImage& previous)
{
    float diff = computeDifference(current, previous);
    return diff > m_threshold;
}

// =========================================================================
// 批量场景分割
// =========================================================================

QVector<Scene> SceneDetector::detectScenes(
    const QVector<QImage>& frames,
    const QVector<int64_t>& timestampsMs)
{
    QVector<Scene> scenes;

    if (frames.isEmpty() || frames.size() != timestampsMs.size()) {
        qWarning() << "[SceneDetector] 帧数与时间戳数不匹配"
                    << "| frames:" << frames.size()
                    << "| timestamps:" << timestampsMs.size();
        return scenes;
    }

    if (frames.size() == 1) {
        Scene s;
        s.id = 0;
        s.startMs = timestampsMs[0];
        s.endMs = timestampsMs[0] + 1000;
        s.keyframeMs = timestampsMs[0];
        s.keyframe = frames[0];
        scenes.append(s);
        return scenes;
    }

    // 判断采样密度：帧间隔 > 2s 视为稀疏，始终用直方图
    const int64_t avgIntervalMs = (timestampsMs.last() - timestampsMs.first())
                                  / (timestampsMs.size() - 1);
    const bool isSparse = avgIntervalMs > 2000;

    qDebug() << "[SceneDetector] detectScenes | frames:" << frames.size()
             << "| avgInterval:" << avgIntervalMs << "ms"
             << "| mode:" << (isSparse ? "histogram" : "TransNetV2");

    std::vector<float> probs;

#ifdef FRAMEMIND_HAS_ONNXRUNTIME
    if (!isSparse && m_useTransNet && m_transnetEngine && m_transnetEngine->isLoaded()) {
        probs = transnetBatchPredict(frames);
    }
#endif

    int currentSceneId = 0;
    int sceneStartIdx  = 0;

    for (int i = 0; i + 1 < frames.size(); ++i) {
        float score = 0.0f;
        if (!probs.empty() && i < static_cast<int>(probs.size())) {
            score = probs[i];
        } else {
            score = histogramDifference(frames[i + 1], frames[i]);
        }

        if (score > m_threshold) {
            Scene s;
            s.id       = currentSceneId++;
            s.startMs  = timestampsMs[sceneStartIdx];
            s.endMs    = timestampsMs[i + 1];
            s.keyframeMs = timestampsMs[sceneStartIdx];
            s.keyframe = frames[sceneStartIdx];
            scenes.append(s);

            emit sceneBoundaryDetected(timestampsMs[i + 1], score);
            sceneStartIdx = i + 1;
        }
    }

    // 闭合最后一个场景
    {
        Scene s;
        s.id       = currentSceneId;
        s.startMs  = timestampsMs[sceneStartIdx];
        s.endMs    = timestampsMs.last() + 1000;
        s.keyframeMs = timestampsMs[sceneStartIdx];
        s.keyframe = frames[sceneStartIdx];
        scenes.append(s);
    }

    qDebug() << "[SceneDetector] 检测完成，共" << scenes.size() << "个场景";
    return scenes;
}

QFuture<QVector<Scene>> SceneDetector::detectScenesAsync(
    const QVector<QImage>& frames,
    const QVector<int64_t>& timestampsMs)
{
    return QtConcurrent::run([this, frames, timestampsMs]() {
        return this->detectScenes(frames, timestampsMs);
    });
}

// =========================================================================
// 阶段 1：直方图粗分割 — 返回候选边界
// =========================================================================

QVector<QPair<int64_t, float>> SceneDetector::detectCandidateBoundaries(
    const QVector<QImage>& frames,
    const QVector<int64_t>& timestampsMs)
{
    QVector<QPair<int64_t, float>> candidates;
    if (frames.size() < 2 || frames.size() != timestampsMs.size()) return candidates;

    for (int i = 0; i + 1 < frames.size(); ++i) {
        float score = histogramDifference(frames[i + 1], frames[i]);
        if (score > m_threshold) {
            candidates.append({timestampsMs[i + 1], score});
        }
    }

    qDebug() << "[SceneDetector] 粗分割候选边界:" << candidates.size() << "个";
    return candidates;
}

// =========================================================================
// 阶段 2：TransNetV2 精确确认
// =========================================================================

QVector<int64_t> SceneDetector::refineBoundariesWithTransNet(
    const QVector<QImage>& denseFrames,
    const QVector<int64_t>& denseTimestampsMs)
{
    QVector<int64_t> confirmed;

#ifdef FRAMEMIND_HAS_ONNXRUNTIME
    if (!m_useTransNet || !m_transnetEngine || !m_transnetEngine->isLoaded()) {
        qDebug() << "[SceneDetector] TransNetV2 不可用，跳过精确确认";
        return confirmed;
    }
    if (denseFrames.size() < 2 || denseFrames.size() != denseTimestampsMs.size()) {
        return confirmed;
    }

    std::vector<float> probs = transnetBatchPredict(denseFrames);

    // TransNetV2 输出的阈值通常用 0.5 判定（sigmoid 输出）
    constexpr float kTransnetThreshold = 0.3f;
    for (int i = 0; i < static_cast<int>(probs.size()) - 1; ++i) {
        if (probs[i] > kTransnetThreshold) {
            confirmed.append(denseTimestampsMs[i]);
        }
    }

    qDebug() << "[SceneDetector] TransNetV2 精确确认:" << confirmed.size()
             << "个边界 (输入帧:" << denseFrames.size() << ")";
#else
    Q_UNUSED(denseFrames)
    Q_UNUSED(denseTimestampsMs)
#endif
    return confirmed;
}

// =========================================================================
// 直方图差异计算（纯 Qt 实现）
// =========================================================================

float SceneDetector::histogramDifference(const QImage& a, const QImage& b)
{
    QImage imgA = downscale(a);
    QImage imgB = downscale(b);

    // RGB 三通道直方图（每通道 16 bins）
    constexpr int BINS = 16;
    constexpr int BIN_SIZE = 256 / BINS;  // = 16

    std::vector<int> histA(3 * BINS, 0);
    std::vector<int> histB(3 * BINS, 0);

    const int w = imgA.width();
    const int h = imgA.height();
    const int n = w * h;

    for (int y = 0; y < h; ++y) {
        const QRgb* lineA = reinterpret_cast<const QRgb*>(imgA.scanLine(y));
        const QRgb* lineB = reinterpret_cast<const QRgb*>(imgB.scanLine(y));
        for (int x = 0; x < w; ++x) {
            histA[0 * BINS + qRed(lineA[x])   / BIN_SIZE]++;
            histA[1 * BINS + qGreen(lineA[x]) / BIN_SIZE]++;
            histA[2 * BINS + qBlue(lineA[x])  / BIN_SIZE]++;

            histB[0 * BINS + qRed(lineB[x])   / BIN_SIZE]++;
            histB[1 * BINS + qGreen(lineB[x]) / BIN_SIZE]++;
            histB[2 * BINS + qBlue(lineB[x])  / BIN_SIZE]++;
        }
    }

    // Bhattacharyya 距离
    // D = -ln( sum( sqrt(p_i * q_i) ) )
    // 归一化直方图后计算，结果映射到 [0, 1]
    float sum = 0.0f;
    for (int i = 0; i < 3 * BINS; ++i) {
        float pa = static_cast<float>(histA[i]) / n;
        float pb = static_cast<float>(histB[i]) / n;
        sum += std::sqrt(pa * pb);
    }

    // sum ∈ [0, 1]，1 表示完全相同
    // 距离 = 1 - sum，映射到 [0, 1]
    float distance = 1.0f - sum;
    return std::clamp(distance, 0.0f, 1.0f);
}

// =========================================================================
// TransNetV2 批量推理
// =========================================================================

// TransNetV2 模型规格（从 ONNX 文件读取确认）：
//   input  "input" : float32 [1, 100, 27, 48, 3]
//     - 1    = batch size（固定）
//     - 100  = 每次处理的帧数（固定维度，不足则 zero-pad）
//     - 27   = 帧高度
//     - 48   = 帧宽度
//     - 3    = RGB 通道
//   output "534"   : float32 [1, 100, 1] — single-frame 切换概率
//   output "535"   : float32 [1, 100, 1] — many-transition 概率（不使用）
//
// 推理策略：
//   每次最多送 kTransnetBatch(100) 帧，循环直到全部处理完。
//   每帧 resize 到 27x48 RGB，归一化到 [0,1]。
//   输出的 probs[i] 代表第 i 帧与 i+1 帧之间发生场景切换的概率。

void SceneDetector::preprocessTransnetFrame(const QImage& img, float* dst) const
{
    QImage scaled = img.scaled(kTransnetW, kTransnetH,
                               Qt::IgnoreAspectRatio,
                               Qt::SmoothTransformation);
    if (scaled.format() != QImage::Format_RGB888) {
        scaled = scaled.convertToFormat(QImage::Format_RGB888);
    }
    for (int y = 0; y < kTransnetH; ++y) {
        const uint8_t* line = scaled.constScanLine(y);
        for (int x = 0; x < kTransnetW; ++x) {
            const uint8_t* px = line + x * 3;
            const int idx = (y * kTransnetW + x) * kTransnetC;
            dst[idx + 0] = static_cast<float>(px[0]);  // R in [0, 255]
            dst[idx + 1] = static_cast<float>(px[1]);  // G
            dst[idx + 2] = static_cast<float>(px[2]);  // B
        }
    }
}

std::vector<float> SceneDetector::transnetBatchPredict(const QVector<QImage>& frames)
{
#ifndef FRAMEMIND_HAS_ONNXRUNTIME
    Q_UNUSED(frames)
    return {};
#else
    const int N           = frames.size();
    const int framePixels = kTransnetH * kTransnetW * kTransnetC;   // 27*48*3 = 3888

    std::vector<std::vector<float>> frameBufs(N, std::vector<float>(framePixels));
    for (int i = 0; i < N; ++i) {
        preprocessTransnetFrame(frames[i], frameBufs[i].data());
    }

    std::vector<float> probs(N, 0.0f);

    int frameIdx = 0;
    while (frameIdx < N) {
        int actualBatch = std::min(kTransnetBatch, N - frameIdx);

        // dim[1] 固定为 kTransnetBatch，不足时 zero-pad
        std::vector<float> inputBuf(kTransnetBatch * framePixels, 0.0f);

        for (int b = 0; b < actualBatch; ++b) {
            float* dst = inputBuf.data() + b * framePixels;
            std::copy(frameBufs[frameIdx + b].begin(),
                      frameBufs[frameIdx + b].end(),
                      dst);
        }

        // shape: [1, 100, 27, 48, 3] = [batch, n_frames, H, W, C]
        std::vector<int64_t> shape = {1, kTransnetBatch, kTransnetH,
                                      kTransnetW, kTransnetC};
        auto inputTensor = m_transnetEngine->createTensor(inputBuf.data(), shape);

        std::vector<Ort::Value> inputs;
        inputs.push_back(std::move(inputTensor));
        std::vector<Ort::Value> outputs;
        m_transnetEngine->run(inputs, outputs);

        if (outputs.empty()) {
            qWarning() << "[SceneDetector] TransNetV2 推理无输出，跳过此批";
            frameIdx += actualBatch;
            continue;
        }

        const float* outData = outputs[0].GetTensorData<float>();
        float maxProb = 0.0f;
        for (int b = 0; b < actualBatch; ++b) {
            float prob = outData[b];  // already sigmoid, expects uint8-range input
            probs[frameIdx + b] = prob;
            if (prob > maxProb) maxProb = prob;
        }

        qDebug() << "[SceneDetector] TransNetV2 batch"
                 << frameIdx << "-" << (frameIdx + actualBatch - 1)
                 << "| maxProb:" << maxProb;

        frameIdx += actualBatch;
    }

    qDebug() << "[SceneDetector] TransNetV2 推理完成 | frames:" << N;
    return probs;
#endif
}

// =========================================================================
// 辅助
// =========================================================================

QImage SceneDetector::downscale(const QImage& img) const
{
    return img.scaled(m_downscaleSize, m_downscaleSize,
                      Qt::IgnoreAspectRatio,
                      Qt::FastTransformation)
             .convertToFormat(QImage::Format_RGB32);
}
