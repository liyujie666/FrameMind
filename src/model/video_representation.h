#ifndef FRAMEMIND_VIDEO_REPRESENTATION_H
#define FRAMEMIND_VIDEO_REPRESENTATION_H

#include <QString>
#include <QVector>
#include <QMap>
#include <QMetaType>
#include <cstdint>

#include "model/videoinfo.h"
#include "model/scene.h"
#include "model/speech_segment.h"
#include "model/entity_profile.h"

/**
 * 视频三层表示（agent-core-design.md §2.1）：感知层 / 结构层 / 语义层。
 *
 * Agent 日常在语义层与结构层工作，只在必要时下探到感知层。
 * 由 VideoIndexer 按 Level 0/1/2 渐进式填充。
 */
struct VideoRepresentation {
    // ===== 通用元信息 =====
    QString   videoId;          // 基于 fileHash 生成
    VideoInfo metadata;         // 感知层元信息（时长/分辨率/fps/有无音频）

    // ===== 结构层 =====
    QVector<Scene>            scenes;             // 场景分割
    QVector<SpeechSegment>    speechSegments;     // 语音转写段
    QVector<EntityProfile>    entities;           // 实体档案

    // ===== 语义层 =====
    QString  videoSummary;                        // 全视频摘要
    QMap<int, QString> sceneDescriptions;         // sceneId → 结构化描述(JSON字符串)

    // ===== 索引进度 =====
    /**
     * 索引级别：
     *   L0 = metadata + 场景骨架（秒级）
     *   L1 = 关键帧 embedding + ASR（几秒到几十秒）
     *   L2 = 场景描述 + 全视频摘要（按需/后台）
     *   L3 = 局部深度分析（用户提问时触发，不整体标记）
     */
    enum IndexLevel {
        NotIndexed = -1,
        Level0     = 0,
        Level1     = 1,
        Level2     = 2
    };
    IndexLevel level = NotIndexed;

    bool isValid() const { return !videoId.isEmpty() && metadata.durationMs > 0; }

    /// 通过时间戳定位所在场景 ID（找不到返回 -1）
    int sceneIdAt(int64_t posMs) const
    {
        for (const Scene& s : scenes) {
            if (posMs >= s.startMs && posMs < s.endMs) return s.id;
        }
        return -1;
    }
};

Q_DECLARE_METATYPE(VideoRepresentation)

#endif // FRAMEMIND_VIDEO_REPRESENTATION_H
