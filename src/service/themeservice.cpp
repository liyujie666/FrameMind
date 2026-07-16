#include "service/themeservice.h"
#include "service/settingsservice.h"

#include <QApplication>
#include <QFile>
#include <QPalette>
#include <QWidget>

ThemeService::ThemeService(SettingsService* settings, QObject* parent)
    : QObject(parent)
    , m_settings(settings)
{
    const QString saved = m_settings
        ? m_settings->get(QStringLiteral("ui.theme_mode"))
        : QString();
    if (!saved.isEmpty()) {
        if (saved == QStringLiteral("light")) {
            m_mode = ThemeMode::Light;
        } else if (saved == QStringLiteral("dark")) {
            m_mode = ThemeMode::Dark;
        } else {
            m_mode = ThemeMode::FollowSystem;
        }
    }

    applyTheme();
}

void ThemeService::setThemeMode(ThemeMode mode)
{
    if (m_mode == mode) return;
    m_mode = mode;

    if (m_settings) {
        QString val;
        switch (mode) {
            case ThemeMode::Light:   val = QStringLiteral("light");   break;
            case ThemeMode::Dark:    val = QStringLiteral("dark");    break;
            default:                 val = QStringLiteral("system");  break;
        }
        m_settings->set(QStringLiteral("ui.theme_mode"), val);
    }

    applyTheme();
}

ThemeService::ThemeMode ThemeService::themeMode() const
{
    return m_mode;
}

bool ThemeService::isDark() const
{
    if (m_mode == ThemeMode::FollowSystem) {
        const QPalette pal = qApp->palette();
        const QColor bg = pal.color(QPalette::Window);
        const int g = bg.red() * 299 + bg.green() * 587 + bg.blue() * 114;
        return g < 128000;
    }
    return m_mode == ThemeMode::Dark;
}

void ThemeService::applyTheme()
{
    const bool dark = isDark();

    QString qss;
    const QString styleFile = dark
        ? QStringLiteral(":/styles/dark.qss")
        : QStringLiteral(":/styles/light.qss");

    QFile f(styleFile);
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qss = QString::fromUtf8(f.readAll());
        f.close();
    }

    if (!qss.isEmpty()) {
        // 性能优化：在应用全局样式表前暂停所有顶层窗口的更新，
        // 避免 setStyleSheet 逐个遍历 widget 树时触发大量中间重绘。
        const auto topLevelWidgets = qApp->topLevelWidgets();
        for (QWidget* w : topLevelWidgets) {
            w->setUpdatesEnabled(false);
        }

        qApp->setStyleSheet(qss);

        for (QWidget* w : topLevelWidgets) {
            w->setUpdatesEnabled(true);
        }
    }

    emit themeChanged(dark);
}

QColor ThemeService::color(const QString& token) const
{
    const bool dark = isDark();

    static const QMap<QString, QPair<QColor, QColor>> colors = {
        { QStringLiteral("primary"),           { { 0x15, 0x65, 0xC0 }, { 0x29, 0x79, 0xFF } } },
        { QStringLiteral("primaryHover"),      { { 0x19, 0x76, 0xD2 }, { 0x44, 0x8A, 0xFF } } },
        { QStringLiteral("primaryPressed"),    { { 0x0D, 0x47, 0xA1 }, { 0x15, 0x65, 0xC0 } } },
        { QStringLiteral("background"),        { { 0xF8, 0xF9, 0xFA }, { 0x0D, 0x11, 0x17 } } },
        { QStringLiteral("surface"),           { { 0xFF, 0xFF, 0xFF }, { 0x1E, 0x1E, 0x2E } } },
        { QStringLiteral("surfaceVariant"),    { { 0xF5, 0xF5, 0xF5 }, { 0x25, 0x25, 0x38 } } },
        { QStringLiteral("sidebar"),           { { 0xF0, 0xF1, 0xF3 }, { 0x16, 0x16, 0x22 } } },
        { QStringLiteral("textPrimary"),       { { 0x1A, 0x1A, 0x1A }, { 0xE0, 0xE0, 0xE0 } } },
        { QStringLiteral("textSecondary"),     { { 0x6B, 0x6B, 0x6B }, { 0x8B, 0x8B, 0x8B } } },
        { QStringLiteral("textOnPrimary"),      { { 0xFF, 0xFF, 0xFF }, { 0xFF, 0xFF, 0xFF } } },
        { QStringLiteral("border"),            { { 0xE0, 0xE0, 0xE0 }, { 0x2D, 0x2D, 0x3D } } },
        { QStringLiteral("borderFocused"),      { { 0x15, 0x65, 0xC0 }, { 0x29, 0x79, 0xFF } } },
        { QStringLiteral("userBubble"),         { { 0x15, 0x65, 0xC0 }, { 0x29, 0x79, 0xFF } } },
        { QStringLiteral("userBubbleText"),    { { 0xFF, 0xFF, 0xFF }, { 0xFF, 0xFF, 0xFF } } },
        { QStringLiteral("aiBubble"),          { { 0xF0, 0xF2, 0xF5 }, { 0x25, 0x25, 0x36 } } },
        { QStringLiteral("aiBubbleText"),      { { 0x1A, 0x1A, 0x1A }, { 0xE0, 0xE0, 0xE0 } } },
        { QStringLiteral("inputBg"),           { { 0xFF, 0xFF, 0xFF }, { 0x1A, 0x1A, 0x2A } } },
        { QStringLiteral("scrollThumb"),       { { 0xC0, 0xC0, 0xC0 }, { 0x3A, 0x3A, 0x4A } } },
    };

    auto it = colors.find(token);
    if (it != colors.end()) {
        return dark ? it.value().second : it.value().first;
    }
    return dark ? Qt::white : Qt::black;
}
