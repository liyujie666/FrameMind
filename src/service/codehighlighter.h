#ifndef FRAMEMIND_CODEHIGHLIGHTER_H
#define FRAMEMIND_CODEHIGHLIGHTER_H

#include <QString>
#include <QColor>
#include <QHash>
#include <QSet>

class ThemeService;

/**
 * 代码语法高亮器
 * 
 * 为常见编程语言生成带语法高亮的 HTML。
 * 支持：C++, Python, JavaScript, JSON, Bash 等。
 * 使用简单的关键字匹配，不依赖外部库。
 */
class CodeHighlighter {
public:
    explicit CodeHighlighter(ThemeService* theme = nullptr);

    /**
     * 对代码进行语法高亮
     * @param code 原始代码
     * @param language 语言标识（cpp, python, javascript, json 等）
     * @param isDarkTheme 是否为深色主题
     * @return 高亮后的 HTML 片段
     */
    QString highlight(const QString& code, const QString& language, bool isDarkTheme = true) const;

    void setThemeService(ThemeService* theme) { m_theme = theme; }

private:
    struct ColorScheme {
        QColor keyword;      // 关键字（if, while, class 等）
        QColor string;       // 字符串
        QColor comment;      // 注释
        QColor number;       // 数字
        QColor function;     // 函数名
        QColor type;         // 类型（int, void, QString 等）
        QColor operator_;    // 操作符
        QColor variable;     // 变量
    };

    ColorScheme getDarkScheme() const;
    ColorScheme getLightScheme() const;

    QString highlightCpp(const QString& code, const ColorScheme& colors) const;
    QString highlightPython(const QString& code, const ColorScheme& colors) const;
    QString highlightJavaScript(const QString& code, const ColorScheme& colors) const;
    QString highlightJson(const QString& code, const ColorScheme& colors) const;
    QString highlightBash(const QString& code, const ColorScheme& colors) const;
    QString highlightPlainText(const QString& code, const ColorScheme& colors) const;

    QString escapeHtml(const QString& text) const;
    QString wrapSpan(const QString& text, const QColor& color) const;

    ThemeService* m_theme = nullptr;

    // 语言关键字缓存
    QSet<QString> m_cppKeywords;
    QSet<QString> m_pythonKeywords;
    QSet<QString> m_jsKeywords;
    QSet<QString> m_bashKeywords;
};

#endif // FRAMEMIND_CODEHIGHLIGHTER_H
