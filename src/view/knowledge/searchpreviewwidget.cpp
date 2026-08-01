#include "view/knowledge/searchpreviewwidget.h"
#include "viewmodel/knowledgeviewmodel.h"
#include "service/themeservice.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QToolButton>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QKeyEvent>

SearchPreviewWidget::SearchPreviewWidget(QWidget* parent)
    : QWidget(parent)
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(10);

    // 说明文字
    auto* hint = new QLabel(
        tr("在当前选中视频的知识库中执行语义检索，验证 RAG 召回效果"), this);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral(
        "font-size:11px; color:#8B8B8B; background:transparent;"));
    v->addWidget(hint);

    // 搜索框 + 按钮
    auto* row = new QHBoxLayout();
    row->setSpacing(8);

    m_queryEdit = new QLineEdit(this);
    m_queryEdit->setPlaceholderText(tr("输入关键词或自然语言问题…"));
    m_queryEdit->setFixedHeight(34);
    connect(m_queryEdit, &QLineEdit::returnPressed,
            this, &SearchPreviewWidget::onSearchClicked);

    m_searchButton = new QToolButton(this);
    m_searchButton->setText(tr("检索"));
    m_searchButton->setFixedSize(60, 34);
    m_searchButton->setCursor(Qt::PointingHandCursor);
    connect(m_searchButton, &QToolButton::clicked,
            this, &SearchPreviewWidget::onSearchClicked);

    row->addWidget(m_queryEdit, 1);
    row->addWidget(m_searchButton);
    v->addLayout(row);

    // 状态标签
    m_statusLabel = new QLabel(this);
    m_statusLabel->setStyleSheet(QStringLiteral(
        "font-size:11px; color:#8B8B8B; background:transparent;"));
    m_statusLabel->hide();
    v->addWidget(m_statusLabel);

    // 结果列表
    m_resultList = new QListWidget(this);
    m_resultList->setSpacing(2);
    m_resultList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_resultList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_resultList->setSelectionMode(QAbstractItemView::SingleSelection);
    v->addWidget(m_resultList, 1);

    applyTheme();
}

void SearchPreviewWidget::setThemeService(ThemeService* theme)
{
    m_theme = theme;
    if (m_theme) {
        connect(m_theme, &ThemeService::themeChanged,
                this, &SearchPreviewWidget::applyTheme);
    }
    applyTheme();
}

void SearchPreviewWidget::setViewModel(KnowledgeViewModel* vm)
{
    if (m_vm == vm) return;
    if (m_vm) disconnect(m_vm, nullptr, this, nullptr);
    m_vm = vm;
    if (m_vm) {
        connect(m_vm, &KnowledgeViewModel::searchResultsReady,
                this, &SearchPreviewWidget::onSearchResultsReady);
        connect(m_vm, &KnowledgeViewModel::searchingChanged,
                this, &SearchPreviewWidget::onSearchingChanged);
    }
}

void SearchPreviewWidget::onSearchClicked()
{
    if (!m_vm) return;
    const QString q = m_queryEdit->text().trimmed();
    if (q.isEmpty()) return;
    m_resultList->clear();
    m_vm->testSearch(q);
}

void SearchPreviewWidget::onSearchResultsReady(const QVector<RetrievalResult>& results)
{
    m_resultList->clear();
    if (results.isEmpty()) {
        m_statusLabel->setText(tr("未找到相关内容"));
        m_statusLabel->show();
        return;
    }
    m_statusLabel->setText(tr("召回 %1 条结果").arg(results.size()));
    m_statusLabel->show();

    for (const auto& r : results) {
        buildResultItem(r);
    }
}

void SearchPreviewWidget::onSearchingChanged(bool searching)
{
    m_searchButton->setEnabled(!searching);
    m_queryEdit->setEnabled(!searching);
    if (searching) {
        m_statusLabel->setText(tr("检索中…"));
        m_statusLabel->show();
    }
}

