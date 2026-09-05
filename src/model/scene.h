#ifndef FRAMEMIND_SCENE_H
#define FRAMEMIND_SCENE_H

#include <QString>
#include <QStringList>
#include <QImage>
#include <QMetaType>
#include <QVector>
#include <cstdint>

#include "model/audio_visual_relation.h"

/** 场景内的代表帧；ptsMs 是实际解码展示时间，避免把请求采样点误作帧时间。 */
struct SceneFrame {
    int64_t requestedMs = 0;
    int64_t ptsMs = 0;
    QString imagePath;
    QImage image;
};

/**
 * 视频场景（agent-core-design.md §2.2 结构层 SceneGraph）。
 *
 * 场景由 SceneDetector 产出，代表一段视觉上相对稳定的视频片段。
 * keyframe 与 keyframePath 是兼容旧 UI 的主代表帧；representativeFrames
 * 记录场景内多个时间状态，用于多帧视觉索引和 VLM 描述。
 */
struct Scene {
    int      id = -1;
    int64_t  startMs = 0;
    int64_t  endMs   = 0;

    /// 场景代表帧（首帧或中间帧）
    int64_t  keyframeMs = 0;
    QString  keyframePath;         // 磁盘路径（相对于 appData/keyframes/<videoId>/）
    QImage   keyframe;             // 可选：内存中的关键帧
    QVector<SceneFrame> representativeFrames;

    /// 场景摘要（融合后的最终描述；未描述时为空）
    QString  description;

    // ===== 音视频融合三类证据（互不覆盖）=====

    /// 纯视觉描述：仅由关键帧生成，禁止音频参与，作为不可污染的事实基线
    QString  visualDescription;

    /// 由视觉模型转录且明确可辨的画面文字（后续可由专用 OCR 实现替换/补充）
    QStringList visibleTexts;
    /// 由多代表帧观察到的动作/状态变化
    QStringList visibleActions;

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
