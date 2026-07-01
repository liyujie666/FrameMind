#include "view/filelist/filelistview.h"

#include "view/filelist/videocarddelegate.h"
#include "viewmodel/filelistviewmodel.h"
#include "service/themeservice.h"

#include <QListView>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QSortFilterProxyModel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMenu>
#include <QFileDialog>
#include <QStandardPaths>
#include <QDesktopServices>
#include <QUrl>
#include <QClipboard>
#include <QApplication>
#include <QFileInfo>
#include <QIcon>

FileListView::FileListView(QWidget* parent)
    : QWidget(parent)
{
    setAutoFillBackground(true);
    setAttribute(Qt::WA_StyledBackground, true);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 20, 24, 20);
    root->setSpacing(16);

    // ---- 顶部工具栏 ----
    auto* header = new QHBoxLayout();
    header->setSpacing(12);

    m_titleLabel = new QLabel(tr("视频文件"), this);
    m_titleLabel->setStyleSheet(QStringLiteral(
        "font-size:18px; font-weight:600; background:transparent;"));
    header->addWidget(m_titleLabel);

    header->addStretch(1);

    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(tr("搜索文件…"));
    m_searchEdit->setFixedWidth(220);
    connect(m_searchEdit, &QLineEdit::textChanged,
            this, &FileListView::onSearchTextChanged);
    header->addWidget(m_searchEdit);

    m_addDirBtn = new QPushButton(tr("添加目录"), this);
    m_addDirBtn->setCursor(Qt::PointingHandCursor);
    connect(m_addDirBtn, &QPushButton::clicked,
            this, &FileListView::onAddDirectory);
    header->addWidget(m_addDirBtn);

    m_openFileBtn = new QPushButton(tr("打开文件"), this);
    m_openFileBtn->setCursor(Qt::PointingHandCursor);
    connect(m_openFileBtn, &QPushButton::clicked,
            this, &FileListView::onOpenFileClicked);
    header->addWidget(m_openFileBtn);

    root->addLayout(header);

    // ---- 中部：网格列表 + 空态浮层 ----
    m_listView = new QListView(this);
    m_listView->setItemDelegate(new VideoCardDelegate(m_listView));
    m_listView->setViewMode(QListView::IconMode);
    m_listView->setFlow(QListView::LeftToRight);
    m_listView->setWrapping(true);
    m_listView->setResizeMode(QListView::Adjust);
    m_listView->setMovement(QListView::Static);
    m_listView->setSelectionMode(QAbstractItemView::SingleSelection);
    m_listView->setSpacing(4);
    m_listView->setUniformItemSizes(true);
    m_listView->setMouseTracking(true);
    m_listView->setContextMenuPolicy(Qt::CustomContextMenu);
    m_listView->setFrameShape(QFrame::NoFrame);

    connect(m_listView, &QListView::doubleClicked,
            this, &FileListView::onDoubleClicked);
    connect(m_listView, &QListView::customContextMenuRequested,
            this, &FileListView::onContextMenu);
    connect(m_listView, &QAbstractItemView::activated,
            this, &FileListView::onDoubleClicked);

    root->addWidget(m_listView, 1);

    // 空态视图
    m_emptyView = new QWidget(this);
    auto* emptyLayout = new QVBoxLayout(m_emptyView);
    emptyLayout->setAlignment(Qt::AlignCenter);
    emptyLayout->setSpacing(16);
    auto* emptyIcon = new QLabel(m_emptyView);
    emptyIcon->setPixmap(QIcon(QStringLiteral(":/icons/files.svg")).pixmap(64, 64));
    emptyIcon->setAlignment(Qt::AlignCenter);
    emptyIcon->setStyleSheet(QStringLiteral("background:transparent;"));
    m_emptyTitle = new QLabel(tr("暂无视频文件"), m_emptyView);
    m_emptyTitle->setAlignment(Qt::AlignCenter);
    m_emptyTitle->setStyleSheet(QStringLiteral(
        "font-size:15px; font-weight:600; background:transparent;"));
    m_emptyHint = new QLabel(
        tr("点击右上角「打开文件」或「添加目录」将视频加入列表"), m_emptyView);
    m_emptyHint->setAlignment(Qt::AlignCenter);
    m_emptyHint->setStyleSheet(QStringLiteral(
        "color:#8B8B8B; font-size:13px; background:transparent;"));
    emptyLayout->addWidget(emptyIcon);
    emptyLayout->addWidget(m_emptyTitle);
    emptyLayout->addWidget(m_emptyHint);
    root->addWidget(m_emptyView, 1);
    m_emptyView->hide();

    applyThemeColors();
}

