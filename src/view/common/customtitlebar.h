#ifndef FRAMEMIND_CUSTOMTITLEBAR_H
#define FRAMEMIND_CUSTOMTITLEBAR_H

#include <QWidget>
#include <QColor>

class QToolButton;
class QLabel;
class ThemeService;

/**
 * 自定义标题栏：
 * - 左侧：Logo 图标 + 标题文字
 * - 右侧：主题切换按钮 + 最小化 + 最大化 + 关闭按钮
 * - 颜色随主题切换自动更新
 * - 支持拖动移动窗口
 */
class CustomTitleBar : public QWidget {
    Q_OBJECT
public:
    explicit CustomTitleBar(ThemeService* theme, QWidget* parent = nullptr);
    void setThemeService(ThemeService* theme);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private slots:
    void onMinimize();
    void onMaximize();
    void onClose();
    void onThemeToggle();

private:
    void applyThemeColors();
    void updateWindowButtonStates();
    void updateThemeToggleIcon();
    void updateLogo();

    ThemeService* m_theme = nullptr;
    QWidget* m_parentWindow = nullptr;

    QLabel* m_logoLabel = nullptr;
    QToolButton* m_minimizeBtn = nullptr;
    QToolButton* m_maximizeBtn = nullptr;
    QToolButton* m_closeBtn = nullptr;
    QToolButton* m_themeToggleBtn = nullptr;

    QColor m_bgColor;
    QColor m_iconColor;
    QColor m_iconHoverColor;
    QColor m_closeHoverColor;

    bool m_isMaximized = false;
    bool m_dragging = false;
    QPoint m_dragStartPos;
    QPoint m_windowStartPos;
};

#endif // FRAMEMIND_CUSTOMTITLEBAR_H
