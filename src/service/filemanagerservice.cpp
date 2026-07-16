#include "service/filemanagerservice.h"

#include "infrastructure/databasemanager.h"

#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QVariant>

FileManagerService::FileManagerService(DatabaseManager* db, QObject* parent)
    : QObject(parent)
    , m_db(db)
{}

QStringList FileManagerService::supportedVideoExtensions()
{
    return { "mp4", "mkv", "avi", "mov", "flv", "ts", "webm", "wmv", "m4v", "3gp" };
}

QString FileManagerService::thumbnailCacheDir() const
{
    const QString base =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString dir = base + QStringLiteral("/thumbnails");
    QDir().mkpath(dir);
    return dir;
}

QString FileManagerService::thumbnailPathFor(const QString& videoPath) const
{
    const QByteArray h =
        QCryptographicHash::hash(videoPath.toUtf8(),
                                 QCryptographicHash::Md5).toHex();
    return thumbnailCacheDir() + QStringLiteral("/") + QString::fromLatin1(h) + QStringLiteral(".jpg");
}

QList<VideoFileItem> FileManagerService::recentFiles(int limit) const
{
    QList<VideoFileItem> list;
    if (!m_db) return list;

    const auto rows = m_db->query(
        QStringLiteral("SELECT path, last_opened, duration_ms FROM recent_files "
                       "ORDER BY last_opened DESC LIMIT ?"),
        { QVariant(limit) });

    list.reserve(rows.size());
    for (const auto& row : rows) {
        const QString p = row.value(QStringLiteral("path")).toString();
        QFileInfo fi(p);
        if (!fi.exists()) continue;
        VideoFileItem item;
        item.path          = p;
        item.displayName   = fi.fileName();
        item.sizeBytes     = fi.size();
        item.lastOpened    = row.value(QStringLiteral("last_opened")).toDateTime();
        item.durationMs    = row.value(QStringLiteral("duration_ms")).toLongLong();
        const QString thumb = thumbnailPathFor(p);
        if (QFileInfo::exists(thumb)) item.thumbnailPath = thumb;
        list.append(item);
    }
    return list;
}

QList<VideoFileItem> FileManagerService::scanDirectory(const QString& dir) const
{
    QList<VideoFileItem> list;
    if (dir.isEmpty()) return list;

    QDir d(dir);
    if (!d.exists()) return list;

    QStringList filters;
    for (const QString& ext : supportedVideoExtensions()) {
        filters << QStringLiteral("*.") + ext;
    }

    const QFileInfoList files = d.entryInfoList(filters,
                                                QDir::Files | QDir::NoDotAndDotDot,
                                                QDir::Name);
    list.reserve(files.size());
    for (const QFileInfo& fi : files) {
        VideoFileItem item;
        item.path = fi.absoluteFilePath();
        item.displayName = fi.fileName();
        item.sizeBytes   = fi.size();
        const QString thumb = thumbnailPathFor(item.path);
        if (QFileInfo::exists(thumb)) item.thumbnailPath = thumb;
        list.append(item);
    }
    return list;
}

void FileManagerService::addToRecent(const QString& path)
{
    addToRecent(path, 0);
}

void FileManagerService::addToRecent(const QString& path, qint64 durationMs)
{
    if (!m_db || path.isEmpty()) return;
    QFileInfo fi(path);
    if (!fi.exists()) return;

    m_db->exec(QStringLiteral(
        "INSERT INTO recent_files(path, last_opened, duration_ms) VALUES(?, CURRENT_TIMESTAMP, ?) "
        "ON CONFLICT(path) DO UPDATE SET "
        "  last_opened=CURRENT_TIMESTAMP, "
        "  duration_ms=CASE WHEN excluded.duration_ms>0 THEN excluded.duration_ms ELSE recent_files.duration_ms END"),
        { QVariant(fi.absoluteFilePath()), QVariant(durationMs) });
    emit recentFilesChanged();
}

void FileManagerService::updateDurationMs(const QString& path, qint64 durationMs)
{
    if (!m_db || path.isEmpty() || durationMs <= 0) return;
    m_db->exec(QStringLiteral(
        "UPDATE recent_files SET duration_ms=? WHERE path=?"),
        { QVariant(durationMs), QVariant(path) });
    emit recentFilesChanged();
}

void FileManagerService::removeFromRecent(const QString& path)
{
    if (!m_db || path.isEmpty()) return;
    m_db->exec(QStringLiteral("DELETE FROM recent_files WHERE path=?"),
               { QVariant(path) });
    emit recentFilesChanged();
}

void FileManagerService::clearRecent()
{
    if (!m_db) return;
    m_db->exec(QStringLiteral("DELETE FROM recent_files"), {});
    emit recentFilesChanged();
}

bool FileManagerService::saveThumbnail(const QString& videoPath, const QImage& frame)
{
    if (frame.isNull() || videoPath.isEmpty()) return false;

    // 缩到 480px 边长（保留宽高比），减少缓存体积
    QImage scaled = frame;
    const int longSide = qMax(frame.width(), frame.height());
    if (longSide > 480) {
        scaled = frame.scaled(
            frame.size() * (480.0 / longSide),
            Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    const QString out = thumbnailPathFor(videoPath);
    const bool ok = scaled.save(out, "JPG", 85);
    if (ok) emit recentFilesChanged();
    return ok;
}