void FileListView::setThemeService(ThemeService* theme)
{
    if (m_theme == theme) return;
    if (m_theme) disconnect(m_theme, nullptr, this, nullptr);
    m_theme = theme;
    if (m_theme) {
        connect(m_theme, &ThemeService::themeChanged,
                this, [this]() { applyThemeColors(); });
        applyThemeColors();
    }
}

void FileListView::applyThemeColors()
{
    // 页面底色 = background；输入/按钮 = surface / primary
    QColor bg, text, textSecondary, border, primary, primaryHover, primaryPressed,
           inputBg, surfaceVariant, scrollThumb;
    if (m_theme) {
        bg              = m_theme->color(QStringLiteral("background"));
        text            = m_theme->color(QStringLiteral("textPrimary"));
        textSecondary   = m_theme->color(QStringLiteral("textSecondary"));
        border          = m_theme->color(QStringLiteral("border"));
        primary         = m_theme->color(QStringLiteral("primary"));
        primaryHover    = m_theme->color(QStringLiteral("primaryHover"));
        primaryPressed  = m_theme->color(QStringLiteral("primaryPressed"));
        inputBg         = m_theme->color(QStringLiteral("inputBg"));
        surfaceVariant  = m_theme->color(QStringLiteral("surfaceVariant"));
        scrollThumb     = m_theme->color(QStringLiteral("scrollThumb"));
    } else {
        bg = QColor("#0D1117"); text = QColor("#E0E0E0");
        textSecondary = QColor("#8B8B8B"); border = QColor("#2D2D3D");
        primary = QColor("#2979FF"); primaryHover = QColor("#448AFF");
        primaryPressed = QColor("#1565C0"); inputBg = QColor("#1A1A2A");
        surfaceVariant = QColor("#252538"); scrollThumb = QColor("#3A3A4A");
    }

    setStyleSheet(QString("FileListView { background:%1; }").arg(bg.name()));

    if (m_titleLabel) {
        m_titleLabel->setStyleSheet(QString(
            "color:%1; font-size:18px; font-weight:600; background:transparent;")
            .arg(text.name()));
    }
    if (m_emptyTitle) {
        m_emptyTitle->setStyleSheet(QString(
            "color:%1; font-size:15px; font-weight:600; background:transparent;")
            .arg(text.name()));
    }
    if (m_emptyHint) {
        m_emptyHint->setStyleSheet(QString(
            "color:%1; font-size:13px; background:transparent;")
            .arg(textSecondary.name()));
    }
    if (m_searchEdit) {
        m_searchEdit->setStyleSheet(QString(
            "QLineEdit { background:%1; color:%2; border:1px solid %3; "
            "border-radius:8px; padding:6px 12px; font-size:13px; }"
            "QLineEdit:focus { border-color:%4; }")
            .arg(inputBg.name(), text.name(), border.name(), primary.name()));
    }
    if (m_addDirBtn) {
        m_addDirBtn->setStyleSheet(QString(
            "QPushButton { background:transparent; color:%1; border:1px solid %2; "
            "border-radius:8px; padding:6px 14px; font-size:13px; }"
            "QPushButton:hover { border-color:%3; color:%3; }")
            .arg(text.name(), border.name(), primary.name()));
    }
    if (m_openFileBtn) {
        m_openFileBtn->setStyleSheet(QString(
            "QPushButton { background:%1; color:#FFFFFF; border:none; "
            "border-radius:8px; padding:7px 16px; font-size:13px; font-weight:500; }"
            "QPushButton:hover { background:%2; }"
            "QPushButton:pressed { background:%3; }")
            .arg(primary.name(), primaryHover.name(), primaryPressed.name()));
    }
    if (m_listView) {
        m_listView->setStyleSheet(QString(
            "QListView { background:%1; border:none; outline:none; }"
            "QListView::item { background:transparent; }"
            "QListView::item:selected { background:transparent; }"
            "QScrollBar:vertical { background:transparent; width:10px; margin:2px; }"
            "QScrollBar::handle:vertical { background:%2; border-radius:4px; min-height:30px; }"
            "QScrollBar::handle:vertical:hover { background:%3; }"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; }")
            .arg(bg.name(), scrollThumb.name(), scrollThumb.lighter(120).name()));
    }
}

