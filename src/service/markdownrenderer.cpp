#include "service/markdownrenderer.h"
#include "service/themeservice.h"
#include "service/codehighlighter.h"

#include <QTextDocument>
#include <QRegularExpression>

MarkdownRenderer::MarkdownRenderer(ThemeService* theme)
    : m_theme(theme)
    , m_highlighter(new CodeHighlighter(theme))
{
}

MarkdownRenderer::~MarkdownRenderer()
{
    delete m_highlighter;
}

void MarkdownRenderer::setThemeService(ThemeService* theme)
{
    m_theme = theme;
    if (m_highlighter) {
        m_highlighter->setThemeService(theme);
    }
}

QString MarkdownRenderer::generateStyle(bool isDarkTheme) const
{
    QColor bgColor, textColor, codeBlockBg, inlineCodeBg;
    QColor quoteBg, quoteBorder, linkColor, tableBorder;

    if (isDarkTheme) {
        bgColor = QColor("#1E1E2E");
        textColor = QColor("#E0E0E0");
        codeBlockBg = QColor("#171721");
        inlineCodeBg = QColor("#2A2A3A");
        quoteBg = QColor("#2B2B3A");
        quoteBorder = QColor("#4C8DFF");
        linkColor = QColor("#2979FF");
        tableBorder = QColor("#3A3A4A");
    } else {
        bgColor = QColor("#FFFFFF");
        textColor = QColor("#1E1E1E");
        codeBlockBg = QColor("#F5F5F5");
        inlineCodeBg = QColor("#E8E8E8");
        quoteBg = QColor("#F0F0F0");
        quoteBorder = QColor("#2979FF");
        linkColor = QColor("#1565C0");
        tableBorder = QColor("#DDDDDD");
    }

    if (m_theme) {
        textColor = m_theme->color(QStringLiteral("textPrimary"));
        linkColor = m_theme->color(QStringLiteral("primary"));
        codeBlockBg = m_theme->color(QStringLiteral("inputBg"));
        quoteBorder = m_theme->color(QStringLiteral("primary"));
    }

    return QString(R"(
<style>
body { color: %1; font-family: 'Segoe UI', 'Microsoft YaHei UI', sans-serif; font-size: 14px; line-height: 1.6; margin: 0; padding: 0; }
p { margin: 0 0 12px 0; }
h1, h2, h3, h4, h5, h6 { margin: 16px 0 10px 0; font-weight: 600; line-height: 1.3; }
h1 { font-size: 24px; } h2 { font-size: 20px; } h3 { font-size: 18px; }
code { font-family: 'Cascadia Mono', 'Consolas', monospace; font-size: 13px; background: %2; padding: 2px 6px; border-radius: 3px; }
pre { background: %3; padding: 12px; border-radius: 6px; overflow-x: auto; margin: 10px 0; line-height: 1.5; }
pre code { background: transparent !important; padding: 0; display: inline; font-family: 'Cascadia Mono', 'Consolas', monospace; font-size: 13px; white-space: pre; }
pre code span { display: inline !important; background: transparent !important; }
blockquote { border-left: 4px solid %4; background: %5; margin: 10px 0; padding: 10px 14px; border-radius: 4px; }
ul, ol { margin: 8px 0; padding-left: 24px; }
li { margin: 4px 0; }
a { color: %6; text-decoration: none; }
a:hover { text-decoration: underline; }
table { border-collapse: collapse; margin: 10px 0; width: 100%%; }
th, td { border: 1px solid %7; padding: 8px 12px; text-align: left; }
th { font-weight: 600; background: %5; }
</style>
)")
        .arg(textColor.name(), inlineCodeBg.name(), codeBlockBg.name(),
             quoteBorder.name(), quoteBg.name(), linkColor.name(), tableBorder.name());
}

QString MarkdownRenderer::escapeHtml(const QString& text) const
{
    QString result = text;
    result.replace(QChar('&'), QStringLiteral("&amp;"));
    result.replace(QChar('<'), QStringLiteral("&lt;"));
    result.replace(QChar('>'), QStringLiteral("&gt;"));
    result.replace(QChar('"'), QStringLiteral("&quot;"));
    return result;
}

