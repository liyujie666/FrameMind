#include "service/agent/tools/search_video_content_tool.h"

#include "service/rag/video_rag_retriever.h"

#include <QJsonArray>
#include <QJsonObject>

SearchVideoContentTool::SearchVideoContentTool(VideoRAGRetriever* retriever)
    : m_retriever(retriever)
{
}

QString SearchVideoContentTool::description() const
{
    return QStringLiteral(
        "在视频中搜索符合描述的画面，返回匹配的时间点列表。"
        "用于'什么时候出现了 X'、'找到 X 的片段'等查询。");
}

QJsonObject SearchVideoContentTool::parameters() const
{
    return QJsonObject{
        { QStringLiteral("type"), QStringLiteral("object") },
        { QStringLiteral("properties"), QJsonObject{
            { QStringLiteral("query"), QJsonObject{
                { QStringLiteral("type"), QStringLiteral("string") },
                { QStringLiteral("description"), QStringLiteral("搜索描述，如'红色汽车的画面'") } } },
            { QStringLiteral("top_k"), QJsonObject{
                { QStringLiteral("type"), QStringLiteral("integer") },
                { QStringLiteral("default"), 5 } } },
            { QStringLiteral("time_range"), QJsonObject{
                { QStringLiteral("type"), QStringLiteral("array") },
                { QStringLiteral("items"), QJsonObject{
                    { QStringLiteral("type"), QStringLiteral("integer") } } },
                { QStringLiteral("description"), QStringLiteral("可选 [start_ms, end_ms]") } } }
        }},
        { QStringLiteral("required"), QJsonArray{ QStringLiteral("query") } }
    };
}

void SearchVideoContentTool::executeAsync(const QString& callId,
                                            const QJsonObject& args,
                                            std::function<void(const ToolResult&)> done)
{
    if (!m_retriever) {
        done(ToolResult::fail(callId, name(), QStringLiteral("Retriever 未注入")));
        return;
    }
    const QString query = args.value(QStringLiteral("query")).toString();
    const int topK = args.value(QStringLiteral("top_k")).toInt(5);
    if (query.isEmpty()) {
        done(ToolResult::fail(callId, name(), QStringLiteral("query 为空")));
        return;
    }

    VideoRAGRetriever::Constraints c;
    c.videoId = m_videoId;
    const auto tr = args.value(QStringLiteral("time_range")).toArray();
    if (tr.size() >= 2) {
        c.startMsGte = tr[0].toVariant().toLongLong();
        c.endMsLte   = tr[1].toVariant().toLongLong();
    }

    const auto hits = m_retriever->retrieve(query, c, topK);

    QJsonArray results;
    for (const auto& h : hits) {
        QJsonObject o;
        o.insert(QStringLiteral("timestamp_ms"), static_cast<qint64>(h.chunk.startMs));
        o.insert(QStringLiteral("end_ms"),       static_cast<qint64>(h.chunk.endMs));
        o.insert(QStringLiteral("score"),        h.score);
        o.insert(QStringLiteral("hit_path"),     h.hitPath);
        o.insert(QStringLiteral("description"),  h.chunk.textContent);
        results.append(o);
    }
    QJsonObject data;
    data.insert(QStringLiteral("query"), query);
    data.insert(QStringLiteral("results"), results);
    done(ToolResult::ok(callId, name(), data));
}