void FileListView::setViewModel(FileListViewModel* vm)
{
    m_vm = vm;
    if (!m_vm) return;

    m_proxy = new QSortFilterProxyModel(this);
    m_proxy->setSourceModel(m_vm);
    m_proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_proxy->setFilterRole(FileListViewModel::DisplayNameRole);

    m_listView->setModel(m_proxy);

    connect(m_vm, &QAbstractItemModel::rowsInserted,
            this, &FileListView::refreshEmptyState);
    connect(m_vm, &QAbstractItemModel::rowsRemoved,
            this, &FileListView::refreshEmptyState);
    connect(m_vm, &QAbstractItemModel::modelReset,
            this, &FileListView::refreshEmptyState);

    refreshEmptyState();
}

void FileListView::refreshEmptyState()
{
    if (!m_vm || !m_proxy) return;
    const bool empty = m_proxy->rowCount() == 0;
    m_listView->setVisible(!empty);
    m_emptyView->setVisible(empty);
}

void FileListView::onDoubleClicked(const QModelIndex& index)
{
    if (!index.isValid()) return;
    const QString path = index.data(FileListViewModel::PathRole).toString();
    if (path.isEmpty()) return;
    emit openRequested(path);
}

void FileListView::onSearchTextChanged(const QString& text)
{
    if (m_proxy) m_proxy->setFilterFixedString(text);
    refreshEmptyState();
}

void FileListView::onAddDirectory()
{
    const QString start =
        QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("选择视频所在目录"), start);
    if (dir.isEmpty() || !m_vm) return;
    m_vm->addFromDirectory(dir);
    refreshEmptyState();
}

void FileListView::onOpenFileClicked()
{
    const QString start =
        QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    const QString path = QFileDialog::getOpenFileName(
        this, tr("打开视频"), start,
        tr("视频文件 (*.mp4 *.mkv *.avi *.mov *.flv *.ts *.webm *.wmv *.m4v);;所有文件 (*.*)"));
    if (path.isEmpty()) return;
    emit openRequested(path);
}

void FileListView::onContextMenu(const QPoint& pos)
{
    if (!m_vm || !m_proxy) return;
    const QModelIndex proxyIdx = m_listView->indexAt(pos);
    if (!proxyIdx.isValid()) return;
    const QModelIndex srcIdx = m_proxy->mapToSource(proxyIdx);
    const QString path = srcIdx.data(FileListViewModel::PathRole).toString();

    QMenu menu(this);

    connect(menu.addAction(tr("打开")), &QAction::triggered,
            this, [this, path]() { emit openRequested(path); });
    connect(menu.addAction(tr("在文件夹中显示")), &QAction::triggered,
            this, [path]() {
                const QFileInfo fi(path);
                QDesktopServices::openUrl(QUrl::fromLocalFile(fi.absolutePath()));
            });
    connect(menu.addAction(tr("复制路径")), &QAction::triggered,
            this, [path]() { QApplication::clipboard()->setText(path); });
    menu.addSeparator();
    connect(menu.addAction(tr("从列表移除")), &QAction::triggered,
            this, [this, srcIdx]() {
                if (m_vm) m_vm->removeAt(srcIdx.row());
            });

    menu.exec(m_listView->viewport()->mapToGlobal(pos));
}
