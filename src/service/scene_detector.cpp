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
    // TransNetV2 ONNX 模型加载
    // 模型输入: [1, 27, 3, 224, 224] (27 帧滑动窗口)
    // 模型输出: [1, 27, 1] (每帧的场景切换概率)
    //
    // 模型导出：参考 https://github.com/soCzech/TransNetV2
    // TODO: 实现 TransNetV2 推理逻辑
    m_transnetEngine = std::make_unique<OnnxRuntimeEngine>(false);
    if (m_transnetEngine->loadModel(modelPath)) {
        m_useTransNet = true;
        qDebug() << "[SceneDetector] TransNetV2 模型加载成功，切换到深度学习模式";
        return true;
    }
    m_transnetEngine.reset();
    qWarning() << "[SceneDetector] TransNetV2 模型加载失败，保持直方图模式";
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
        return 1.0f;  // 缺帧视为最大差异
    }

    if (m_useTransNet && m_transnetEngine && m_transnetEngine->isLoaded()) {
        return transnetPredict(current, previous);
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
        s.sceneId = 0;
        s.startMs = timestampsMs[0];
        s.endMs = timestampsMs[0] + 1000;
        s.keyframe = frames[0];
        s.changeScore = 0.0f;
        scenes.append(s);
        return scenes;
    }

    // 1. 逐帧检测边界
    int currentSceneId = 0;
    int sceneStartIdx = 0;

    for (int i = 1; i < frames.size(); ++i) {
        float diff = computeDifference(frames[i], frames[i - 1]);

        if (diff > m_threshold) {
            // 场景切换：闭合当前场景
            Scene s;
            s.sceneId = currentSceneId;
            s.startMs = timestampsMs[sceneStartIdx];
            s.endMs = timestampsMs[i];
            s.keyframe = frames[sceneStartIdx];
            s.changeScore = diff;
            scenes.append(s);

            emit sceneBoundaryDetected(timestampsMs[i], diff);

            currentSceneId++;
            sceneStartIdx = i;
        }
    }

    // 2. 闭合最后一个场景
    if (sceneStartIdx < frames.size()) {
        Scene s;
        s.sceneId = currentSceneId;
        s.startMs = timestampsMs[sceneStartIdx];
        s.endMs = timestampsMs.last() + 1000;
        s.keyframe = frames[sceneStartIdx];
        s.changeScore = 0.0f;
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
// TransNetV2 推理（可选，暂为占位）
// =========================================================================

float SceneDetector::transnetPredict(const QImage& current,
                                      const QImage& previous)
{
    // TODO: 实现 TransNetV2 推理
    // TransNetV2 需要 27 帧滑动窗口输入，不能只用 2 帧比较
    // 完整实现需要维护帧缓冲区，收集 27 帧后批量推理
    //
    // 临时回退到直方图差异
    return histogramDifference(current, previous);
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
