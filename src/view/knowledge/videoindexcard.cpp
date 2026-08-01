#include "view/knowledge/videoindexcard.h"
#include "service/themeservice.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QToolButton>
#include <QPainter>
#include <QMouseEvent>
#include <QEnterEvent>
#include <QContextMenuEvent>
#include <QMenu>
#include <QAction>
#include <QDesktopServices>
#include <QFileInfo>
#include <QUrl>
#include <QApplication>
#include <QClipboard>

VideoIndexCard::VideoIndexCard(const KnowledgeViewModel::VideoIndexSummary& summary,
                               QWidget* parent)
    : QWidget(parent)
    , m_summary(summary)
{
    setFixedHeight(76);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setCursor(Qt::PointingHandCursor);
    setMouseTracking(true);

    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(12, 8, 8, 8);
    outer->setSpacing(8);

    // 左侧：视频图标占位
    auto* iconLabel = new QLabel(this);
    iconLabel->setFixedSize(36, 36);
    iconLabel->setStyleSheet(QStringLiteral(
        "background:#2979FF22; border-radius:8px;"));
    iconLabel->setAlignment(Qt::AlignCenter);
    iconLabel->setText(QStringLiteral("▶"));
    iconLabel->setStyleSheet(QStringLiteral(
        "font-size:14px; color:#2979FF; background:#2979FF18;"
        "border-radius:8px;"));
    outer->addWidget(iconLabel, 0, Qt::AlignVCenter);

    // 中间：文字信息
    auto* infoCol = new QVBoxLayout();
    infoCol->setSpacing(2);
    infoCol->setContentsMargins(0, 0, 0, 0);

    // 行1：文件名 + Level 徽章
    auto* row1 = new QHBoxLayout();
    row1->setSpacing(6);
    row1->setContentsMargins(0, 0, 0, 0);

    m_nameLabel = new QLabel(this);
    m_nameLabel->setAttribute(Qt::WA_StyledBackground, false);
    m_nameLabel->setStyleSheet(QStringLiteral(
        "font-size:13px; font-weight:600; background:transparent;"));
    m_nameLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    m_levelBadge = new QLabel(this);
    m_levelBadge->setAttribute(Qt::WA_StyledBackground, false);
    m_levelBadge->setFixedHeight(18);
    m_levelBadge->setAlignment(Qt::AlignCenter);

    row1->addWidget(m_nameLabel, 1);
    row1->addWidget(m_levelBadge, 0);
    infoCol->addLayout(row1);

    // 行2：chunk 统计
    m_statsLabel = new QLabel(this);
    m_statsLabel->setAttribute(Qt::WA_StyledBackground, false);
    m_statsLabel->setStyleSheet(QStringLiteral(
        "font-size:11px; color:#8B8B8B; background:transparent;"));
    infoCol->addWidget(m_statsLabel);

    // 行3：最后索引时间
    m_dateLabel = new QLabel(this);
    m_dateLabel->setAttribute(Qt::WA_StyledBackground, false);
    m_dateLabel->setStyleSheet(QStringLiteral(
        "font-size:10px; color:#666; background:transparent;"));
    infoCol->addWidget(m_dateLabel);

    outer->addLayout(infoCol, 1);

    // 右侧：删除按钮
    m_deleteButton = new QToolButton(this);
    m_deleteButton->setFixedSize(28, 28);
    m_deleteButton->setToolTip(tr("删除此视频索引"));
    m_deleteButton->setCursor(Qt::PointingHandCursor);
    m_deleteButton->setText(QStringLiteral("✕"));
    m_deleteButton->setStyleSheet(QStringLiteral(
        "QToolButton { border:none; border-radius:6px; background:transparent;"
        "              color:#888; font-size:12px; }"
        "QToolButton:hover { background:#FF4C4C22; color:#FF4C4C; }"));
    connect(m_deleteButton, &QToolButton::clicked, this,
            [this]() { emit removeRequested(m_summary.videoId); });
    outer->addWidget(m_deleteButton, 0, Qt::AlignVCenter);

    updateSummary(summary);
}

