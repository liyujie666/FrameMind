#pragma once
#include "service/agent/tool_base.h"
#include <QPointer>
class VideoRAGStore;
class GetSemanticUnitTool : public ITool {
public:
    explicit GetSemanticUnitTool(VideoRAGStore*);
    QString name() const override {return QStringLiteral("get_semantic_unit");}
    QString description() const override;
    QJsonObject parameters() const override;
    void executeAsync(const QString&,const QJsonObject&,std::function<void(const ToolResult&)>) override;
    void setVideoId(const QString& id) {m_videoId=id;}
private:
    QPointer<VideoRAGStore> m_store;
    QString m_videoId;
};
