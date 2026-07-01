#ifndef FRAMEMIND_THEMEDPANEL_H
#define FRAMEMIND_THEMEDPANEL_H

#include <QWidget>
#include <QColor>

class ThemeService;

/**
 * 主题化圆角面板容器。
 *
 * 与页面背景色形成明显对比（surface vs background），自绘圆角背景 + 1px 边框。
 * 订阅 ThemeService::themeChanged，自动在暗/亮主题之间切换配色。
 * 内部子 widget 需要 setAutoFillBackground(false) 才不会覆盖圆角。
 */
class ThemedPanel : public QWidget {
    Q_OBJECT
public:
    explicit ThemedPanel(QWidget* parent = nullptr);

    /// 绑定主题服务（可空，为空时使用暗色默认值）
    void setThemeService(ThemeService* theme);

    /// 圆角半径（默认 12）
    void setRadius(int radius);

    /// 是否绘制 1px 边框（默认 true）
    void setBorderVisible(bool visible);

protected:
    void paintEvent(QPaintEvent* event) override;

private slots:
    void onThemeChanged();

private:
    ThemeService* m_theme = nullptr;
    int    m_radius = 12;
    bool   m_borderVisible = true;
    QColor m_bg;
    QColor m_border;
};

#endif // FRAMEMIND_THEMEDPANEL_H
