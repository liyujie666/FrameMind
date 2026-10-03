#include "view/player/videosummarycard.h"

#include "service/themeservice.h"
#include "view/player/analysisuistyle.h"

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
{
    setObjectName(QStringLiteral("videoSummaryCard"));
    setAttribute(Qt::WA_StyledBackground, true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 10, 0, 0);
    layout->setSpacing(10);

    auto* header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 16, 0);
    header->setSpacing(10);
    m_title = new QLabel(tr("视频概览"), this);
    m_title->setTextFormat(Qt::PlainText);
    m_title->setObjectName(QStringLiteral("summaryTitle"));
    header->addWidget(m_title);

    header->addStretch(1);

    m_copyButton = new QToolButton(this);
    m_copyButton->setObjectName(QStringLiteral("analysisGhost"));
    m_copyButton->setIconSize(QSize(16, 16));
    m_copyButton->setProperty("copyAction", true);
    m_copyButton->setToolTip(tr("复制完整视频概览"));
    m_copyButton->setAccessibleName(tr("复制视频概览"));
    m_copyButton->setCursor(Qt::PointingHandCursor);
    m_copyButton->setEnabled(false);
    connect(m_copyButton, &QToolButton::clicked, this, [this]() {
        QApplication::clipboard()->setText(m_summary);
    });
    header->addWidget(m_copyButton);
    layout->addLayout(header);

    auto* bodyCard = new QFrame(this);
    bodyCard->setObjectName(QStringLiteral("summaryContentCard"));
    auto* bodyLayout = new QVBoxLayout(bodyCard);
    bodyLayout->setContentsMargins(16, 16, 16, 16);
    bodyLayout->setSpacing(0);

    m_browser = new SummaryTextBrowser(bodyCard);
    m_browser->setObjectName(QStringLiteral("summaryBody"));
    m_browser->viewport()->setAutoFillBackground(false);
    m_browser->setAccessibleName(tr("视频概览正文"));
    bodyLayout->addWidget(m_browser);
    layout->addWidget(bodyCard);

    applyTheme();
}

VideoSummaryCard::~VideoSummaryCard() = default;

void VideoSummaryCard::setThemeService(ThemeService* theme)
{
    if (m_theme == theme) return;
    if (m_theme) disconnect(m_theme, nullptr, this, nullptr);
    m_theme = theme;
    if (m_theme) {
        connect(m_theme, &ThemeService::themeChanged,
                this, [this](bool) { applyTheme(); });
    }
    applyTheme();
}

void VideoSummaryCard::setSummary(const QString& summary)
{
    if (m_summary == summary.trimmed()) return;
    // 保留原文；主题变化只调整纯文本排版。
    m_summary = summary.trimmed();
    m_displayBody = m_summary;

    // 分离分析服务附在摘要首行的视频类型，旧缓存没有这一行也能显示。
    static const QRegularExpression typeLine(QStringLiteral(
        "\\A(?:📌[\\t ]*)?视频类型[：:][\\t ]*([^\\r\\n]+)(?:\\r?\\n|$)"));
    const auto typeMatch = typeLine.match(m_displayBody);
    if (typeMatch.hasMatch()) {
        m_displayBody.remove(0, typeMatch.capturedLength());
        m_displayBody = m_displayBody.trimmed();
    }

    // 区域已有标题，去掉正文开头重复的“视频概览”标题。
    static const QRegularExpression overviewHeading(QStringLiteral(
        "\\A#{1,6}[\\t ]+视频概览[\\t ]*(?:\\r?\\n|$)"));
    const auto headingMatch = overviewHeading.match(m_displayBody);
    if (headingMatch.hasMatch()) {
        m_displayBody.remove(0, headingMatch.capturedLength());
        m_displayBody = m_displayBody.trimmed();
    }

    m_copyButton->setEnabled(!m_summary.isEmpty());
    renderSummary();
}

void VideoSummaryCard::clear()
{
    m_summary.clear();
    m_displayBody.clear();
    m_copyButton->setEnabled(false);
    m_browser->clear();
}

void VideoSummaryCard::applyTheme()
{
    const AnalysisUi::Colors colors(m_theme);
    m_copyButton->setIcon(AnalysisUi::copyIcon(colors));
    const bool dark = m_theme ? m_theme->isDark() : true;
    const QColor text = m_theme ? m_theme->color("textPrimary")
                               : QColor(dark ? "#E0E0E0" : "#1A1A1A");
    const QColor secondary = m_theme ? m_theme->color("textSecondary")
                                    : QColor(dark ? "#8B8B8B" : "#6B6B6B");
    const QColor primary = m_theme ? m_theme->color("primary")
                                  : QColor(dark ? "#2979FF" : "#1565C0");
    const QColor variant = m_theme ? m_theme->color("surfaceVariant")
                                  : QColor(dark ? "#252538" : "#F5F5F5");
    const QColor border = m_theme ? m_theme->color("border")
                                 : QColor(dark ? "#2D2D3D" : "#E0E0E0");

    setStyleSheet(QStringLiteral(
        "QWidget { background:transparent; border:none; }"
        "QWidget#videoSummaryCard { background:transparent; border:none; }"
        "QFrame#summaryContentCard { background:%1; border:1px solid %2; border-radius:10px; }"
        "QLabel#summaryTitle { color:%3; font-size:18px; font-weight:600; background:transparent; border:none; }"
        "QTextBrowser#summaryBody { color:%3; font-size:14px; background:transparent; border:none; padding:0; selection-background-color:%4; }"
    ).arg(AnalysisUi::cardSurface(colors).name(), AnalysisUi::cardBorder(colors).name(), text.name(), primary.name())
        + AnalysisUi::actionStyles(colors));

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
    m_browser->setPlainText(m_displayBody);

    // 用 Qt 原生段落格式设置行距，避免依赖浏览器式 CSS 的隐式继承。
    QTextDocument* doc = m_browser->document();
    QTextCursor cursor(doc);
    cursor.beginEditBlock();
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        QTextBlockFormat format = block.blockFormat();
        format.setLineHeight(format.headingLevel() > 0 ? 135 : 150,
                             QTextBlockFormat::ProportionalHeight);
        cursor.setPosition(block.position());
        cursor.setBlockFormat(format);
    }
    cursor.endEditBlock();
}
