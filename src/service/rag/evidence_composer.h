#ifndef FRAMEMIND_EVIDENCE_COMPOSER_H
#define FRAMEMIND_EVIDENCE_COMPOSER_H

#include <QList>
#include <QImage>
#include <QJsonArray>
#include <QString>
#include <QVector>

#include "model/retrieval_result.h"

/**
 * 将检索命中转换为可追溯的文本与视觉证据包。
 *
 * 文字证据进入 VideoContext，原始关键帧进入 VLM 请求，避免仅依赖
 * caption/摘要造成视觉细节在生成阶段丢失。
 */
class EvidenceComposer final
{
public:
    static QString formatText(const QVector<RetrievalResult>& evidence,
                              int maxItems = 6,
                              int maxCharsPerItem = 240);

    static QList<QImage> mergeFrames(const QList<QImage>& userFrames,
                                     const QVector<RetrievalResult>& evidence,
                                     int maxEvidenceFrames = 3,
                                     int maxFrameEdge = 1280);

    /// 只持久化回答追溯需要的字段；embedding 不进入 checkpoint，避免无意义膨胀。
    static QJsonArray toJson(const QVector<RetrievalResult>& evidence);
    static QVector<RetrievalResult> fromJson(const QJsonArray& json);

private:
    static QString formatMs(int64_t ms);
    static QString hitPathLabel(const QString& hitPath);
};

#endif // FRAMEMIND_EVIDENCE_COMPOSER_H
