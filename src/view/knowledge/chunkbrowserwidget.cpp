#include "view/knowledge/chunkbrowserwidget.h"
#include "viewmodel/knowledgeviewmodel.h"
#include "service/themeservice.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QComboBox>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>

ChunkBrowserWidget::ChunkBrowserWidget(QWidget* parent)
    : QWidget(parent)
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(8);

    // 顶部工具栏
    auto* topBar = new QHBoxLayout();
    topBar->setSpacing(10);

    auto* colLabel = new QLabel(tr("集合："), this);
    colLabel->setStyleSheet(QStringLiteral(
        "font-size:12px; color:#8B8B8B; background:transparent;"));

    m_collectionCombo = new QComboBox(this);
    m_collectionCombo->addItem(tr("全部"),        -1);
    m_collectionCombo->addItem(tr("视觉帧"),        0);   // VisualFrames
    m_collectionCombo->addItem(tr("文本段"),        1);   // TextSegments
    m_collectionCombo->addItem(tr("实体档案"),      2);   // EntityProfiles
    m_collectionCombo->addItem(tr("QA 缓存"),       3);   // QACache
    m_collectionCombo->setFixedHeight(28);

    m_countLabel = new QLabel(tr("共 0 条"), this);
    m_countLabel->setStyleSheet(QStringLiteral(
        "font-size:11px; color:#8B8B8B; background:transparent;"));

    topBar->addWidget(colLabel);
    topBar->addWidget(m_collectionCombo);
    topBar->addStretch(1);
    topBar->addWidget(m_countLabel);
    v->addLayout(topBar);

    // 列表
    m_list = new QListWidget(this);
    m_list->setSpacing(2);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    v->addWidget(m_list, 1);

    connect(m_collectionCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ChunkBrowserWidget::onCollectionChanged);

    applyTheme();
}

void ChunkBrowserWidget::setThemeService(ThemeService* theme)
{
    m_theme = theme;
    if (m_theme) {
        connect(m_theme, &ThemeService::themeChanged,
                this, &ChunkBrowserWidget::applyTheme);
    }
    applyTheme();
}

void ChunkBrowserWidget::setViewModel(KnowledgeViewModel* vm)
{
    if (m_vm == vm) return;
    if (m_vm) disconnect(m_vm, nullptr, this, nullptr);
    m_vm = vm;
    if (m_vm) {
        connect(m_vm, &KnowledgeViewModel::chunksChanged,
                this, &ChunkBrowserWidget::onChunksChanged);
    }
}

void ChunkBrowserWidget::onChunksChanged(const QVector<VideoChunk>& chunks)
{
    m_list->clear();
    for (const auto& c : chunks) {
        buildItem(c);
    }
    m_countLabel->setText(tr("共 %1 条").arg(chunks.size()));
}

void ChunkBrowserWidget::onCollectionChanged(int index)
{
    if (!m_vm) return;
    const int col = m_collectionCombo->itemData(index).toInt();
    m_vm->filterChunks(col);
}

void ChunkBrowserWidget::buildItem(const VideoChunk& chunk)
{
    auto* item = new QListWidgetItem(m_list);

    // Build rich display text
    const QString timeRange = QStringLiteral("[%1 → %2]")
        .arg(formatMs(chunk.startMs), formatMs(chunk.endMs));
    const QString typeName = chunkTypeName(chunk.chunkType);
    const QString typeClr  = chunkTypeColor(chunk.chunkType);

    // Preview first 80 chars of text content
    QString preview = chunk.textContent.simplified();
    if (preview.length() > 80) {
        preview = preview.left(80) + QStringLiteral("…");
    }
    if (preview.isEmpty()) preview = tr("（无文本内容）");

    const bool hasVisual = !chunk.frameEmbedding.empty();
    const bool hasText   = !chunk.textEmbedding.empty();
    const QString embBadge = QString(QStringLiteral("[%1%2]"))
        .arg(hasText   ? QStringLiteral("T") : QString(),
             hasVisual ? QStringLiteral("V") : QString());

    item->setText(QStringLiteral("%1  %2  %3\n%4")
                  .arg(typeName, timeRange, embBadge, preview));
    item->setToolTip(chunk.textContent);

    // Color-code the item by type via foreground (approximate)
    item->setForeground(QColor(typeClr));

    m_list->addItem(item);
}

QString ChunkBrowserWidget::chunkTypeName(VideoChunk::ChunkType t)
{
    switch (t) {
    case VideoChunk::SceneSummary:   return tr("场景描述");
    case VideoChunk::SpeechSegment:  return tr("语音转写");
    case VideoChunk::Event:          return tr("事件");
    case VideoChunk::FrameDesc:      return tr("帧描述");
    case VideoChunk::QAcache:        return tr("QA缓存");
    }
    return tr("未知");
}

