#pragma once

#include "service/themeservice.h"
#include "model/video_presentation_types.h"
#include <QApplication>
#include <QEvent>
#include <QIcon>
#include <QLabel>
#include <QPainter>
#include <QPolygonF>
#include <QPixmap>
#include <QFontMetrics>
#include <QPen>
#include <QPalette>
#include <QResizeEvent>
#include <QVariant>

// Local presentation helpers. No global palette or model data is changed.
namespace AnalysisUi {
inline QColor blend(const QColor& base, const QColor& accent, qreal amount) {
    return QColor::fromRgbF(base.redF() * (1 - amount) + accent.redF() * amount,
        base.greenF() * (1 - amount) + accent.greenF() * amount,
        base.blueF() * (1 - amount) + accent.blueF() * amount);
}
struct Colors {
    QColor text, secondary, surface, border, primary, soft, hover, success, warning, error;
    bool dark;
    explicit Colors(const ThemeService* theme) {
        dark = theme ? theme->isDark() : qApp->palette().color(QPalette::Window).lightness() < 128;
        auto get = [theme, this](const char* token, const char* light, const char* night) {
            return theme ? theme->color(QString::fromLatin1(token)) : QColor(dark ? night : light);
        };
        text = get("textPrimary", "#1A1A1A", "#E0E0E0");
        secondary = get("textSecondary", "#6B6B6B", "#8B8B8B");
        surface = get("surface", "#FFFFFF", "#1E1E2E");
        border = get("border", "#E0E0E0", "#2D2D3D");
        primary = get("primary", "#1565C0", "#2979FF");
        success = QColor(dark ? "#69C49A" : "#247A57");
        warning = QColor(dark ? "#D7B578" : "#9B6B27");
        error = QColor(dark ? "#E48D98" : "#B34D58");
        soft = blend(surface, primary, dark ? .12 : .05);
        hover = blend(surface, primary, dark ? .08 : .03);
    }
};
inline QColor statusColor(const Colors& c, ArtifactState state) {
    switch (state) {
    case ArtifactState::Ready: return c.success;
    case ArtifactState::Running: return c.primary;
    case ArtifactState::Partial: return c.warning;
    case ArtifactState::Failed: return c.error;
    case ArtifactState::Pending:
    case ArtifactState::Skipped:
    case ArtifactState::Cancelled: return c.secondary;
    }
    return c.secondary;
}
inline QIcon copyIcon(const Colors& c) {
    return QIcon(c.dark ? ":/icons/copy_light.png" : ":/icons/copy_dark.png");
}
enum class Icon { Copy, ChevronDown, ChevronUp, Play, ArrowRight, Refresh };
inline QColor cardSurface(const Colors& c) {
    return c.dark ? blend(c.surface, c.primary, .04) : QColor("#F5F7FB");
}
inline QColor cardBorder(const Colors& c) {
    return c.dark ? blend(c.border, c.primary, .16) : QColor("#E4EAF5");
}
inline QIcon icon(Icon kind, QColor color) {
    // Paint at 2x with a logical 16px canvas for crisp desktop/DPI rendering.
    auto paint = [kind](QColor ink) {
        QPixmap pixmap(32, 32);
        pixmap.setDevicePixelRatio(2);
        pixmap.fill(Qt::transparent);
        QPainter p(&pixmap);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(ink, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        if (kind == Icon::Copy) {
            p.drawRoundedRect(QRectF(6, 2, 8, 10), 1.5, 1.5);
            p.drawRoundedRect(QRectF(2, 6, 8, 8), 1.5, 1.5);
        } else if (kind == Icon::ChevronDown || kind == Icon::ChevronUp) {
            const bool up = kind == Icon::ChevronUp;
            p.drawLine(QPointF(4, up ? 10 : 6), QPointF(8, up ? 6 : 10));
            p.drawLine(QPointF(8, up ? 6 : 10), QPointF(12, up ? 10 : 6));
        } else if (kind == Icon::Play) {
            QPolygonF triangle{QPointF(5, 3), QPointF(12, 8), QPointF(5, 13)};
            p.setBrush(ink); p.drawPolygon(triangle);
        } else if (kind == Icon::ArrowRight) {
            p.drawLine(QPointF(3, 8), QPointF(13, 8));
            p.drawLine(QPointF(9, 4), QPointF(13, 8));
            p.drawLine(QPointF(13, 8), QPointF(9, 12));
        } else {
            p.drawArc(QRectF(3, 3, 10, 10), 45 * 16, 280 * 16);
            p.drawLine(QPointF(12, 2), QPointF(12, 6));
            p.drawLine(QPointF(8, 6), QPointF(12, 6));
        }
        return pixmap;
    };
    QIcon result(paint(color));
    color.setAlpha(100);
    result.addPixmap(paint(color), QIcon::Disabled);
    return result;
}
inline QString actionStyles(const Colors& c) {
    return QStringLiteral(
        "QToolButton#analysisGhost, QPushButton#analysisAsk { color:%1; background:transparent; border:1px solid transparent; border-radius:8px; padding:7px 9px; font-size:12px; }"
        "QToolButton#analysisGhost:hover, QPushButton#analysisAsk:hover { background:%2; }"
        "QToolButton#analysisGhost:focus, QPushButton#analysisAsk:focus { border-color:%1; }"
        "QToolButton#analysisGhost:disabled, QPushButton#analysisAsk:disabled { color:%3; }"
        "QToolButton#analysisReview { color:%1; background:%2; border:1px solid %4; border-radius:8px; padding:6px 10px; font-size:12px; }"
        "QToolButton#analysisReview:hover, QToolButton#analysisReview:focus { border-color:%1; }"
    ).arg(c.primary.name(), c.soft.name(), c.secondary.name(), blend(c.border, c.primary, .18).name());
}
class PreviewLabel final : public QLabel {
public:
    explicit PreviewLabel(const QString& fullText, QWidget* parent)
        : QLabel(parent), m_fullText(fullText.simplified()) {
        setTextFormat(Qt::PlainText);
        setToolTip(fullText);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        updateText();
    }
    void setFullText(const QString& text) {
        m_fullText = text.simplified(); setToolTip(text); updateText();
    }
protected:
    void resizeEvent(QResizeEvent* event) override { QLabel::resizeEvent(event); updateText(); }
    void changeEvent(QEvent* event) override {
        QLabel::changeEvent(event);
        if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) updateText();
    }
private:
    void updateText() { setText(fontMetrics().elidedText(m_fullText, Qt::ElideRight, qMax(0, contentsRect().width()))); }
    QString m_fullText;
};
}
