#ifndef FRAMEMIND_FILELISTVIEW_H
#define FRAMEMIND_FILELISTVIEW_H

#include <QWidget>

class FileListViewModel;
class ThemeService;
class QListView;
class QLabel;
class QPushButton;
class QLineEdit;
class QSortFilterProxyModel;

/**
 * 文件列表页：网格式排列视频缩略图卡片。
 * - 顶部：标题 / 搜索框 / 添加目录 / 打开文件 按钮
 * - 中部：QListView(IconMode + Wrap) + VideoCardDelegate 呈现缩略图网格
 * - 空态：无最近文件时展示引导视图
 * - 交互：双击 → openRequested(path)；右键 → 移除/打开/复制路径
 */
class FileListView : public QWidget {
    Q_OBJECT
public:
    explicit FileListView(QWidget* parent = nullptr);

    void setViewModel(FileListViewModel* vm);
    void setThemeService(ThemeService* theme);

signals:
    /// 用户请求打开某视频（双击卡片 / Enter）
    void openRequested(const QString& path);

private slots:
    void onDoubleClicked(const QModelIndex& index);
    void onContextMenu(const QPoint& pos);
    void onAddDirectory();
    void onOpenFileClicked();
    void onSearchTextChanged(const QString& text);
    void refreshEmptyState();

private:
    void applyThemeColors();

    FileListViewModel*      m_vm = nullptr;
    QSortFilterProxyModel*  m_proxy = nullptr;
    ThemeService*           m_theme = nullptr;

    QLabel*      m_titleLabel = nullptr;
    QLabel*      m_emptyTitle = nullptr;
    QLabel*      m_emptyHint = nullptr;
    QLineEdit*   m_searchEdit = nullptr;
    QPushButton* m_addDirBtn = nullptr;
    QPushButton* m_openFileBtn = nullptr;
    QListView*   m_listView = nullptr;
    QWidget*     m_emptyView = nullptr;
};

#endif // FRAMEMIND_FILELISTVIEW_H
