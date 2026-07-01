#include "view/sidebar/sidebarview.h"
#include "service/themeservice.h"

#include <QVBoxLayout>
#include <QToolButton>
#include <QButtonGroup>
#include <QLabel>
#include <QPainter>
#include <QIcon>
#include <QApplication>

SidebarView::SidebarView(QWidget* parent)
    : QWidget(parent)
{
    setFixedWidth(64);
    setAutoFillBackground(true);

    // 默认暗色 fallback
    m_bgColor   = QColor("#161622");
    m_hoverBg   = QColor("#252538");
    m_indicator = QColor("#2979FF");

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 12, 0, 12);
    layout->setSpacing(16);
    layout->setAlignment(Qt::AlignHCenter);

    // 顶部头像
    auto* avatar = new QLabel(this);
    avatar->setFixedSize(40, 40);
    avatar->setPixmap(QIcon(QStringLiteral(":/icons/avatar.svg")).pixmap(40, 40));
    avatar->setScaledContents(true);
    avatar->setToolTip(tr("用户"));
    avatar->setCursor(Qt::PointingHandCursor);
    avatar->setStyleSheet(QStringLiteral("background:transparent;"));
    layout->addWidget(avatar, 0, Qt::AlignHCenter);

    layout->addSpacing(8);

    // 中间功能图标按钮组
    m_group = new QButtonGroup(this);
    m_group->setExclusive(true);

    struct NavDef { const char* icon; const char* tip; };
    const NavDef navs[] = {
        { ":/icons/chat.svg",      "对话" },
        { ":/icons/files.svg",     "文件" },
        { ":/icons/knowledge.svg", "知识库" },
    };
    for (const auto& def : navs) {
        auto* btn = makeIconButton(QString::fromUtf8(def.icon), tr(def.tip));
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
    connect(m_settingsButton, &QToolButton::clicked,
            this, &SidebarView::settingsClicked);

    // 页面路由信号
    for (int i = 0; i < m_navButtons.size(); ++i) {
        connect(m_navButtons[i], &QToolButton::clicked, this,
                [this, i]() { emit pageRequested(i); });
    }

    applyThemeColors();
}

void SidebarView::setThemeService(ThemeService* theme)
{
    if (m_theme == theme) return;
    if (m_theme) disconnect(m_theme, nullptr, this, nullptr);
    m_theme = theme;
    if (m_theme) {
        connect(m_theme, &ThemeService::themeChanged,
                this, [this]() { applyThemeColors(); update(); });
        applyThemeColors();
        update();
    }
}

void SidebarView::applyThemeColors()
{
    if (m_theme) {
        m_bgColor   = m_theme->color(QStringLiteral("sidebar"));
        m_hoverBg   = m_theme->color(QStringLiteral("surfaceVariant"));
        m_indicator = m_theme->color(QStringLiteral("primary"));
    }

    QPalette pal = palette();
    pal.setColor(QPalette::Window, m_bgColor);
    setPalette(pal);

    const QString qss = QString(
        "QToolButton { border:none; border-radius:8px; background:transparent; }"
        "QToolButton:hover { background:%1; }"
        "QToolButton:checked { background:%1; }")
        .arg(m_hoverBg.name());
    for (auto* btn : m_navButtons) btn->setStyleSheet(qss);
    if (m_settingsButton) m_settingsButton->setStyleSheet(qss);
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
    return btn;
}

void SidebarView::paintEvent(QPaintEvent* event)
{
    QWidget::paintEvent(event);

    // 激活态左侧 3px 主题色指示条
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
    painter.setBrush(m_indicator);
    painter.drawRoundedRect(QRectF(0, y, 3, barH), 1.5, 1.5);
}
