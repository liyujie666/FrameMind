#ifndef FRAMEMIND_SIDEBARVIEW_H
#define FRAMEMIND_SIDEBARVIEW_H

#include <QWidget>
#include <QList>
#include <QColor>

class QToolButton;
class QButtonGroup;
class ThemeService;

/**
 * 左侧导航栏（固定 64px 宽）：
 *   顶部头像 → 中间功能图标按钮组 → 底部设置按钮。
 * 激活态在按钮左侧绘制 3px 蓝色指示条。颜色随主题切换刷新。
 */
class SidebarView : public QWidget {
    Q_OBJECT
public:
    explicit SidebarView(QWidget* parent = nullptr);

    void setThemeService(ThemeService* theme);

signals:
    void pageRequested(int index);   // 预留：导航到指定页面
    void settingsClicked();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QToolButton* makeIconButton(const QString& iconPath, const QString& tip);
    void applyThemeColors();

    ThemeService*        m_theme = nullptr;
    QButtonGroup*        m_group = nullptr;
    QList<QToolButton*>  m_navButtons;
    QToolButton*         m_settingsButton = nullptr;

    QColor m_bgColor;
    QColor m_hoverBg;
    QColor m_indicator;
};

#endif // FRAMEMIND_SIDEBARVIEW_H
