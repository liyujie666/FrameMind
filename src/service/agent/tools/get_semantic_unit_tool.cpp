#include "service/agent/tools/get_semantic_unit_tool.h"
#include "service/rag/video_rag_store.h"
GetSemanticUnitTool::GetSemanticUnitTool(VideoRAGStore* store):m_store(store) {}
QString GetSemanticUnitTool::description() const {return QStringLiteral("读取当前视频活动构建的主题、问答、知识点或步骤。可按时间/单元ID定位，沿previous/next展开。原始证据分页读取；证据不足时调用analyze_time_range局部复核。");}
QJsonObject GetSemanticUnitTool::parameters() const {
    return {{"type","object"},{"properties",QJsonObject{
        {"unit_id",QJsonObject{{"type","string"}}},{"build_id",QJsonObject{{"type","string"}}},
        {"timestamp_ms",QJsonObject{{"type","integer"}}},{"relation",QJsonObject{{"type","string"},{"enum",QJsonArray{"current","previous","next"}}}},
        {"source_offset",QJsonObject{{"type","integer"}}},{"source_limit",QJsonObject{{"type","integer"}}}}}};
}
void GetSemanticUnitTool::executeAsync(const QString& call,const QJsonObject& args,std::function<void(const ToolResult&)> done) {
    if(!m_store || m_videoId.isEmpty()) {done(ToolResult::fail(call,name(),QStringLiteral("当前视频未就绪")));return;}
    const auto active=m_store->activeBuild(m_videoId);
    if(active.buildId.isEmpty() || (!args["build_id"].toString().isEmpty() && args["build_id"].toString()!=active.buildId)) {done(ToolResult::fail(call,name(),QStringLiteral("语义构建未就绪或版本已变化，请重新检索")));return;}
    auto units=m_store->listUnits(active.buildId);SemanticUnit unit;
    for(const auto& candidate:units) if((!args["unit_id"].toString().isEmpty() && candidate.unitId==args["unit_id"].toString())
        || (args["unit_id"].toString().isEmpty() && candidate.startMs<=args["timestamp_ms"].toVariant().toLongLong() && candidate.endMs>args["timestamp_ms"].toVariant().toLongLong())) {unit=candidate;break;}
    const QString relation=args["relation"].toString("current");
    if(relation=="next") unit=m_store->getUnit(active.buildId,unit.nextUnitId);
    else if(relation=="previous") unit=m_store->getUnit(active.buildId,unit.previousUnitId);
    if(!unit.isValid()) {done(ToolResult::fail(call,name(),QStringLiteral("未找到语义单元或相邻步骤")));return;}
    const auto raw=m_store->rawChunks(active.rawSnapshotId);QHash<QString,VideoChunk> byId;for(const auto& c:raw) byId[c.chunkId]=c;
    const int offset=qMax(0,args["source_offset"].toInt()),limit=qBound(1,args["source_limit"].toInt(20),100);QJsonArray sources;
    for(const auto& id:unit.sourceChunkIds.mid(offset,limit)) if(byId.contains(id)) {const auto c=byId[id];sources.append(QJsonObject{{"chunk_id",id},{"start_ms",qint64(c.startMs)},{"end_ms",qint64(c.endMs)},{"text",c.textContent},{"frame_path",c.keyframePath},{"metadata",QJsonObject::fromVariantMap(c.metadata)}});}
    auto data=unit.toJson();data["video_id"]=m_videoId;data["raw_snapshot_id"]=active.rawSnapshotId;data["revision"]=active.revision;data["sources"]=sources;data["source_count"]=unit.sourceChunkIds.size();data["next_source_offset"]=offset+limit<unit.sourceChunkIds.size()?offset+limit:-1;
    done(ToolResult::ok(call,name(),data));
}
