#include "view/player/videosummarycard.h"

#include "service/markdownrenderer.h"
#include "service/themeservice.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QClipboard>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QShowEvent>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtMath>

namespace {

// 文档首次显示、重新换行和主题排版变化都会更新高度。
// 隐藏页面的文档宽度尚未确定，留到 showEvent 后测量。
class SummaryTextBrowser final : public QTextBrowser {
public:
    explicit SummaryTextBrowser(QWidget* parent)
        : QTextBrowser(parent)
    {
        setReadOnly(true);
        setOpenLinks(false);
        setFrameShape(QFrame::NoFrame);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        document()->setDocumentMargin(0);

        connect(document()->documentLayout(),
                &QAbstractTextDocumentLayout::documentSizeChanged,
                this, [this](const QSizeF&) { scheduleHeightUpdate(); });
        connect(document(), &QTextDocument::contentsChanged,
                this, [this]() { scheduleHeightUpdate(); });
    }

protected:
    void showEvent(QShowEvent* event) override
    {
        QTextBrowser::showEvent(event);
        scheduleHeightUpdate();
    }

    void resizeEvent(QResizeEvent* event) override
    {
        QTextBrowser::resizeEvent(event);
        if (event->size().width() != event->oldSize().width())
            scheduleHeightUpdate();
    }

    void wheelEvent(QWheelEvent* event) override
    {
        // 正文不独立滚动，把滚轮交给总结 Tab 的外层 QScrollArea。
        event->ignore();
    }

private:
    void scheduleHeightUpdate()
    {
        if (m_heightUpdatePending) return;
        m_heightUpdatePending = true;
        QTimer::singleShot(0, this, [this]() {
            m_heightUpdatePending = false;
            if (!isVisible() || viewport()->width() <= 0) return;

            const int textWidth = viewport()->width();
            if (!qFuzzyCompare(document()->textWidth(), qreal(textWidth)))
                document()->setTextWidth(textWidth);

            const int contentHeight = qCeil(document()->size().height());
            const int chromeHeight = height() - viewport()->height();
            const int targetHeight = qMax(24, contentHeight + chromeHeight + 4);
            if (minimumHeight() != targetHeight || maximumHeight() != targetHeight)
                setFixedHeight(targetHeight);
        });
    }

    bool m_heightUpdatePending = false;
};

} // namespace

