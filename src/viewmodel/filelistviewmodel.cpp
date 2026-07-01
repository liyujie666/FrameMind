#include "viewmodel/filelistviewmodel.h"

#include "service/filemanagerservice.h"
#include "service/playerservice.h"
#include "infrastructure/eventbus.h"

#include <QFileInfo>
#include <QDir>
#include <QLocale>
#include <QSet>

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
}

void FileListViewModel::refresh()
{
    if (!m_fileService) return;
    beginResetModel();
    m_items = m_fileService->recentFiles(120);
    endResetModel();
}

void FileListViewModel::removeAt(int row)
{
    if (row < 0 || row >= m_items.size()) return;
    const QString path = m_items.at(row).path;
    if (m_fileService) m_fileService->removeFromRecent(path);
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