void SearchPreviewWidget::buildResultItem(const RetrievalResult& r)
{
    auto* item = new QListWidgetItem(m_resultList);

    const QString timeRange = QStringLiteral("[%1 → %2]")
        .arg(formatMs(r.chunk.startMs), formatMs(r.chunk.endMs));
    const QString pathLabel = hitPathLabel(r.hitPath);
    const QString score     = QStringLiteral("%.3f").arg(r.score, 0, 'f', 3);

    QString preview = r.chunk.textContent.simplified();
    if (preview.length() > 100) preview = preview.left(100) + QStringLiteral("…");
    if (preview.isEmpty())      preview = tr("（无文本内容）");

    item->setText(QStringLiteral("★ %1  %2  %3  %4\n%5")
                  .arg(score, pathLabel, timeRange,
                       QString(), // padding
                       preview));
    item->setToolTip(r.chunk.textContent);
    item->setForeground(QColor(hitPathColor(r.hitPath)));

    m_resultList->addItem(item);
}

QString SearchPreviewWidget::hitPathLabel(const QString& path)
{
    if (path == QLatin1String("visual"))   return tr("[视觉]");
    if (path == QLatin1String("text"))     return tr("[文本]");
    if (path == QLatin1String("entity"))   return tr("[实体]");
    if (path == QLatin1String("qa_cache")) return tr("[QA]");
    return QStringLiteral("[—]");
}

QString SearchPreviewWidget::hitPathColor(const QString& path)
{
    if (path == QLatin1String("visual"))   return QStringLiteral("#AB47BC");
    if (path == QLatin1String("text"))     return QStringLiteral("#4CAF50");
    if (path == QLatin1String("entity"))   return QStringLiteral("#FF9800");
    if (path == QLatin1String("qa_cache")) return QStringLiteral("#26C6DA");
    return QStringLiteral("#9E9E9E");
}

QString SearchPreviewWidget::formatMs(int64_t ms)
{
    const int h = static_cast<int>(ms / 3600000);
    const int m = static_cast<int>((ms % 3600000) / 60000);
    const int s = static_cast<int>((ms % 60000) / 1000);
    if (h > 0)
        return QString::asprintf("%d:%02d:%02d", h, m, s);
    return QString::asprintf("%d:%02d", m, s);
}

void SearchPreviewWidget::applyTheme()
{
    const bool dark    = m_theme ? m_theme->isDark() : true;
    const QString bg      = m_theme ? m_theme->color(QStringLiteral("background")).name()    : QStringLiteral("#12121F");
    const QString surface = m_theme ? m_theme->color(QStringLiteral("surface")).name()       : QStringLiteral("#1E1E2E");
    const QString text    = m_theme ? m_theme->color(QStringLiteral("textPrimary")).name()   : QStringLiteral("#E0E0E0");
    const QString textSub = m_theme ? m_theme->color(QStringLiteral("textSecondary")).name() : QStringLiteral("#8B8B8B");
    const QString border  = m_theme ? m_theme->color(QStringLiteral("border")).name()        : QStringLiteral("#2D2D3D");
    const QString accent  = m_theme ? m_theme->color(QStringLiteral("primary")).name()       : QStringLiteral("#2979FF");
    const QString accentHover = m_theme ? m_theme->color(QStringLiteral("primaryHover")).name() : QStringLiteral("#448AFF");

    const QColor accentClr = m_theme ? m_theme->color(QStringLiteral("primary")) : QColor(0x29, 0x79, 0xFF);
    const QString hoverBg  = QStringLiteral("rgba(%1,%2,%3,28)")
        .arg(accentClr.red()).arg(accentClr.green()).arg(accentClr.blue());

    const QColor thumbClr  = m_theme ? m_theme->color(QStringLiteral("scrollThumb"))
                                     : QColor(dark ? "#3A3A4A" : "#C0C0C0");
    const QString thumb      = thumbClr.name();
    const QString thumbHover = (dark ? thumbClr.lighter(130) : thumbClr.darker(120)).name();

    Q_UNUSED(dark)

    m_queryEdit->setStyleSheet(QString(
        "QLineEdit{background:%1;color:%2;border:1px solid %3;"
        "          border-radius:8px;padding:4px 10px;font-size:13px;}"
        "QLineEdit:focus{border-color:%4;}"
    ).arg(surface, text, border, accent));

    m_searchButton->setStyleSheet(QString(
        "QToolButton{background:%1;color:#FFF;border:none;"
        "             border-radius:8px;font-size:13px;font-weight:600;}"
        "QToolButton:hover{background:%2;}"
        "QToolButton:disabled{background:%3;color:%4;}"
    ).arg(accent, accentHover, surface, textSub));

    m_resultList->setStyleSheet(QString(
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

    if (m_statusLabel)
        m_statusLabel->setStyleSheet(
            QString("font-size:11px;color:%1;background:transparent;").arg(textSub));
}
