#ifndef FRAMEMIND_SIDEBARVIEW_H
#define FRAMEMIND_SIDEBARVIEW_H

#include <QWidget>
#include <QList>

class QToolButton;
class QButtonGroup;

/**
 * 左侧导航栏（固定 64px 宽）：
 *   顶部头像 → 中间功能图标按钮组 → 底部设置按钮。
 * 激活态在按钮左侧绘制 3px 蓝色指示条。
 * M1 仅完成视觉与互斥选中，不接页面路由。
 */
class SidebarView : public QWidget {
    Q_OBJECT
public:
    explicit SidebarView(QWidget* parent = nullptr);

signals:
    void pageRequested(int index);   // 预留：导航到指定页面

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QToolButton* makeIconButton(const QString& iconPath, const QString& tip);
    void updatePalette();

    QButtonGroup*        m_group = nullptr;
    QList<QToolButton*>  m_navButtons;
    QToolButton*         m_settingsButton = nullptr;
};

#endif // FRAMEMIND_SIDEBARVIEW_H
