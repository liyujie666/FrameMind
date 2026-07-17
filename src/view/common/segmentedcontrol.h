#ifndef FRAMEMIND_SEGMENTEDCONTROL_H
#define FRAMEMIND_SEGMENTEDCONTROL_H

#include <QWidget>
#include <QVector>
#include <QString>
#include <QColor>
#include <QRect>

class QPropertyAnimation;
class ThemeService;

/**
 * 现代滑块式分段控件（iOS 风格 Segmented Control）。
 *
 * 用法：
 *   auto* seg = new SegmentedControl(this);
 *   seg->setItems({tr("时间线"), tr("检测"), tr("字幕")});
 *   connect(seg, &SegmentedControl::currentChanged, ...);
 *
 * 视觉：
 *   - 外层圆角胶囊背景（surfaceVariant）
 *   - 选中项后面有一个圆角滑块（primary 底色），带 200ms 缓动动画
 *   - 选中/未选中文字颜色区分
 */
class SegmentedControl : public QWidget {
    Q_OBJECT
    Q_PROPERTY(QRect thumbRect READ thumbRect WRITE setThumbRect)
public:
    explicit SegmentedControl(QWidget* parent = nullptr);

    void setThemeService(ThemeService* theme);
    void setItems(const QStringList& items);
    int  currentIndex() const { return m_currentIndex; }

    QRect thumbRect() const { return m_thumbRect; }
    void  setThumbRect(const QRect& r);

public slots:
    void setCurrentIndex(int index);

signals:
    void currentChanged(int index);

protected:
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void updateThumbRectImmediate();
    QRect thumbRectForIndex(int index) const;
    int  indexAt(const QPoint& pos) const;
    void refreshColors();

    QStringList m_items;
    int m_currentIndex = 0;

    QRect m_thumbRect;
    QPropertyAnimation* m_anim = nullptr;

    ThemeService* m_theme = nullptr;
    QColor m_bg;
    QColor m_thumbColor;
    QColor m_activeText;
    QColor m_inactiveText;

    static constexpr int kRadius = 10;
    static constexpr int kPadding = 4;
    static constexpr int kHeightHint = 34;
};

#endif // FRAMEMIND_SEGMENTEDCONTROL_H
