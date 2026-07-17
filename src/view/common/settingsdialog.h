#ifndef FRAMEMIND_SETTINGSDIALOG_H
#define FRAMEMIND_SETTINGSDIALOG_H

#include <QDialog>
#include <QColor>
#include <QString>

class QStackedWidget;
class QListWidget;
class QToolButton;
class QWidget;
class QComboBox;
class QLineEdit;
class QLabel;
class QPushButton;
class ThemeService;
class SettingsService;
class AgentService;
class LLMProviderService;

/**
 * 设置对话框：
 * - 左侧：导航列表（常规、主题、AI）
 * - 右侧：对应页面内容
 * - 整体随主题颜色更新
 */
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(ThemeService* theme,
                           SettingsService* settings,
                           AgentService* agent,
                           LLMProviderService* providers,
                           QWidget* parent = nullptr);

signals:
    void openVideoRequested(const QString& path);

protected:
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void onNavItemClicked(int row);
    void onOpenVideo();
    void onProviderChanged(int index);
    void onSaveAiSettings();
    void onTestConnection();
    void onConnectionTestResult(const QString& providerId, bool success, const QString& message);

private:
    void buildGeneralPage(QWidget* parent);
    void buildThemePage(QWidget* parent);
    void buildAiPage(QWidget* parent);
    void applyThemeColors();
    void updateNavIcons();
    void refreshProviderFields();
    void updateProviderStatus(const class LLMProvider& provider);

    ThemeService* m_theme = nullptr;
    SettingsService* m_settings = nullptr;
    AgentService* m_agent = nullptr;
    LLMProviderService* m_providers = nullptr;

    QListWidget* m_navList = nullptr;
    QStackedWidget* m_contentStack = nullptr;

    // AI 页面控件
    QComboBox* m_providerCombo = nullptr;
    QLineEdit* m_endpointEdit = nullptr;
    QComboBox* m_modelCombo = nullptr;
    QLineEdit* m_apiKeyEdit = nullptr;
    QLabel* m_providerStatusLabel = nullptr;
    class QPushButton* m_testConnectionBtn = nullptr;
    bool m_connectionTesting = false;

    QColor m_bgColor;
    QColor m_navBgColor;
    QColor m_navHoverColor;
    QColor m_navActiveColor;
    QColor m_textColor;
    QColor m_textSecondaryColor;
    QColor m_borderColor;
};

#endif // FRAMEMIND_SETTINGSDIALOG_H
