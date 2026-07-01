#include "view/common/themedpanel.h"
#include "service/themeservice.h"

#include <QPainter>
#include <QPainterPath>

ThemedPanel::ThemedPanel(QWidget* parent)
    : QWidget(parent)
{
    setAutoFillBackground(false);
    setAttribute(Qt::WA_StyledBackground, false);
    // 默认暗色 fallback
    m_bg     = QColor("#1E1E2E");
    m_border = QColor("#2D2D3D");
}

void ThemedPanel::setThemeService(ThemeService* theme)
{
    if (m_theme == theme) return;
    if (m_theme) {
        disconnect(m_theme, nullptr, this, nullptr);
    }
    m_theme = theme;
    if (m_theme) {
        connect(m_theme, &ThemeService::themeChanged,
                this, &ThemedPanel::onThemeChanged);
        onThemeChanged();
    }
}

void ThemedPanel::setRadius(int radius)
{
    if (m_radius == radius) return;
    m_radius = radius;
    update();
}

void ThemedPanel::setBorderVisible(bool visible)
{
    if (m_borderVisible == visible) return;
    m_borderVisible = visible;
    update();
}

void ThemedPanel::onThemeChanged()
{
    if (!m_theme) return;
    m_bg     = m_theme->color(QStringLiteral("surface"));
    m_border = m_theme->color(QStringLiteral("border"));
    update();
}

void ThemedPanel::paintEvent(QPaintEvent* /*event*/)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // 内缩 0.5px 以让边框像素对齐
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath path;
    path.addRoundedRect(r, m_radius, m_radius);

    p.fillPath(path, m_bg);

    if (m_borderVisible) {
        QPen pen(m_border);
        pen.setWidth(1);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    }
}
