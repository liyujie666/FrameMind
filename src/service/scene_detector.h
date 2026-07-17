#ifndef FRAMEMIND_SCENE_DETECTOR_H
#define FRAMEMIND_SCENE_DETECTOR_H

#include <QObject>
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
 * 场景分割服务。
 *
 * 两种模式：
 *   1. 直方图差异（默认）— 纯 Qt/OpenCV 算法，无需模型，速度极快
 *      适合 M3 第一阶段快速验证
 *   2. TransNetV2（可选）— 深度学习场景检测，需加载 ONNX 模型
 *      当直方图差异精度不足时升级（需要 FRAMEMIND_HAS_ONNXRUNTIME）
 *
 * 调用流程：
 *   SceneDetector detector;
 *   detector.setThreshold(0.3f);
 *   // 逐帧或按间隔采样调用
 *   float diff = detector.computeDifference(currentFrame, prevFrame);
 *   if (diff > threshold) { /* 场景切换 */ }
 *
 *   或一次性处理整个视频的采样帧序列：
 *   auto scenes = detector.detectScenes(frames, timestamps);
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
    /// @param frames 均匀采样的帧序列
    /// @param timestampsMs 每帧对应的时间戳(ms)
    /// @return 场景列表（keyframe 取每个场景的首帧）
    QVector<Scene> detectScenes(const QVector<QImage>& frames,
                                 const QVector<int64_t>& timestampsMs);

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

    /// TransNetV2 推理（可选）
    float transnetPredict(const QImage& current, const QImage& previous);

    /// 降采样
    QImage downscale(const QImage& img) const;

    float                                   m_threshold = 0.3f;
    int                                     m_downscaleSize = 64;
    bool                                    m_useTransNet = false;
#ifdef FRAMEMIND_HAS_ONNXRUNTIME
    std::unique_ptr<OnnxRuntimeEngine>      m_transnetEngine;
#endif
};

#endif // FRAMEMIND_SCENE_DETECTOR_H
