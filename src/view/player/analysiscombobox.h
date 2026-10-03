#pragma once

#include "view/player/analysisuistyle.h"
#include <QComboBox>
#include <QEnterEvent>
#include <QFocusEvent>
#include <QFrame>
#include <QHideEvent>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QListView>
#include <QMouseEvent>
#include <QPointer>
#include <QScreen>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QVBoxLayout>

namespace AnalysisUi {
class ComboOptionDelegate final : public QStyledItemDelegate {
public:
    ComboOptionDelegate(QComboBox* combo, QObject* parent)
        : QStyledItemDelegate(parent), m_combo(combo), m_colors(nullptr) {}
    void setColors(const Colors& colors) { m_colors = colors; }
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        auto opt = option; initStyleOption(&opt, index);
        return QSize(opt.fontMetrics.horizontalAdvance(opt.text) + 46, qMax(38, opt.fontMetrics.height() + 18));
    }
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        auto opt = option; initStyleOption(&opt, index);
        const bool enabled = opt.state.testFlag(QStyle::State_Enabled);
        const bool current = m_combo && index.row() == m_combo->currentIndex();
        const bool highlighted = opt.state.testFlag(QStyle::State_Selected) || opt.state.testFlag(QStyle::State_MouseOver);
        const QRectF row = QRectF(opt.rect).adjusted(0, 2, 0, -2);
        painter->save(); painter->setRenderHint(QPainter::Antialiasing);
        if (highlighted || current) {
            painter->setPen(Qt::NoPen); painter->setBrush(m_colors.soft);
            painter->drawRoundedRect(row, 8, 8);
        }
        painter->setFont(opt.font);
        painter->setPen(!enabled ? m_colors.secondary : current ? m_colors.primary : m_colors.text);
        const QRect textRect = opt.rect.adjusted(12, 0, -34, 0);
        painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
            opt.fontMetrics.elidedText(opt.text, Qt::ElideRight, qMax(0, textRect.width())));
        if (current) {
            const qreal x = row.right() - 18, y = row.center().y();
            painter->setPen(QPen(m_colors.primary, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter->drawLine(QPointF(x - 4, y), QPointF(x - 1, y + 3));
            painter->drawLine(QPointF(x - 1, y + 3), QPointF(x + 5, y - 3));
        }
        painter->restore();
    }
private:
    QPointer<QComboBox> m_combo;
    Colors m_colors;
};

// Own the entire popup surface. No QComboBox private container or native menu
// primitive paints a second frame over this rounded panel.
class ComboPopup final : public QWidget {
public:
    explicit ComboPopup(QWidget* owner)
        : QWidget(owner, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint),
          m_owner(owner), m_colors(nullptr) {
        setObjectName("analysisComboPopup");
        setStyleSheet(QStringLiteral("QWidget#analysisComboPopup { background:transparent; border:none; }"));
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_NoMouseReplay);
        setAutoFillBackground(false);
    }
    void setColors(const Colors& colors) { m_colors = colors; update(); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(m_colors.border, 1)); painter.setBrush(m_colors.surface);
        painter.drawRoundedRect(QRectF(rect()).adjusted(.5, .5, -.5, -.5), 10, 10);
    }
    void hideEvent(QHideEvent* event) override {
        QWidget::hideEvent(event);
        if (m_owner) m_owner->update();
    }
private:
    QPointer<QWidget> m_owner;
    Colors m_colors;
};