void VideoIndexCard::setThemeService(ThemeService* theme)
{
    m_theme = theme;
    if (m_theme) {
        connect(m_theme, &ThemeService::themeChanged,
                this, [this]() { applyTheme(); update(); });
    }
    applyTheme();
}

void VideoIndexCard::setSelected(bool selected)
{
    if (m_selected == selected) return;
    m_selected = selected;
    update();
}

void VideoIndexCard::updateSummary(const KnowledgeViewModel::VideoIndexSummary& summary)
{
    m_summary = summary;

    // File name (truncate in UI via elide)
    m_nameLabel->setText(summary.fileName.isEmpty()
                         ? summary.videoId.left(24)
                         : summary.fileName);
    m_nameLabel->setToolTip(summary.filePath.isEmpty()
                            ? summary.videoId
                            : summary.filePath);

    // Level badge
    const QString lvlText = levelText(summary.level);
    const QColor  lvlClr  = levelColor(summary.level);
    m_levelBadge->setText(lvlText);
    m_levelBadge->setFixedWidth(lvlText.length() * 8 + 14);
    m_levelBadge->setStyleSheet(QString(
        "font-size:10px; font-weight:700; border-radius:4px; padding:0 4px;"
        "color:#FFF; background:%1;").arg(lvlClr.name()));

    // Chunk stats
    m_statsLabel->setText(tr("视觉帧 %1  ·  文本段 %2  ·  QA %3  ·  共 %4 条")
        .arg(summary.visualCount)
        .arg(summary.textCount)
        .arg(summary.qaCacheCount)
        .arg(summary.totalChunks));

    // Date
    if (summary.lastIndexed.isValid()) {
        m_dateLabel->setText(tr("更新于 %1")
            .arg(summary.lastIndexed.toString(QStringLiteral("MM-dd hh:mm"))));
    } else {
        m_dateLabel->setText(QString());
    }
}

void VideoIndexCard::applyTheme()
{
    if (!m_theme) return;
    const QString text    = m_theme->color(QStringLiteral("textPrimary")).name();
    const QString textSub = m_theme->color(QStringLiteral("textSecondary")).name();

    if (m_nameLabel)
        m_nameLabel->setStyleSheet(QString(
            "font-size:13px;font-weight:600;background:transparent;color:%1;").arg(text));
    if (m_statsLabel)
        m_statsLabel->setStyleSheet(QString(
            "font-size:11px;background:transparent;color:%1;").arg(textSub));
    if (m_dateLabel)
        m_dateLabel->setStyleSheet(QString(
            "font-size:10px;background:transparent;color:%1;").arg(textSub));

    update();
}

QString VideoIndexCard::levelText(int level) const
{
    switch (level) {
    case 0: return QStringLiteral("L0");
    case 1: return QStringLiteral("L1");
    case 2: return QStringLiteral("L2");
    default: return QStringLiteral("—");
    }
}

QColor VideoIndexCard::levelColor(int level) const
{
    switch (level) {
    case 0: return QColor(QStringLiteral("#FF9800"));
    case 1: return QColor(QStringLiteral("#4CAF50"));
    case 2: return QColor(QStringLiteral("#2979FF"));
    default: return QColor(QStringLiteral("#9E9E9E"));
    }
}

void VideoIndexCard::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        emit selected(m_summary.videoId);
    }
    QWidget::mousePressEvent(event);
}

