#ifndef FRAMEMIND_MARKDOWNRENDERER_H
#define FRAMEMIND_MARKDOWNRENDERER_H

#include <QString>
#include <QColor>

class ThemeService;
class CodeHighlighter;

/**
 * Markdown → HTML 转换服务（增强版，支持代码语法高亮）
 */
class MarkdownRenderer {
public:
    explicit MarkdownRenderer(ThemeService* theme = nullptr);
    ~MarkdownRenderer();

    QString toHtml(const QString& markdown, bool isDarkTheme = true) const;
    void setThemeService(ThemeService* theme);

private:
    QString generateStyle(bool isDarkTheme) const;
    QString escapeHtml(const QString& text) const;
    QString processMarkdown(const QString& markdown) const;
    QString applyCodeHighlighting(const QString& html, bool isDarkTheme) const;

    ThemeService* m_theme = nullptr;
    CodeHighlighter* m_highlighter = nullptr;
};

#endif // FRAMEMIND_MARKDOWNRENDERER_H
