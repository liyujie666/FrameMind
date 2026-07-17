#ifndef FRAMEMIND_FILEMANAGERSERVICE_H
#define FRAMEMIND_FILEMANAGERSERVICE_H

#include <QObject>
#include <QString>
#include <QList>
#include <QDateTime>
#include <QImage>

class DatabaseManager;

/**
 * 视频文件条目（最近文件 / 扫描目录返回项）。
 */
struct VideoFileItem {
    QString  path;             // 绝对路径
    QString  displayName;      // 显示名（不含路径）
    qint64   sizeBytes = 0;    // 文件大小（B）
    QDateTime lastOpened;      // 上次打开时间，未打开时为空
    QString  thumbnailPath;    // 磁盘缓存的缩略图 jpg 路径；不存在时为空
    qint64   durationMs = 0;   // 媒体时长（ms）；未探测 / 不支持时为 0
};

/**
 * 文件管理服务：负责最近文件的持久化、目录扫描以及视频缩略图缓存。
 *
 * 缩略图缓存约定：
 *   - 位置：<AppData>/thumbnails/<md5(path)>.jpg
 *   - 缩略图由外部（PlayerService 首帧解码 → FileListViewModel）调 saveThumbnail 存入
 */
class FileManagerService : public QObject {
    Q_OBJECT
public:
    explicit FileManagerService(DatabaseManager* db, QObject* parent = nullptr);

    /// 返回按 last_opened DESC 排序的最近文件列表
    QList<VideoFileItem> recentFiles(int limit = 60) const;

    /// 扫描目录下的视频文件（非递归、按扩展名过滤），不写库
    QList<VideoFileItem> scanDirectory(const QString& dir) const;

    /// 把某路径加入最近文件（视频打开时调用）
    void addToRecent(const QString& path);
    /// 把某路径加入最近文件，并记录时长（打开视频时由调用方提供更精确的值）
    void addToRecent(const QString& path, qint64 durationMs);

    /// 从最近文件里移除
    void removeFromRecent(const QString& path);

    /// 清空最近文件
    void clearRecent();

    /// 缩略图缓存目录（首次调用自动创建）
    QString thumbnailCacheDir() const;

    /// 该文件的缩略图缓存路径（不代表已存在）
    QString thumbnailPathFor(const QString& videoPath) const;

    /// 存缩略图（自动缩到 480 边长、jpg 85 质量）
    bool saveThumbnail(const QString& videoPath, const QImage& frame);

    /// 更新某路径记录的 durationMs（不改变 last_opened）
    void updateDurationMs(const QString& path, qint64 durationMs);

signals:
    /// 最近文件列表发生变化（添加 / 删除 / 缩略图更新）
    void recentFilesChanged();

private:
    static QStringList supportedVideoExtensions();

    DatabaseManager* m_db = nullptr;
};

#endif // FRAMEMIND_FILEMANAGERSERVICE_H