void VideoIndexCard::contextMenuEvent(QContextMenuEvent* event)
{
    // Select this card first
    emit selected(m_summary.videoId);

    const bool dark   = m_theme ? m_theme->isDark() : true;
    const QString bg      = m_theme ? m_theme->color(QStringLiteral("surface")).name()     : QStringLiteral("#1E1E2E");
    const QString text    = m_theme ? m_theme->color(QStringLiteral("textPrimary")).name() : QStringLiteral("#E0E0E0");
    const QString border  = m_theme ? m_theme->color(QStringLiteral("border")).name()      : QStringLiteral("#2D2D3D");
    const QString accent  = m_theme ? m_theme->color(QStringLiteral("primary")).name()     : QStringLiteral("#2979FF");
    const QColor accentClr = m_theme ? m_theme->color(QStringLiteral("primary")) : QColor(0x29,0x79,0xFF);
    const QString hoverBg = QStringLiteral("rgba(%1,%2,%3,40)")
        .arg(accentClr.red()).arg(accentClr.green()).arg(accentClr.blue());

    Q_UNUSED(dark)

    QMenu menu(this);
    menu.setStyleSheet(QString(
        "QMenu {"
        "  background:%1; color:%2; border:1px solid %3;"
        "  border-radius:8px; padding:4px 0; font-size:13px; }"
        "QMenu::item { padding:6px 20px 6px 14px; border-radius:4px; margin:1px 4px; }"
        "QMenu::item:selected { background:%4; }"
        "QMenu::separator { height:1px; background:%3; margin:4px 8px; }"
    ).arg(bg, text, border, hoverBg));

    // 在文件管理器中显示（仅文件路径存在时启用）
    const bool hasPath = !m_summary.filePath.isEmpty()
                         && QFileInfo::exists(m_summary.filePath);

    QAction* showInExplorer = menu.addAction(
        QIcon(QStringLiteral(":/icons/file_") +
              (m_theme && !m_theme->isDark() ? QStringLiteral("dark") : QStringLiteral("light")) +
              QStringLiteral(".png")),
        tr("在文件管理器中显示"));
    showInExplorer->setEnabled(hasPath);

    // 复制文件路径
    QAction* copyPath = menu.addAction(tr("复制文件路径"));
    copyPath->setEnabled(!m_summary.filePath.isEmpty());

    menu.addSeparator();

    // 删除索引
    QAction* removeAction = menu.addAction(tr("删除知识库索引"));
    removeAction->setIcon(QIcon());   // 无图标，用文字即可

    // 应用删除项的红色高亮
    removeAction->setProperty("danger", true);
    QString dangerStyle = menu.styleSheet() +
        QStringLiteral("QMenu::item[danger=\"true\"]:selected { background:rgba(255,76,76,40); }");
    menu.setStyleSheet(dangerStyle);

    QAction* chosen = menu.exec(event->globalPos());

    if (chosen == showInExplorer) {
        // QDesktopServices::openUrl 传 file:/// 父目录，让资源管理器定位到该文件
        const QUrl url = QUrl::fromLocalFile(QFileInfo(m_summary.filePath).absolutePath());
        QDesktopServices::openUrl(url);
        emit openInExplorerRequested(m_summary.filePath);
    } else if (chosen == copyPath) {
        QApplication::clipboard()->setText(m_summary.filePath);
    } else if (chosen == removeAction) {
        emit removeRequested(m_summary.videoId);
    }
}

void VideoIndexCard::paintEvent(QPaintEvent* event)
{
    QWidget::paintEvent(event);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const bool dark = m_theme ? m_theme->isDark() : true;

    QColor bg;
    if (m_selected) {
        const QColor accent = m_theme
            ? m_theme->color(QStringLiteral("primary"))
            : QColor(QStringLiteral("#2979FF"));
        bg = QColor(accent.red(), accent.green(), accent.blue(), dark ? 34 : 22);
    } else if (m_hovered) {
        // 亮色主题：纯黑 6% 透明；暗色主题：纯白 6% 透明
        bg = dark ? QColor(255, 255, 255, 15) : QColor(0, 0, 0, 15);
    } else {
        bg = Qt::transparent;
    }

    if (bg.alpha() > 0) {
        p.setPen(Qt::NoPen);
        p.setBrush(bg);
        p.drawRoundedRect(rect().adjusted(2, 2, -2, -2), 8, 8);
    }

    // Left accent bar when selected
    if (m_selected) {
        const QColor accent = m_theme
            ? m_theme->color(QStringLiteral("primary"))
            : QColor(QStringLiteral("#2979FF"));
        p.setBrush(accent);
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(QRectF(2, rect().height() / 2 - 12, 3, 24), 1.5f, 1.5f);
    }
}

void VideoIndexCard::enterEvent(QEnterEvent* event)
{
    m_hovered = true;
    update();
    QWidget::enterEvent(event);
}

void VideoIndexCard::leaveEvent(QEvent* event)
{
    m_hovered = false;
    update();
    QWidget::leaveEvent(event);
}
