#ifndef FRAMEMIND_THEMESERVICE_H
#define FRAMEMIND_THEMESERVICE_H

#include <QObject>
#include <QString>
#include <QColor>

class SettingsService;

/**
 * 主题服务：管理应用主题切换（暗色/亮色/跟随系统）。
 * 通过 DI 注入，不使用全局单例。
 */
class ThemeService : public QObject {
    Q_OBJECT
public:
    enum class ThemeMode { FollowSystem, Light, Dark };
    Q_ENUM(ThemeMode)

    explicit ThemeService(SettingsService* settings, QObject* parent = nullptr);

    void setThemeMode(ThemeMode mode);
    ThemeMode themeMode() const;
    bool isDark() const;
    void applyTheme();

    QColor color(const QString& token) const;

signals:
    void themeChanged(bool isDark);
    void themeChangedImmediate(bool isDark);

private:
    void onSystemThemeChanged();
    void applyPalette();

    SettingsService* m_settings = nullptr;
    ThemeMode m_mode = ThemeMode::FollowSystem;
};

#endif // FRAMEMIND_THEMESERVICE_H
