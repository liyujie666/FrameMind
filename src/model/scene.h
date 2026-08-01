#ifndef FRAMEMIND_SCENE_H
#define FRAMEMIND_SCENE_H

#include <QString>
#include <QImage>
#include <QMetaType>
#include <cstdint>

#include "model/audio_visual_relation.h"

/**
 * 视频场景（agent-core-design.md §2.2 结构层 SceneGraph）。
 *
 * 场景由 SceneDetector 产出，代表一段视觉上相对稳定的视频片段。
 * keyframe 与 keyframePath 二选一：内存驻留可用 QImage，
 * 大视频/持久化建议只保留磁盘路径。
 */
struct Scene {
    int      id = -1;
    int64_t  startMs = 0;
    int64_t  endMs   = 0;

    /// 场景代表帧（首帧或中间帧）
    int64_t  keyframeMs = 0;
    QString  keyframePath;         // 磁盘路径（相对于 appData/keyframes/<videoId>/）
    QImage   keyframe;             // 可选：内存中的关键帧

    /// 场景摘要（融合后的最终描述；未描述时为空）
    QString  description;

    // ===== 音视频融合三类证据（互不覆盖）=====

    /// 纯视觉描述：仅由关键帧生成，禁止音频参与，作为不可污染的事实基线
    QString  visualDescription;

    /// 同期音频的独立摘要（对白/旁白/背景媒体说了什么）
    QString  audioSummary;

    /// 保守融合描述：视觉事实 + 同期音频，按 audioRelation 控制归因强度
    QString  fusedDescription;

    AudioVisualRelation audioRelation = AudioVisualRelation::Unknown;
    float               audioRelationConfidence = 0.0f;
    SceneAudioType      audioType = SceneAudioType::None;

    int64_t durationMs() const { return endMs - startMs; }
    bool contains(int64_t posMs) const { return posMs >= startMs && posMs < endMs; }
    bool isValid() const { return id >= 0 && endMs > startMs; }
};

Q_DECLARE_METATYPE(Scene)

#endif // FRAMEMIND_SCENE_H