QString MarkdownRenderer::processMarkdown(const QString& markdown) const
{
    QString result = markdown;
    QList<QPair<QString, QString>> codeBlocks;
    
    // 第一步：提取所有代码块，用唯一ID替换
    QRegularExpression codeBlockRegex(
        QStringLiteral("```([a-zA-Z0-9]*)[\\r\\n]([\\s\\S]*?)```"));
    
    int blockIndex = 0;
    QStringList placeholders;
    auto it = codeBlockRegex.globalMatch(result);
    
    // 先收集所有匹配，避免在循环中修改字符串导致位置错误
    QList<QRegularExpressionMatch> matches;
    while (it.hasNext()) {
        matches.append(it.next());
    }
    
    // 从后向前替换，保持位置准确
    for (int i = matches.size() - 1; i >= 0; --i) {
        auto match = matches[i];
        QString language = match.captured(1);
        QString code = match.captured(2);
        QString placeholder = QString("XPLACEHOLDERX%1XPLACEHOLDERX").arg(i);
        
        codeBlocks.prepend(qMakePair(language, code));
        placeholders.prepend(placeholder);
        
        result.replace(match.capturedStart(), match.capturedLength(), placeholder);
    }
    
    // 第二步：让Qt解析剩余的markdown
    QTextDocument doc;
    doc.setMarkdown(result);
    QString html = doc.toHtml();
    
    // 第三步：提取body内容
    QRegularExpression bodyRegex(
        QStringLiteral("<body[^>]*>(.*)</body>"),
        QRegularExpression::DotMatchesEverythingOption);
    auto match = bodyRegex.match(html);
    if (match.hasMatch()) {
        html = match.captured(1);
    }
    
    // 第四步：移除inline样式
    html.remove(QRegularExpression(QStringLiteral("style=\"[^\"]*\"")));
    
    // 第五步：将占位符替换回代码块（处理可能被包裹的情况）
    for (int i = 0; i < codeBlocks.size(); ++i) {
        QString placeholder = placeholders[i];
        QString language = codeBlocks[i].first;
        QString code = codeBlocks[i].second;
        
        // 转义HTML
        code.replace(QChar('&'), QStringLiteral("&amp;"));
        code.replace(QChar('<'), QStringLiteral("&lt;"));
        code.replace(QChar('>'), QStringLiteral("&gt;"));
        code.replace(QChar('"'), QStringLiteral("&quot;"));
        
        // 生成单个pre标签
        QString codeBlock = QString("<pre><code class=\"language-%1\">%2</code></pre>")
                                .arg(language.isEmpty() ? "text" : language, code);
        
        // 处理可能被包裹在p标签中的情况
        QRegularExpression wrappedPlaceholder(
            QString("<p[^>]*>\\s*%1\\s*</p>").arg(QRegularExpression::escape(placeholder)));
        html.replace(wrappedPlaceholder, codeBlock);
        
        // 直接替换（如果没有被包裹）
        html.replace(placeholder, codeBlock);
    }
    
    return html.trimmed();
}

QString MarkdownRenderer::applyCodeHighlighting(const QString& html, bool isDarkTheme) const
{
    QString result = html;
    
    QRegularExpression codeBlockRegex(
        QStringLiteral("<pre><code class=\"language-([^\"]+)\">([\\s\\S]*?)</code></pre>"),
        QRegularExpression::MultilineOption);
    
    auto it = codeBlockRegex.globalMatch(result);
    QList<QPair<int, QPair<int, QString>>> replacements;
    
    while (it.hasNext()) {
        auto match = it.next();
        QString language = match.captured(1);
        QString code = match.captured(2);
        
        // 反转义HTML
        code.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
        code.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
        code.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
        code.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
        
        // 高亮代码
        QString highlighted = m_highlighter->highlight(code, language, isDarkTheme);
        QString replacement = QString("<pre><code>%1</code></pre>").arg(highlighted);
        
        replacements.append(qMakePair(match.capturedStart(), 
                                      qMakePair(match.capturedLength(), replacement)));
    }
    
    // 从后往前替换，避免位置偏移
    for (int i = replacements.size() - 1; i >= 0; --i) {
        result.replace(replacements[i].first, 
                      replacements[i].second.first, 
                      replacements[i].second.second);
    }
    
    return result;
}

QString MarkdownRenderer::toHtml(const QString& markdown, bool isDarkTheme) const
{
    if (markdown.isEmpty()) {
        return QStringLiteral("<p style='color:#8B8B8B;'>?????????</p>");
    }

    QString style = generateStyle(isDarkTheme);
    QString body = processMarkdown(markdown);
    body = applyCodeHighlighting(body, isDarkTheme);

    return style + body;
}