VideoSummaryCard::VideoSummaryCard(QWidget* parent)
    : QWidget(parent)
    , m_renderer(std::make_unique<MarkdownRenderer>())
{
    setObjectName(QStringLiteral("videoSummaryCard"));
    setAttribute(Qt::WA_StyledBackground, true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(14);

    auto* header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(10);
    m_title = new QLabel(tr("视频概览"), this);
    m_title->setObjectName(QStringLiteral("summaryTitle"));
    header->addWidget(m_title);

    m_typeLabel = new QLabel(this);
    m_typeLabel->setObjectName(QStringLiteral("summaryType"));
    m_typeLabel->setTextFormat(Qt::PlainText);
    m_typeLabel->hide();
    header->addWidget(m_typeLabel);
    header->addStretch(1);

    m_copyButton = new QToolButton(this);
    m_copyButton->setObjectName(QStringLiteral("summaryCopy"));
    m_copyButton->setText(tr("复制"));
    m_copyButton->setToolTip(tr("复制完整视频概览"));
    m_copyButton->setAccessibleName(tr("复制视频概览"));
    m_copyButton->setCursor(Qt::PointingHandCursor);
    m_copyButton->setEnabled(false);
    connect(m_copyButton, &QToolButton::clicked, this, [this]() {
        QApplication::clipboard()->setText(m_summary);
    });
    header->addWidget(m_copyButton);
    layout->addLayout(header);

    m_browser = new SummaryTextBrowser(this);
    m_browser->setObjectName(QStringLiteral("summaryBody"));
    m_browser->setAccessibleName(tr("视频概览正文"));
    layout->addWidget(m_browser);

    applyTheme();
}

VideoSummaryCard::~VideoSummaryCard() = default;

void VideoSummaryCard::setThemeService(ThemeService* theme)
{
    if (m_theme == theme) return;
    if (m_theme) disconnect(m_theme, nullptr, this, nullptr);
    m_theme = theme;
    m_renderer->setThemeService(theme);
    if (m_theme) {
        connect(m_theme, &ThemeService::themeChanged,
                this, [this](bool) { applyTheme(); });
    }
    applyTheme();
}

void VideoSummaryCard::setSummary(const QString& summary)
{
    // 保留原始 Markdown，主题变化不经过 toMarkdown() 往返转换。
    m_summary = summary.trimmed();
    m_markdownBody = m_summary;
    m_typeLabel->clear();
    m_typeLabel->hide();

    // 分离分析服务附在摘要首行的视频类型，旧缓存没有这一行也能显示。
    static const QRegularExpression typeLine(QStringLiteral(
        "\\A(?:📌[\\t ]*)?视频类型[：:][\\t ]*([^\\r\\n]+)(?:\\r?\\n|$)"));
    const auto typeMatch = typeLine.match(m_markdownBody);
    if (typeMatch.hasMatch()) {
        const QString type = typeMatch.captured(1).trimmed();
        m_typeLabel->setText(type);
        m_typeLabel->setVisible(!type.isEmpty());
        m_markdownBody.remove(0, typeMatch.capturedLength());
        m_markdownBody = m_markdownBody.trimmed();
    }

    // 卡片已有标题，去掉正文开头重复的“视频概览”标题。
    static const QRegularExpression overviewHeading(QStringLiteral(
        "\\A#{1,6}[\\t ]+视频概览[\\t ]*(?:\\r?\\n|$)"));
    const auto headingMatch = overviewHeading.match(m_markdownBody);
    if (headingMatch.hasMatch()) {
        m_markdownBody.remove(0, headingMatch.capturedLength());
        m_markdownBody = m_markdownBody.trimmed();
    }

    m_copyButton->setEnabled(!m_summary.isEmpty());
    renderSummary();
}

void VideoSummaryCard::clear()
{
    m_summary.clear();
    m_markdownBody.clear();
    m_typeLabel->clear();
    m_typeLabel->hide();
    m_copyButton->setEnabled(false);
    m_browser->clear();
}

void VideoSummaryCard::setContentType(const QString& type) {m_typeLabel->setText(type);m_typeLabel->setVisible(!type.isEmpty());}

void VideoSummaryCard::applyTheme()
{
    const bool dark = m_theme ? m_theme->isDark() : true;
    const QColor text = m_theme ? m_theme->color("textPrimary")
                               : QColor(dark ? "#E0E0E0" : "#1A1A1A");
    const QColor secondary = m_theme ? m_theme->color("textSecondary")
                                    : QColor(dark ? "#8B8B8B" : "#6B6B6B");
    const QColor primary = m_theme ? m_theme->color("primary")
                                  : QColor(dark ? "#2979FF" : "#1565C0");
    const QColor surface = m_theme ? m_theme->color("surface")
                                  : QColor(dark ? "#1E1E2E" : "#FFFFFF");
    const QColor variant = m_theme ? m_theme->color("surfaceVariant")
                                  : QColor(dark ? "#252538" : "#F5F5F5");
    const QColor border = m_theme ? m_theme->color("border")
                                 : QColor(dark ? "#2D2D3D" : "#E0E0E0");

    setStyleSheet(QStringLiteral(
        "QWidget#videoSummaryCard { background:%1; border:1px solid %2; border-radius:10px; }"
        "QLabel#summaryTitle { color:%3; font-size:15px; font-weight:600; background:transparent; border:none; }"
        "QLabel#summaryType { color:%4; background:%5; border:none; border-radius:5px; padding:3px 8px; font-size:11px; }"
        "QToolButton#summaryCopy { color:%4; font-size:12px; background:transparent; border:none; border-radius:5px; padding:5px 8px; }"
        "QToolButton#summaryCopy:hover { color:%3; background:%5; }"
        "QToolButton#summaryCopy:pressed { background:%2; }"
        "QToolButton#summaryCopy:disabled { color:%4; }"
        "QTextBrowser#summaryBody { color:%3; font-size:14px; background:transparent; border:none; padding:0; selection-background-color:%6; }"
    ).arg(surface.name(), border.name(), text.name(), secondary.name(),
          variant.name(), primary.name()));

    QFont font = m_browser->font();
    font.setPixelSize(14);
    m_browser->document()->setDefaultFont(font);
    m_browser->document()->setDefaultStyleSheet(QStringLiteral(
        "body { color:%1; font-size:14px; }"
        "p { margin-top:0px; margin-bottom:10px; }"
        "h1 { color:%1; font-size:18px; font-weight:600; margin-top:16px; margin-bottom:8px; }"
        "h2 { color:%1; font-size:16px; font-weight:600; margin-top:14px; margin-bottom:7px; }"
        "h3,h4,h5,h6 { color:%1; font-size:14px; font-weight:600; margin-top:12px; margin-bottom:6px; }"
        "ul,ol { margin-top:4px; margin-bottom:10px; margin-left:16px; }"
        "li { margin-top:0px; margin-bottom:5px; }"
        "strong,b { color:%1; font-weight:600; }"
        "a { color:%2; text-decoration:none; }"
        "blockquote { color:%3; margin-top:8px; margin-bottom:10px; margin-left:12px; }"
        "code { font-family:Consolas,'Courier New',monospace; font-size:13px; background-color:%4; }"
        "pre { font-family:Consolas,'Courier New',monospace; font-size:13px; background-color:%4; white-space:pre-wrap; margin-top:8px; margin-bottom:10px; }"
        "table { border-collapse:collapse; }"
        "th,td { border:1px solid %5; padding:6px 8px; }"
        "th { background-color:%4; font-weight:600; }"
    ).arg(text.name(), primary.name(), secondary.name(), variant.name(), border.name()));

    // 隐藏 Tab 也更新渲染数据；尺寸留到实际显示时同步。
    renderSummary();
}

void VideoSummaryCard::renderSummary()
{
    const bool dark = m_theme ? m_theme->isDark() : true;
    m_browser->setHtml(m_renderer->toHtmlBody(m_markdownBody, dark));

    // 用 Qt 原生段落格式设置行距，避免依赖浏览器式 CSS 的隐式继承。
    QTextDocument* doc = m_browser->document();
    QTextCursor cursor(doc);
    cursor.beginEditBlock();
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        QTextBlockFormat format = block.blockFormat();
        format.setLineHeight(format.headingLevel() > 0 ? 135 : 165,
                             QTextBlockFormat::ProportionalHeight);
        cursor.setPosition(block.position());
        cursor.setBlockFormat(format);
    }
    cursor.endEditBlock();
}
