#include "viewmodel/knowledgeviewmodel.h"

#include "service/rag/video_rag_store.h"
#include "service/rag/video_rag_retriever.h"
#include "infrastructure/databasemanager.h"

#include <QFileInfo>
#include <QDateTime>
#include <QDebug>

KnowledgeViewModel::KnowledgeViewModel(VideoRAGStore*     ragStore,
                                       VideoRAGRetriever* retriever,
                                       DatabaseManager*   db,
                                       QObject*           parent)
    : QObject(parent)
    , m_ragStore(ragStore)
    , m_retriever(retriever)
    , m_db(db)
{
}

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

KnowledgeViewModel::VideoIndexSummary
KnowledgeViewModel::buildSummary(const QString& videoId) const
{
    VideoIndexSummary s;
    s.videoId = videoId;

    const auto visualChunks = m_ragStore->listChunks(VideoRAGStore::VisualFrames, videoId);
    const auto textChunks   = m_ragStore->listChunks(VideoRAGStore::TextSegments, videoId);
    const auto qaChunks     = m_ragStore->listChunks(VideoRAGStore::QACache,      videoId);

    s.visualCount  = visualChunks.size();
    s.textCount    = textChunks.size();
    s.qaCacheCount = qaChunks.size();
    s.totalChunks  = s.visualCount + s.textCount + s.qaCacheCount;

    // Index level
    bool hasVLM = false;
    for (const auto& c : textChunks) {
        if (c.chunkType == VideoChunk::SceneSummary) { hasVLM = true; break; }
    }
    if (hasVLM)             s.level = 2;
    else if (s.textCount)   s.level = 1;
    else if (s.visualCount) s.level = 0;
    else                    s.level = -1;

    // Read file_path from chunk metadata (written by VideoIndexer)
    auto tryFilePath = [&](const QVector<VideoChunk>& chunks) {
        for (const auto& c : chunks) {
            const QString p = c.metadata.value(QStringLiteral("file_path")).toString();
            if (!p.isEmpty()) return p;
        }
        return QString{};
    };
    s.filePath = tryFilePath(textChunks);
    if (s.filePath.isEmpty()) s.filePath = tryFilePath(visualChunks);
    if (s.filePath.isEmpty()) s.filePath = tryFilePath(qaChunks);

    // Last indexed time from DB
    if (m_db) {
        const auto rows = m_db->query(
            QStringLiteral(
                "SELECT MAX(created_at) AS last_indexed FROM rag_chunks "
                "WHERE video_id = ?"),
            { videoId });
        if (!rows.isEmpty()) {
            const QString dt = rows.first().value(QStringLiteral("last_indexed")).toString();
            if (!dt.isEmpty())
                s.lastIndexed = QDateTime::fromString(dt, Qt::ISODate);
        }
    }

    s.fileName = s.filePath.isEmpty()
        ? videoId.left(16) + QStringLiteral("...")
        : QFileInfo(s.filePath).fileName();

    return s;
}

// -----------------------------------------------------------------------
// Public slots
// -----------------------------------------------------------------------

void KnowledgeViewModel::loadIndexedVideos()
{
    if (!m_db) {
        emit statusMessage(tr("数据库未就绪"), true);
        return;
    }

    // Collect distinct video_ids from rag_chunks
    const auto rows = m_db->query(
        QStringLiteral(
            "SELECT DISTINCT video_id FROM rag_chunks ORDER BY created_at DESC"));

    m_videos.clear();
    for (const auto& row : rows) {
        const QString vid = row.value(QStringLiteral("video_id")).toString();
        if (vid.isEmpty()) continue;
        m_ragStore->loadVideo(vid);
        m_videos.append(buildSummary(vid));
    }

    emit indexedVideosChanged(m_videos);
}

void KnowledgeViewModel::selectVideo(const QString& videoId)
{
    if (videoId == m_selectedVideoId) return;
    m_selectedVideoId  = videoId;
    m_activeCollection = -1;
    m_searchResults.clear();

    if (videoId.isEmpty()) {
        m_allChunks.clear();
        m_chunks.clear();
        emit chunksChanged(m_chunks);
        return;
    }

    m_ragStore->loadVideo(videoId);
    filterChunks(-1);
}

