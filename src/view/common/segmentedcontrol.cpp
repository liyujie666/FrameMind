#include "view/common/segmentedcontrol.h"
#include "service/themeservice.h"

#include <QPainter>
#include <QPainterPath>
#include <QPropertyAnimation>
#include <QEasingCurve>
#include <QMouseEvent>
#include <QFontMetrics>

SegmentedControl::SegmentedControl(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_StyledBackground, false);
    setAutoFillBackground(false);
    setCursor(Qt::PointingHandCursor);
    setMinimumHeight(kHeightHint);

    m_anim = new QPropertyAnimation(this, "thumbRect", this);
    m_anim->setDuration(220);
    m_anim->setEasingCurve(QEasingCurve::OutCubic);

    // 默认暗色 fallback
    m_bg           = QColor("#252538");
    m_thumbColor   = QColor("#2979FF");
    m_activeText   = QColor("#FFFFFF");
    m_inactiveText = QColor("#8B8B8B");
}

void SegmentedControl::setThemeService(ThemeService* theme)
{
    if (m_theme == theme) return;
    if (m_theme) disconnect(m_theme, nullptr, this, nullptr);
    m_theme = theme;
    if (m_theme) {
        connect(m_theme, &ThemeService::themeChanged,
                this, [this]() { refreshColors(); });
        refreshColors();
    }
}

void SegmentedControl::refreshColors()
{
    if (m_theme) {
        m_bg           = m_theme->color(QStringLiteral("surfaceVariant"));
        m_thumbColor   = m_theme->color(QStringLiteral("primary"));
        m_activeText   = m_theme->color(QStringLiteral("textOnPrimary"));
        m_inactiveText = m_theme->color(QStringLiteral("textSecondary"));
    }
    update();
}

void SegmentedControl::setItems(const QStringList& items)
{
    m_items = items;
    if (m_currentIndex >= m_items.size()) m_currentIndex = 0;
    updateGeometry();
    updateThumbRectImmediate();
    update();
}

void SegmentedControl::setCurrentIndex(int index)
{
    if (index < 0 || index >= m_items.size()) return;
    if (index == m_currentIndex) return;
    m_currentIndex = index;

    const QRect target = thumbRectForIndex(index);
    m_anim->stop();
    m_anim->setStartValue(m_thumbRect);
    m_anim->setEndValue(target);
    m_anim->start();

    emit currentChanged(index);
    update();
}

void SegmentedControl::setThumbRect(const QRect& r)
{
    m_thumbRect = r;
    update();
}

QSize SegmentedControl::sizeHint() const
{
    if (m_items.isEmpty()) return QSize(200, kHeightHint);
    QFontMetrics fm(font());
    int maxW = 0;
    for (const auto& s : m_items) {
        maxW = qMax(maxW, fm.horizontalAdvance(s));
    }
    return QSize((maxW + 32) * m_items.size() + 2 * kPadding, kHeightHint);
}

QSize SegmentedControl::minimumSizeHint() const
{
    return QSize(160, kHeightHint);
}

QRect SegmentedControl::thumbRectForIndex(int index) const
{
    if (m_items.isEmpty()) return {};
    const int n = m_items.size();
    const int innerW = width() - 2 * kPadding;
    const int segW = innerW / n;
    const int x = kPadding + index * segW;
    return QRect(x, kPadding, segW, height() - 2 * kPadding);
}

void SegmentedControl::updateThumbRectImmediate()
{
    m_thumbRect = thumbRectForIndex(m_currentIndex);
}

void SegmentedControl::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    updateThumbRectImmediate();
}

int SegmentedControl::indexAt(const QPoint& pos) const
{
    if (m_items.isEmpty()) return -1;
    const int n = m_items.size();
    const int innerW = width() - 2 * kPadding;
    const int segW = innerW / n;
    const int idx = (pos.x() - kPadding) / qMax(1, segW);
    if (idx < 0 || idx >= n) return -1;
    return idx;
}

void SegmentedControl::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        const int idx = indexAt(event->pos());
        if (idx >= 0) setCurrentIndex(idx);
    }
    QWidget::mousePressEvent(event);
}

void SegmentedControl::paintEvent(QPaintEvent* /*event*/)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // 外层胶囊背景
    QPainterPath bgPath;
    bgPath.addRoundedRect(QRectF(rect()), kRadius, kRadius);
    p.fillPath(bgPath, m_bg);

    if (m_items.isEmpty()) return;

    // 滑块
    if (!m_thumbRect.isEmpty()) {
        QPainterPath thumbPath;
        thumbPath.addRoundedRect(QRectF(m_thumbRect),
                                 kRadius - 2, kRadius - 2);
        p.fillPath(thumbPath, m_thumbColor);
    }

    // 文本
    p.setFont(font());
    const int n = m_items.size();
    const int innerW = width() - 2 * kPadding;
    const int segW = innerW / n;
    for (int i = 0; i < n; ++i) {
        QRect segRect(kPadding + i * segW, kPadding,
                      segW, height() - 2 * kPadding);
        const bool active = (i == m_currentIndex);
        p.setPen(active ? m_activeText : m_inactiveText);
        p.drawText(segRect, Qt::AlignCenter, m_items.at(i));
    }
}
