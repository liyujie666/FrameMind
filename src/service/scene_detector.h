#ifndef FRAMEMIND_SCENE_DETECTOR_H
#define FRAMEMIND_SCENE_DETECTOR_H

#include <QObject>
#include <QFuture>
#include <QImage>
#include <QVector>
#include <cstdint>
#include <memory>
#include <vector>

#include "model/scene.h"

#ifdef FRAMEMIND_HAS_ONNXRUNTIME
class OnnxRuntimeEngine;
#endif

/**
 * 场景分割服务（两阶段混合策略）。
 *
 * 阶段 1 — 粗分割：
 *   对稀疏采样帧（每 10s 一帧）用直方图 Bhattacharyya 距离检测候选边界。
 *   速度极快，适合任意采样间隔。
 *
 * 阶段 2 — 精确确认（可选，需 TransNetV2）：
 *   对每个候选边界附近做局部密集采样（1 fps），将连续帧送入 TransNetV2
 *   确认精确切换位置。若 TransNetV2 未加载则跳过此阶段。
 *
 * 调用流程：
 *   SceneDetector detector;
 *   detector.setThreshold(0.3f);
 *   auto scenes = detector.detectScenes(frames, timestamps);
 *
 * 或两阶段分离调用：
 *   auto candidates = detector.detectCandidateBoundaries(sparseFrames, timestamps);
 *   auto refined = detector.refineBoundaries(denseFrames, denseTimestamps);
 */
class SceneDetector : public QObject {
    Q_OBJECT
public:
    explicit SceneDetector(QObject* parent = nullptr);
    ~SceneDetector() override;

    // ---- 配置 ----

    /// 设置场景切换阈值 (0~1, 默认 0.3)
    /// 值越大越不容易触发切换（更保守）
    void setThreshold(float threshold) { m_threshold = threshold; }
    float threshold() const { return m_threshold; }

    /// 设置采样帧降采样尺寸（默认 64x64，越小越快）
    void setDownscaleSize(int size) { m_downscaleSize = size; }

    /// 可选：加载 TransNetV2 ONNX 模型
    /// 加载后自动切换到 TransNetV2 模式
    bool loadTransNetV2(const QString& modelPath);

    /// 是否使用 TransNetV2
    bool isUsingTransNet() const { return m_useTransNet; }

    // ---- 逐帧检测 ----

    /// 计算两帧之间的差异 (0~1，越大越可能是场景切换)
    float computeDifference(const QImage& current, const QImage& previous);

    /// 判断是否发生场景切换
    bool isSceneChange(const QImage& current, const QImage& previous);

    // ---- 批量检测 ----

    /// 对一组采样帧执行场景分割，返回场景列表
    /// 稀疏采样时自动使用直方图算法；密集采样且 TransNetV2 可用时使用深度学习
    /// @param frames 采样的帧序列
    /// @param timestampsMs 每帧对应的时间戳(ms)
    /// @return 场景列表（keyframe 取每个场景的首帧）
    QVector<Scene> detectScenes(const QVector<QImage>& frames,
                                 const QVector<int64_t>& timestampsMs);

    /// 阶段 1：直方图粗分割，返回候选边界时间戳列表
    /// 每个元素是 (boundaryTimestampMs, score)
    QVector<QPair<int64_t, float>> detectCandidateBoundaries(
        const QVector<QImage>& frames,
        const QVector<int64_t>& timestampsMs);

    /// 阶段 2：用 TransNetV2 对密集采样帧做精确边界检测
    /// 输入应为连续帧序列（1~3 fps），返回确认的边界时间戳
    QVector<int64_t> refineBoundariesWithTransNet(
        const QVector<QImage>& denseFrames,
        const QVector<int64_t>& denseTimestampsMs);

    /// 异步批量检测
    QFuture<QVector<Scene>> detectScenesAsync(
        const QVector<QImage>& frames,
        const QVector<int64_t>& timestampsMs);

signals:
    /// 检测到场景切换
    void sceneBoundaryDetected(int64_t timestampMs, float difference);

private:
    /// 直方图差异计算（纯 Qt 实现）
    float histogramDifference(const QImage& a, const QImage& b);

    /// TransNetV2 批量推理：对整个帧序列一次性计算所有切换概率
    /// 返回长度为 frames.size() 的概率向量，probs[i] 表示第 i 帧与第 i+1 帧之间的切换概率
    std::vector<float> transnetBatchPredict(const QVector<QImage>& frames);

    /// 将单帧预处理为 TransNetV2 所需格式：resize 到 48x27，归一化到 [0,1]，RGB float
    /// 写入 dst，共 27×48×3 个 float
    void preprocessTransnetFrame(const QImage& img, float* dst) const;

    /// 降采样（直方图模式用）
    QImage downscale(const QImage& img) const;

    // TransNetV2 模型参数（由 ONNX shape [1,100,27,48,3] 确定）
    // 输入语义: [batch, n_frames, H, W, C]
    static constexpr int kTransnetH      = 27;   // 模型输入帧高
    static constexpr int kTransnetW      = 48;   // 模型输入帧宽
    static constexpr int kTransnetC      = 3;    // 模型输入通道数（RGB）
    static constexpr int kTransnetBatch  = 100;  // 每次最多处理帧数

    float                                   m_threshold = 0.3f;
    int                                     m_downscaleSize = 64;
    bool                                    m_useTransNet = false;
#ifdef FRAMEMIND_HAS_ONNXRUNTIME
    std::unique_ptr<OnnxRuntimeEngine>      m_transnetEngine;
#endif
};

#endif // FRAMEMIND_SCENE_DETECTOR_H
