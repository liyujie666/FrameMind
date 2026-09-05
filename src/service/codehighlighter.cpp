#include "service/codehighlighter.h"
#include "service/themeservice.h"

#include <QRegularExpression>

CodeHighlighter::CodeHighlighter(ThemeService* theme)
    : m_theme(theme)
{
    // C++ 关键字
    m_cppKeywords = {
        "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor",
        "bool", "break", "case", "catch", "char", "char8_t", "char16_t", "char32_t",
        "class", "compl", "concept", "const", "consteval", "constexpr", "constinit",
        "const_cast", "continue", "co_await", "co_return", "co_yield", "decltype",
        "default", "delete", "do", "double", "dynamic_cast", "else", "enum", "explicit",
        "export", "extern", "false", "float", "for", "friend", "goto", "if", "inline",
        "int", "long", "mutable", "namespace", "new", "noexcept", "not", "not_eq",
        "nullptr", "operator", "or", "or_eq", "private", "protected", "public",
        "register", "reinterpret_cast", "requires", "return", "short", "signed",
        "sizeof", "static", "static_assert", "static_cast", "struct", "switch",
        "template", "this", "thread_local", "throw", "true", "try", "typedef",
        "typeid", "typename", "union", "unsigned", "using", "virtual", "void",
        "volatile", "wchar_t", "while", "xor", "xor_eq"
    };

    m_pythonKeywords = {
        "False", "None", "True", "and", "as", "assert", "async", "await", "break",
        "class", "continue", "def", "del", "elif", "else", "except", "finally",
        "for", "from", "global", "if", "import", "in", "is", "lambda", "nonlocal",
        "not", "or", "pass", "raise", "return", "try", "while", "with", "yield"
    };

    m_jsKeywords = {
        "abstract", "arguments", "await", "boolean", "break", "byte", "case", "catch",
        "char", "class", "const", "continue", "debugger", "default", "delete", "do",
        "double", "else", "enum", "eval", "export", "extends", "false", "final",
        "finally", "float", "for", "function", "goto", "if", "implements", "import",
        "in", "instanceof", "int", "interface", "let", "long", "native", "new",
        "null", "package", "private", "protected", "public", "return", "short",
        "static", "super", "switch", "synchronized", "this", "throw", "throws",
        "transient", "true", "try", "typeof", "var", "void", "volatile", "while",
        "with", "yield"
    };

    m_bashKeywords = {
        "if", "then", "else", "elif", "fi", "case", "esac", "for", "select",
        "while", "until", "do", "done", "in", "function", "time", "coproc",
        "echo", "exit", "return", "break", "continue", "shift", "export",
        "readonly", "declare", "local", "unset", "source"
    };
}

CodeHighlighter::ColorScheme CodeHighlighter::getDarkScheme() const
{
    ColorScheme scheme;
    scheme.keyword = QColor("#C586C0");
    scheme.string = QColor("#CE9178");
    scheme.comment = QColor("#6A9955");
    scheme.number = QColor("#B5CEA8");
    scheme.function = QColor("#DCDCAA");
    scheme.type = QColor("#4EC9B0");
    scheme.operator_ = QColor("#D4D4D4");
    scheme.variable = QColor("#9CDCFE");
    return scheme;
}

CodeHighlighter::ColorScheme CodeHighlighter::getLightScheme() const
{
    ColorScheme scheme;
    scheme.keyword = QColor("#AF00DB");
    scheme.string = QColor("#A31515");
    scheme.comment = QColor("#008000");
    scheme.number = QColor("#098658");
    scheme.function = QColor("#795E26");
    scheme.type = QColor("#267F99");
    scheme.operator_ = QColor("#000000");
    scheme.variable = QColor("#001080");
    return scheme;
}

QString CodeHighlighter::escapeHtml(const QString& text) const
{
    QString result = text;
    result.replace(QChar('&'), QStringLiteral("&amp;"));
    result.replace(QChar('<'), QStringLiteral("&lt;"));
    result.replace(QChar('>'), QStringLiteral("&gt;"));
    result.replace(QChar('"'), QStringLiteral("&quot;"));
    result.replace(QChar('\''), QStringLiteral("&#39;"));
    return result;
}

QString CodeHighlighter::wrapSpan(const QString& text, const QColor& color) const
{
    return QString("<span style='color:%1'>%2</span>").arg(color.name(), text);
}

