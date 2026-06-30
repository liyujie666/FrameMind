#ifndef FRAMEMIND_VIDEOCONTEXT_H
#define FRAMEMIND_VIDEOCONTEXT_H

#include <QString>
#include <cstdint>

/**
 * 视频上下文：注入到 System Prompt 的视频元信息 + 结构概览 + 摘要。
 *
 * M2 阶段一般为空（最小 system prompt）；M3 起由 VideoAnalysisService 填充。
 * 字段对齐 api-protocol.md §4.1 模板。
 */
struct VideoContext {
    QString fileName;
    int64_t durationMs = 0;
    int     width = 0;
    int     height = 0;
    double  fps = 0.0;
    bool    hasAudio = false;
    QString sceneOverview;   // 场景概览
    QString videoSummary;    // 视频摘要

    bool isEmpty() const
    {
        return fileName.isEmpty() && sceneOverview.isEmpty()
               && videoSummary.isEmpty();
    }
};

#endif // FRAMEMIND_VIDEOCONTEXT_H
