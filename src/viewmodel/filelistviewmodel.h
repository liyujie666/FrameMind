#ifndef FRAMEMIND_FILELISTVIEWMODEL_H
#define FRAMEMIND_FILELISTVIEWMODEL_H

#include <QAbstractListModel>
#include <QList>

#include "service/filemanagerservice.h"

class FileManagerService;
class EventBus;
class PlayerService;

/**
 * 文件列表 ViewModel（QAbstractListModel），驱动 FileListView 的网格显示。
 *
 * 数据源来自 FileManagerService::recentFiles()；打开视频后 PlayerService 的
 * frameDecoded 首帧会自动回填缩略图。
 */
class FileListViewModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles {
        PathRole = Qt::UserRole + 1,
        DisplayNameRole,
        SizeRole,
        LastOpenedRole,
        ThumbnailPathRole,
    };

    explicit FileListViewModel(FileManagerService* fileService,
                               EventBus* eventBus,
                               PlayerService* playerService,
                               QObject* parent = nullptr);

    // QAbstractListModel
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    /// 返回指定 row 的视频路径
    QString pathAt(int row) const;

public slots:
    /// 手动扫描一个目录并合并进列表（不入库，仅用于展示）
    void addFromDirectory(const QString& dir);

    /// 刷新（从服务重新取最近文件）
    void refresh();

    /// 从列表移除并从最近文件表中删除
    void removeAt(int row);

signals:
    /// 请求打开视频（View 双击 / Enter 时发出）
    void openRequested(const QString& path);

private slots:
    void onRecentFilesChanged();
    void onPlayerFrameDecoded(const QImage& frame);

private:
    FileManagerService* m_fileService = nullptr;
    EventBus*           m_eventBus = nullptr;
    PlayerService*      m_playerService = nullptr;

    QList<VideoFileItem> m_items;

    // 首帧回填缩略图：记录当前"待截图"的视频路径
    QString m_pendingThumbForPath;
};

#endif // FRAMEMIND_FILELISTVIEWMODEL_H
