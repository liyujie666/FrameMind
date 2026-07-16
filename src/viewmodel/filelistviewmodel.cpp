#include "viewmodel/filelistviewmodel.h"

#include "service/filemanagerservice.h"
#include "service/playerservice.h"
#include "infrastructure/eventbus.h"
#include "util/mediaprobe.h"

#include <QFileInfo>
#include <QDir>
#include <QLocale>
#include <QSet>
#include <QHash>
#include <QThread>
#include <QElapsedTimer>
#include <algorithm>

FileListViewModel::FileListViewModel(FileManagerService* fileService,
                                     EventBus* eventBus,
                                     PlayerService* playerService,
                                     QObject* parent)
    : QAbstractListModel(parent)
    , m_fileService(fileService)
    , m_eventBus(eventBus)
    , m_playerService(playerService)
{
    if (m_fileService) {
        connect(m_fileService, &FileManagerService::recentFilesChanged,
                this, &FileListViewModel::onRecentFilesChanged);
    }
    if (m_eventBus) {
        // 视频打开时：记录待截图路径，同时刷新列表使新条目上榜
        connect(m_eventBus, &EventBus::videoOpened, this,
                [this](const QString& path) {
                    m_pendingThumbForPath = path;
                    refresh();
                });
    }
    if (m_playerService) {
        // 首帧解码后回填缩略图（连一次即可，之后每帧都会到达；只在有 pending 时写盘）
        connect(m_playerService, &PlayerService::frameDecoded,
                this, &FileListViewModel::onPlayerFrameDecoded,
                Qt::QueuedConnection);
    }

    refresh();
}

FileListViewModel::~FileListViewModel()
{
    // QPointer watcher's auto-delete 由 QObject parent 控制；显式清空一次保证析构顺序
    const auto watchers = m_probeWatchers.values();
    m_probeWatchers.clear();
    for (const auto& w : watchers) {
        if (!w) continue;
        w->disconnect();
        w->cancel();
        // Qt6 的 QFutureWatcher::waitForFinished() 无超时参数；析构期短轮询避免长 probe 阻塞进程退出
        QElapsedTimer t; t.start();
        while (w->isRunning() && t.elapsed() < 5000) {
            QThread::msleep(10);
        }
        w->deleteLater();
    }
}

int FileListViewModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) return 0;
    return m_items.size();
}

QVariant FileListViewModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size())
        return {};

    const VideoFileItem& it = m_items.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
    case DisplayNameRole:
        return it.displayName;
    case Qt::ToolTipRole:
        return it.path;
    case PathRole:
        return it.path;
    case SizeRole:
        return it.sizeBytes;
    case LastOpenedRole:
        return it.lastOpened;
    case ThumbnailPathRole:
        return it.thumbnailPath;
    case DurationRole:
        return it.durationMs;
    default:
        return {};
    }
}

QHash<int, QByteArray> FileListViewModel::roleNames() const
{
    return {
        { PathRole,          "path" },
        { DisplayNameRole,   "displayName" },
        { SizeRole,          "sizeBytes" },
        { LastOpenedRole,    "lastOpened" },
        { ThumbnailPathRole, "thumbnailPath" },
        { DurationRole,      "durationMs" },
    };
}

QString FileListViewModel::pathAt(int row) const
{
    if (row < 0 || row >= m_items.size()) return {};
    return m_items.at(row).path;
}

void FileListViewModel::addFromDirectory(const QString& dir)
{
    if (!m_fileService || dir.isEmpty()) return;
    const auto scanned = m_fileService->scanDirectory(dir);
    if (scanned.isEmpty()) return;

    beginResetModel();
    // 已存在的按 path 去重，将扫描项合并到末尾
    QSet<QString> exist;
    for (const auto& it : m_items) exist.insert(it.path);
    for (const auto& it : scanned) {
        if (!exist.contains(it.path)) m_items.append(it);
    }
    endResetModel();

    // 新加入的文件：启动异步 probe 补 duration/thumbnail
    enqueueMissingProbes();
}

void FileListViewModel::refresh()
{
    if (!m_fileService) return;
    beginResetModel();
    m_items = m_fileService->recentFiles(120);
    endResetModel();

    enqueueMissingProbes();
}