QString CodeHighlighter::highlight(const QString& code, const QString& language, bool isDarkTheme) const
{
    const ColorScheme colors = isDarkTheme ? getDarkScheme() : getLightScheme();

    if (language == QStringLiteral("cpp") || language == QStringLiteral("c++") ||
        language == QStringLiteral("c")) {
        return highlightCpp(code, colors);
    } else if (language == QStringLiteral("python") || language == QStringLiteral("py")) {
        return highlightPython(code, colors);
    } else if (language == QStringLiteral("javascript") || language == QStringLiteral("js") ||
               language == QStringLiteral("typescript") || language == QStringLiteral("ts")) {
        return highlightJavaScript(code, colors);
    } else if (language == QStringLiteral("json")) {
        return highlightJson(code, colors);
    } else if (language == QStringLiteral("bash") || language == QStringLiteral("sh")) {
        return highlightBash(code, colors);
    } else {
        return highlightPlainText(code, colors);
    }
}

QString CodeHighlighter::highlightPlainText(const QString& code, const ColorScheme& colors) const
{
    Q_UNUSED(colors);
    return escapeHtml(code);
}

QString CodeHighlighter::highlightCpp(const QString& code, const ColorScheme& colors) const
{
    QString result;
    QStringList lines = code.split('\n');
    for (int lineIdx = 0; lineIdx < lines.size(); ++lineIdx) {
        const QString& line = lines[lineIdx];
        QString processedLine;
        int i = 0;
        while (i < line.length()) {
            if (i < line.length() - 1 && line[i] == '/' && line[i + 1] == '/') {
                processedLine += wrapSpan(escapeHtml(line.mid(i)), colors.comment);
                break;
            }
            if (line[i] == '"') {
                int start = i++;
                while (i < line.length() && (line[i] != '"' || line[i - 1] == '\\')) i++;
                if (i < line.length()) i++;
                processedLine += wrapSpan(escapeHtml(line.mid(start, i - start)), colors.string);
                continue;
            }
            if (line[i].isDigit()) {
                int start = i;
                while (i < line.length() && (line[i].isLetterOrNumber() || line[i] == '.')) i++;
                processedLine += wrapSpan(escapeHtml(line.mid(start, i - start)), colors.number);
                continue;
            }
            if (line[i].isLetter() || line[i] == '_') {
                int start = i;
                while (i < line.length() && (line[i].isLetterOrNumber() || line[i] == '_')) i++;
                QString word = line.mid(start, i - start);
                if (m_cppKeywords.contains(word)) {
                    processedLine += wrapSpan(escapeHtml(word), colors.keyword);
                } else {
                    processedLine += escapeHtml(word);
                }
                continue;
            }
            processedLine += escapeHtml(QString(line[i]));
            i++;
        }
        result += processedLine;
        if (lineIdx < lines.size() - 1) {
            result += "<br>";
        }
    }
    return result;
}

QString CodeHighlighter::highlightPython(const QString& code, const ColorScheme& colors) const
{
    QString result;
    QStringList lines = code.split('\n');
    for (int lineIdx = 0; lineIdx < lines.size(); ++lineIdx) {
        const QString& line = lines[lineIdx];
        QString processedLine;
        int i = 0;
        while (i < line.length()) {
            if (line[i] == '#') {
                processedLine += wrapSpan(escapeHtml(line.mid(i)), colors.comment);
                break;
            }
            if (line[i] == '"' || line[i] == '\'') {
                QChar quote = line[i];
                int start = i++;
                while (i < line.length() && (line[i] != quote || line[i - 1] == '\\')) i++;
                if (i < line.length()) i++;
                processedLine += wrapSpan(escapeHtml(line.mid(start, i - start)), colors.string);
                continue;
            }
            if (line[i].isDigit()) {
                int start = i;
                while (i < line.length() && (line[i].isLetterOrNumber() || line[i] == '.')) i++;
                processedLine += wrapSpan(escapeHtml(line.mid(start, i - start)), colors.number);
                continue;
            }
            if (line[i].isLetter() || line[i] == '_') {
                int start = i;
                while (i < line.length() && (line[i].isLetterOrNumber() || line[i] == '_')) i++;
                QString word = line.mid(start, i - start);
                if (m_pythonKeywords.contains(word)) {
                    processedLine += wrapSpan(escapeHtml(word), colors.keyword);
                } else {
                    processedLine += escapeHtml(word);
                }
                continue;
            }
            processedLine += escapeHtml(QString(line[i]));
            i++;
        }
        result += processedLine;
        if (lineIdx < lines.size() - 1) {
            result += "<br>";
        }
    }
    return result;
}

QString CodeHighlighter::highlightJavaScript(const QString& code, const ColorScheme& colors) const { return highlightCpp(code, colors); }
QString CodeHighlighter::highlightJson(const QString& code, const ColorScheme& colors) const { return highlightPlainText(code, colors); }
QString CodeHighlighter::highlightBash(const QString& code, const ColorScheme& colors) const { return highlightPython(code, colors); }
