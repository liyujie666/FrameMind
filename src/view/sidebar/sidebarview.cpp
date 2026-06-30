#include "view/sidebar/sidebarview.h"

#include <QVBoxLayout>
#include <QToolButton>
#include <QButtonGroup>
#include <QLabel>
#include <QPainter>
#include <QIcon>

SidebarView::SidebarView(QWidget* parent)
    : QWidget(parent)
{
    setFixedWidth(64);
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, QColor("#F0F1F3"));  // Light.Sidebar
    setPalette(pal);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 12, 0, 12);
    layout->setSpacing(16);
    layout->setAlignment(Qt::AlignHCenter);

    // 顶部头像（圆形占位）
    auto* avatar = new QLabel(this);
    avatar->setFixedSize(40, 40);
    avatar->setPixmap(QIcon(QStringLiteral(":/icons/avatar.svg")).pixmap(40, 40));
    avatar->setStyleSheet(QStringLiteral(
        "border-radius:20px; background:#D0D4DA;"));
    avatar->setScaledContents(true);
    avatar->setToolTip(tr("用户"));
    layout->addWidget(avatar, 0, Qt::AlignHCenter);

    layout->addSpacing(8);

    // 中间功能图标按钮组（互斥可选中）
    m_group = new QButtonGroup(this);
    m_group->setExclusive(true);

    struct NavDef { const char* icon; const char* tip; };
    const NavDef navs[] = {
        { ":/icons/chat.svg",      "对话" },
        { ":/icons/files.svg",     "文件" },
        { ":/icons/knowledge.svg", "知识库" },
    };
    for (const auto& def : navs) {
        auto* btn = makeIconButton(QString::fromUtf8(def.icon),
                                   tr(def.tip));
        m_group->addButton(btn);
        m_navButtons.append(btn);
        layout->addWidget(btn, 0, Qt::AlignHCenter);
        connect(btn, &QToolButton::toggled, this, [this](bool) { update(); });
    }
    if (!m_navButtons.isEmpty()) {
        m_navButtons.first()->setChecked(true);
    }

    layout->addStretch(1);

    // 底部设置按钮
    m_settingsButton = makeIconButton(QStringLiteral(":/icons/settings.svg"),
                                      tr("设置"));
    m_settingsButton->setCheckable(false);
    layout->addWidget(m_settingsButton, 0, Qt::AlignHCenter);

    // 预留路由信号（M1 不接入）
    for (int i = 0; i < m_navButtons.size(); ++i) {
        connect(m_navButtons[i], &QToolButton::clicked, this,
                [this, i]() { emit pageRequested(i); });
    }
}

QToolButton* SidebarView::makeIconButton(const QString& iconPath, const QString& tip)
{
    auto* btn = new QToolButton(this);
    btn->setIcon(QIcon(iconPath));
    btn->setIconSize(QSize(24, 24));
    btn->setFixedSize(48, 48);
    btn->setCheckable(true);
    btn->setAutoRaise(true);
    btn->setToolTip(tip);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setStyleSheet(QStringLiteral(
        "QToolButton { border:none; border-radius:8px; }"
        "QToolButton:hover { background:#E2E5EA; }"
        "QToolButton:checked { background:#DCE6F7; }"));
    return btn;
}

void SidebarView::paintEvent(QPaintEvent* event)
{
    QWidget::paintEvent(event);

    // 激活态左侧 3px 蓝色指示条
    QToolButton* checked = nullptr;
    for (auto* btn : m_navButtons) {
        if (btn->isChecked()) { checked = btn; break; }
    }
    if (!checked) return;

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const int barH = 24;
    const int y = checked->y() + (checked->height() - barH) / 2;
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor("#1565C0"));  // Light.Primary
    painter.drawRoundedRect(QRectF(0, y, 3, barH), 1.5, 1.5);
}