QString ChunkBrowserWidget::chunkTypeColor(VideoChunk::ChunkType t)
{
    switch (t) {
    case VideoChunk::SceneSummary:  return QStringLiteral("#2979FF");
    case VideoChunk::SpeechSegment: return QStringLiteral("#4CAF50");
    case VideoChunk::Event:         return QStringLiteral("#FF9800");
    case VideoChunk::FrameDesc:     return QStringLiteral("#AB47BC");
    case VideoChunk::QAcache:       return QStringLiteral("#26C6DA");
    }
    return QStringLiteral("#9E9E9E");
}

QString ChunkBrowserWidget::formatMs(int64_t ms)
{
    const int h = static_cast<int>(ms / 3600000);
    const int m = static_cast<int>((ms % 3600000) / 60000);
    const int s = static_cast<int>((ms % 60000) / 1000);
    if (h > 0)
        return QString::asprintf("%d:%02d:%02d", h, m, s);
    return QString::asprintf("%d:%02d", m, s);
}

void ChunkBrowserWidget::applyTheme()
{
    const bool dark    = m_theme ? m_theme->isDark() : true;
    const QString bg      = m_theme ? m_theme->color(QStringLiteral("background")).name()    : QStringLiteral("#12121F");
    const QString surface = m_theme ? m_theme->color(QStringLiteral("surface")).name()       : QStringLiteral("#1E1E2E");
    const QString text    = m_theme ? m_theme->color(QStringLiteral("textPrimary")).name()   : QStringLiteral("#E0E0E0");
    const QString textSub = m_theme ? m_theme->color(QStringLiteral("textSecondary")).name() : QStringLiteral("#8B8B8B");
    const QString border  = m_theme ? m_theme->color(QStringLiteral("border")).name()        : QStringLiteral("#2D2D3D");
    const QString accent  = m_theme ? m_theme->color(QStringLiteral("primary")).name()       : QStringLiteral("#2979FF");

    const QColor accentClr = m_theme ? m_theme->color(QStringLiteral("primary")) : QColor(0x29, 0x79, 0xFF);
    const QString hoverBg  = QStringLiteral("rgba(%1,%2,%3,28)")
        .arg(accentClr.red()).arg(accentClr.green()).arg(accentClr.blue());

    const QColor thumbClr  = m_theme ? m_theme->color(QStringLiteral("scrollThumb"))
                                     : QColor(dark ? "#3A3A4A" : "#C0C0C0");
    const QString thumb      = thumbClr.name();
    const QString thumbHover = (dark ? thumbClr.lighter(130) : thumbClr.darker(120)).name();

    const QString arrowIcon = dark
        ? QStringLiteral(":/icons/down_light.png")
        : QStringLiteral(":/icons/down_dark.png");

    m_list->setStyleSheet(QString(
        "QListWidget { background:%1; border:1px solid %2; border-radius:8px;"
        "              color:%3; font-size:12px; outline:none; }"
        "QListWidget::item { background:%4; border-radius:6px;"
        "                    padding:6px 8px; margin:1px 2px; }"
        "QListWidget::item:selected { background:%5; }"
        "QListWidget::item:hover    { background:%6; }"
        "QScrollBar:vertical {"
        "  width:5px; background:transparent; margin:0; border-radius:2px; }"
        "QScrollBar::handle:vertical {"
        "  background:%7; border-radius:2px; min-height:24px; }"
        "QScrollBar::handle:vertical:hover { background:%8; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {"
        "  background:transparent; }"
    ).arg(bg, border, text, surface,
          accent + QStringLiteral("44"),
          hoverBg,
          thumb, thumbHover));

    m_collectionCombo->setStyleSheet(QString(
        "QComboBox { background:%1; color:%2; border:1px solid %3;"
        "            border-radius:6px; padding:2px 8px 2px 10px; font-size:12px; }"
        "QComboBox:focus { border-color:%5; }"
        "QComboBox::drop-down { border:none; width:24px; }"
        "QComboBox::down-arrow { image:url(%4); width:12px; height:12px; }"
        "QComboBox QAbstractItemView { background:%1; color:%2; border:1px solid %3;"
        "                             border-radius:6px; outline:none;"
        "                             selection-background-color:%5; }"
    ).arg(surface, text, border, arrowIcon, accent));

    if (m_countLabel)
        m_countLabel->setStyleSheet(
            QString("font-size:11px;color:%1;background:transparent;").arg(textSub));
}