void FileListViewModel::removeAt(int row)
{
    if (row < 0 || row >= m_items.size()) return;
    const QString path = m_items.at(row).path;
    if (m_fileService) m_fileService->removeFromRecent(path);

    // 取消该行以及后续所有按 row 索引的 watcher（它们的 row 在 removeAt 后会偏移）
    QList<int> toRemove;
    for (auto it = m_probeWatchers.begin(); it != m_probeWatchers.end(); ++it) {
        if (it.key() >= row) toRemove.append(it.key());
    }
    for (int k : toRemove) {
        auto it = m_probeWatchers.find(k);
        if (it != m_probeWatchers.end()) {
            if (*it) {
                (*it)->disconnect();
                (*it)->cancel();
                QElapsedTimer t; t.start();
                while ((*it)->isRunning() && t.elapsed() < 2000) {
                    QThread::msleep(10);
                }
                (*it)->deleteLater();
            }
            m_probeWatchers.erase(it);
        }
    }

    beginRemoveRows({}, row, row);
    m_items.removeAt(row);
    endRemoveRows();
}

void FileListViewModel::onRecentFilesChanged()
{
    refresh();
}

void FileListViewModel::onPlayerFrameDecoded(const QImage& frame)
{
    if (m_pendingThumbForPath.isEmpty() || frame.isNull()) return;
    if (!m_fileService) return;

    // 只用首帧写盘一次，写完就清标记
    const QString path = m_pendingThumbForPath;
    m_pendingThumbForPath.clear();
    m_fileService->saveThumbnail(path, frame);
    // saveThumbnail 会发 recentFilesChanged，间接 refresh 更新缩略图路径
}

// ---------------------------------------------------------------------------
// 异步 probe 调度
// ---------------------------------------------------------------------------

void FileListViewModel::enqueueMissingProbes()
{
    // 跳过时长已知的条目；缩略图单独靠 saveThumbnail/open 流程补
    for (int i = 0; i < m_items.size(); ++i) {
        const auto& it = m_items.at(i);
        if (it.durationMs > 0) continue;
        if (m_probeWatchers.contains(i)) continue;

        const QString p = it.path;
        // 只处理本地存在文件；流/URL 跳过
        const QFileInfo fi(p);
        if (!fi.exists()) continue;

        const QString lower = p.toLower();
        if (lower.startsWith(QStringLiteral("rtsp")) ||
            lower.startsWith(QStringLiteral("rtmp")) ||
            lower.startsWith(QStringLiteral("http"))) continue;

        auto* watcher = new QFutureWatcher<MediaProbeResult>(this);
        const QString mediaPath = p;
        watcher->setFuture(MediaProbe().probeAsync(mediaPath, 480));

        // row 必须按值捕获：watcher 完成时 m_items 可能已经重排
        const int row = i;
        connect(watcher, &QFutureWatcher<MediaProbeResult>::finished,
                this, [this, watcher, row, mediaPath]() {
            if (!watcher) return;
            const auto r = watcher->result();
            watcher->deleteLater();

            // 越界（refresh 后）→ 丢弃
            if (row < 0 || row >= m_items.size()) return;

            VideoFileItem& it2 = m_items[row];
            // 路径检查：若 refresh 后该行的 path 已经不同，丢弃本次结果
            if (it2.path != mediaPath) return;

            bool changed = false;
            if (r.durationMs > 0 && it2.durationMs != r.durationMs) {
                it2.durationMs = r.durationMs;
                changed = true;
                if (m_fileService) m_fileService->updateDurationMs(it2.path, r.durationMs);
            }
            if (!r.thumbnail.isEmpty() && it2.thumbnailPath != r.thumbnail) {
                it2.thumbnailPath = r.thumbnail;
                changed = true;
            }
            if (changed) {
                const QModelIndex idx = index(row);
                emit dataChanged(idx, idx,
                                 { ThumbnailPathRole, DurationRole });
            }
            m_probeWatchers.remove(row);
        });

        m_probeWatchers.insert(i, watcher);
    }
}

void FileListViewModel::onProbeFinished(int /*row*/)
{
    // 仅作接口占位，便于后续扩展；实际逻辑由 lambda 直接处理
}