void KnowledgeViewModel::filterChunks(int collection)
{
    m_activeCollection = collection;

    if (m_selectedVideoId.isEmpty()) {
        m_chunks.clear();
        emit chunksChanged(m_chunks);
        return;
    }

    // Always re-read all collections when switching
    if (collection < 0) {
        m_allChunks.clear();
        for (int col : { (int)VideoRAGStore::VisualFrames,
                         (int)VideoRAGStore::TextSegments,
                         (int)VideoRAGStore::EntityProfiles,
                         (int)VideoRAGStore::QACache }) {
            const auto c = m_ragStore->listChunks(
                static_cast<VideoRAGStore::Collection>(col), m_selectedVideoId);
            m_allChunks.append(c);
        }
        // Sort by start time
        std::sort(m_allChunks.begin(), m_allChunks.end(),
                  [](const VideoChunk& a, const VideoChunk& b) {
                      return a.startMs < b.startMs;
                  });
        m_chunks = m_allChunks;
    } else {
        m_chunks = m_ragStore->listChunks(
            static_cast<VideoRAGStore::Collection>(collection), m_selectedVideoId);
        std::sort(m_chunks.begin(), m_chunks.end(),
                  [](const VideoChunk& a, const VideoChunk& b) {
                      return a.startMs < b.startMs;
                  });
    }

    emit chunksChanged(m_chunks);
}

void KnowledgeViewModel::removeVideoIndex(const QString& videoId)
{
    if (!m_ragStore || videoId.isEmpty()) return;
    m_ragStore->invalidateVideo(videoId);

    m_videos.erase(
        std::remove_if(m_videos.begin(), m_videos.end(),
                       [&](const VideoIndexSummary& s) { return s.videoId == videoId; }),
        m_videos.end());

    if (m_selectedVideoId == videoId) {
        m_selectedVideoId.clear();
        m_chunks.clear();
        m_allChunks.clear();
        emit chunksChanged(m_chunks);
    }

    emit videoIndexRemoved(videoId);
    emit indexedVideosChanged(m_videos);
    emit statusMessage(tr("已删除视频索引"));
}

void KnowledgeViewModel::cleanupStale(int maxAgeDays)
{
    if (!m_db) return;

    // Count before
    const auto before = m_db->query(
        QStringLiteral("SELECT COUNT(*) AS cnt FROM rag_chunks"));
    const int cntBefore = before.isEmpty() ? 0
                          : before.first().value(QStringLiteral("cnt")).toInt();

    m_ragStore->cleanupStale(maxAgeDays);

    const auto after = m_db->query(
        QStringLiteral("SELECT COUNT(*) AS cnt FROM rag_chunks"));
    const int cntAfter = after.isEmpty() ? 0
                         : after.first().value(QStringLiteral("cnt")).toInt();

    const int removed = qMax(0, cntBefore - cntAfter);
    emit staleIndexCleaned(removed);
    emit statusMessage(tr("已清理 %1 条过期知识条目").arg(removed));

    loadIndexedVideos();
}

void KnowledgeViewModel::testSearch(const QString& query)
{
    if (query.trimmed().isEmpty() || m_selectedVideoId.isEmpty()) return;
    if (!m_retriever) {
        emit statusMessage(tr("检索服务未就绪"), true);
        return;
    }

    m_searching = true;
    emit searchingChanged(true);

    VideoRAGRetriever::Constraints c;
    c.videoId = m_selectedVideoId;

    const QVector<RetrievalResult> results = m_retriever->retrieve(query, c, 8);
    m_searchResults = results;

    m_searching = false;
    emit searchingChanged(false);
    emit searchResultsReady(m_searchResults);
}

void KnowledgeViewModel::clearSearchResults()
{
    m_searchResults.clear();
    emit searchResultsReady(m_searchResults);
}