// QComboBox retains item data and closed-state keyboard behavior. A QListView
// handles search/navigation in the owned popup; commits use the same signals.
class ComboBox final : public QComboBox {
public:
    explicit ComboBox(QWidget* parent, bool heading = false)
        : QComboBox(parent), m_heading(heading), m_colors(nullptr) {
        setStyleSheet(QStringLiteral(
            "QComboBox { background:transparent; border:none; padding:0; }"
            "QComboBox:hover { border:none; }"
            "QComboBox::drop-down { border:none; }"
            "QComboBox::down-arrow { image:none; border:none; }"));
        m_popup = new ComboPopup(this);
        auto* layout = new QVBoxLayout(m_popup);
        layout->setContentsMargins(7, 7, 7, 7); layout->setSpacing(0);
        m_list = new QListView(m_popup); m_list->setObjectName("analysisComboOptions");
        m_list->setMouseTracking(true); m_list->setSpacing(0);
        m_list->setFrameShape(QFrame::NoFrame);
        m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_list->setSelectionMode(QAbstractItemView::SingleSelection);
        m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_list->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        m_list->installEventFilter(this); layout->addWidget(m_list);
        m_delegate = new ComboOptionDelegate(this, m_list); m_list->setItemDelegate(m_delegate);
        connect(m_list, &QListView::clicked, this, [this](const QModelIndex& index) { commit(index); });
        setMaxVisibleItems(8); setFocusPolicy(Qt::StrongFocus); setCursor(Qt::PointingHandCursor);
        setSizeAdjustPolicy(QComboBox::AdjustToContents);
        setMinimumHeight(36); setThemeColors(m_colors);
    }
    void setThemeColors(const Colors& colors) {
        m_colors = colors; m_delegate->setColors(colors); m_popup->setColors(colors);
        auto f = font(); f.setPixelSize(m_heading ? 15 : 13);
        f.setWeight(m_heading ? QFont::DemiBold : QFont::Normal); setFont(f); m_list->setFont(f);
        auto palette = m_list->palette();
        palette.setColor(QPalette::Base, colors.surface); palette.setColor(QPalette::Window, colors.surface);
        palette.setColor(QPalette::Text, colors.text); m_list->setPalette(palette);
        m_list->setStyleSheet(QStringLiteral(
            "QListView#analysisComboOptions { color:%1; background:%2; border:none; outline:none; padding:0; }"
            "QScrollBar:vertical { width:5px; background:transparent; margin:4px 0; }"
            "QScrollBar::handle:vertical { background:%3; border-radius:2px; min-height:24px; }"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; }"
            "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background:transparent; }")
            .arg(colors.text.name(), colors.surface.name(), colors.border.name()));
        setMinimumWidth(sizeHint().width()); m_list->viewport()->update(); updateGeometry(); update();
    }
    QSize sizeHint() const override {
        const QFontMetrics listMetrics(m_list ? m_list->font() : font());
        int textWidth = fontMetrics().horizontalAdvance(placeholderText());
        for (int i = 0; i < count(); ++i) {
            textWidth = qMax(textWidth, fontMetrics().horizontalAdvance(itemText(i)));
            textWidth = qMax(textWidth, listMetrics.horizontalAdvance(itemText(i)));
        }
        // 46px item text/check spacing + 14px popup padding + 5px scrollbar
        // and a small font/DPI rounding allowance. Both widths use this budget.
        return QSize(qMax(m_heading ? 148 : 164, textWidth + 72), qMax(36, fontMetrics().height() + 16));
    }
    QSize minimumSizeHint() const override { return sizeHint(); }
    void showPopup() override {
        if (!isEnabled() || count() == 0) return;
        if (m_list->model() != model()) {
            m_list->setModel(model());
            connect(m_list->selectionModel(), &QItemSelectionModel::currentChanged, this,
                [this](const QModelIndex& index) {
                    if (!m_popup->isVisible() || !index.isValid()) return;
                    const int row = index.row(); const auto text = itemText(row);
                    QPointer<ComboBox> guard(this); emit highlighted(row);
                    if (guard) emit textHighlighted(text);
                });
        }
        m_list->setRootIndex(rootModelIndex()); m_list->setModelColumn(modelColumn());
        m_list->setAccessibleName(accessibleName());
        m_list->setCurrentIndex(model()->index(currentIndex(), modelColumn(), rootModelIndex()));
        const int rowHeight = qMax(38, m_list->fontMetrics().height() + 18);
        const int wantedHeight = qMin(count(), qMax(1, maxVisibleItems())) * rowHeight + 14;
        const auto top = mapToGlobal(QPoint(0, 0));
        const auto available = screen()->availableGeometry();
        const int below = qMax(0, available.bottom() - (top.y() + height() + 4) + 1);
        const int above = qMax(0, top.y() - 4 - available.top());
        const bool openAbove = below < wantedHeight && above > below;
        const int popupHeight = qMax(1, qMin(wantedHeight, openAbove ? above : below));
        const int x = qBound(available.left(), top.x(), qMax(available.left(), available.right() - width() + 1));
        const int y = openAbove ? top.y() - 4 - popupHeight : top.y() + height() + 4;
        m_popup->setFixedSize(width(), popupHeight); m_popup->move(x, y);
        m_popup->show(); m_list->setFocus(Qt::PopupFocusReason);
        m_list->scrollTo(m_list->currentIndex(), QAbstractItemView::EnsureVisible); update();
    }
    void hidePopup() override {
        m_popup->hide(); QComboBox::hidePopup(); update();
    }
protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == m_list && event->type() == QEvent::ShortcutOverride) {
            auto* key = static_cast<QKeyEvent*>(event);
            if (key->key() == Qt::Key_Escape || key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
                key->accept(); return true;
            }
        }
        if (watched == m_list && event->type() == QEvent::KeyPress) {
            auto* key = static_cast<QKeyEvent*>(event);
            if (key->key() == Qt::Key_Escape ||
                (key->key() == Qt::Key_Up && key->modifiers().testFlag(Qt::AltModifier))) {
                hidePopup(); setFocus(Qt::PopupFocusReason); return true;
            }
            if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter || key->key() == Qt::Key_Space) {
                commit(m_list->currentIndex()); return true;
            }
            if (key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) {
                hidePopup(); setFocus(Qt::TabFocusReason);
                focusNextPrevChild(key->key() == Qt::Key_Tab && !key->modifiers().testFlag(Qt::ShiftModifier));
                return true;
            }
        }
        return QComboBox::eventFilter(watched, event);
    }
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            if (m_popup->isVisible()) hidePopup(); else showPopup();
            event->accept(); return;
        }
        QComboBox::mousePressEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) { event->accept(); return; }
        QComboBox::mouseReleaseEvent(event);
    }
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
        const bool open = m_popup->isVisible();
        const bool active = isEnabled() && (underMouse() || hasFocus() || open);
        painter.setBrush(active ? m_colors.soft : m_colors.surface);
        painter.setPen(QPen(isEnabled() && (hasFocus() || open) ? m_colors.primary : m_colors.border, 1));
        painter.drawRoundedRect(QRectF(rect()).adjusted(.5, .5, -.5, -.5), 9, 9);
        painter.setFont(font()); painter.setPen(isEnabled() ? m_colors.text : m_colors.secondary);
        const QRect labelRect = rect().adjusted(12, 0, -34, 0);
        const auto label = currentIndex() < 0 ? placeholderText() : currentText();
        painter.drawText(labelRect, Qt::AlignLeft | Qt::AlignVCenter,
            fontMetrics().elidedText(label, Qt::ElideRight, qMax(0, labelRect.width())));
        const qreal x = width() - 18, y = height() / 2.;
        painter.setPen(QPen(active ? m_colors.primary : m_colors.secondary, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawLine(QPointF(x - 4, y + (open ? 2 : -2)), QPointF(x, y + (open ? -2 : 2)));
        painter.drawLine(QPointF(x, y + (open ? -2 : 2)), QPointF(x + 4, y + (open ? 2 : -2)));
    }
    void enterEvent(QEnterEvent* event) override { QComboBox::enterEvent(event); update(); }
    void leaveEvent(QEvent* event) override { QComboBox::leaveEvent(event); update(); }
    void focusInEvent(QFocusEvent* event) override { QComboBox::focusInEvent(event); update(); }
    void focusOutEvent(QFocusEvent* event) override { QComboBox::focusOutEvent(event); update(); }
    void resizeEvent(QResizeEvent* event) override {
        QComboBox::resizeEvent(event);
        if (m_popup && m_popup->isVisible()) hidePopup();
    }
    void hideEvent(QHideEvent* event) override {
        if (m_popup && m_popup->isVisible()) hidePopup();
        QComboBox::hideEvent(event);
    }
private:
    void commit(const QModelIndex& index) {
        if (!index.isValid() || !(index.flags() & Qt::ItemIsEnabled) || !(index.flags() & Qt::ItemIsSelectable)) return;
        const int row = index.row(); const auto text = itemText(row);
        hidePopup(); setFocus(Qt::PopupFocusReason);
        QPointer<ComboBox> guard(this); setCurrentIndex(row);
        if (!guard) return;
        emit activated(row);
        if (guard) emit textActivated(text);
    }
    bool m_heading;
    Colors m_colors;
    ComboPopup* m_popup = nullptr;
    QListView* m_list = nullptr;
    ComboOptionDelegate* m_delegate = nullptr;
};
}
