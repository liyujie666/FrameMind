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

    // 存储图标路径（默认亮色主题，图标为深色）
    m_navIconPaths = {
        ":/icons/dialog_light.png",
        ":/icons/file_light.png",
        ":/icons/knowledge_light.png",
    };

    struct NavDef { const char* icon; const char* tip; };
    const NavDef navs[] = {
        { ":/icons/dialog_light.png",      "对话" },
        { ":/icons/file_light.png",     "文件" },
        { ":/icons/knowledge_light.png", "知识库" },
    };
    for (int i = 0; i < sizeof(navs)/sizeof(navs[0]); ++i) {
        auto* btn = makeIconButton(QString::fromUtf8(navs[i].icon), tr(navs[i].tip));
        m_group->addButton(btn);
        m_navButtons.append(btn);
        layout->addWidget(btn, 0, Qt::AlignHCenter);
        connect(btn, &QToolButton::toggled, this, [this](bool) { update(); });
    }
    if (!m_navButtons.isEmpty()) {
        m_navButtons.first()->setChecked(true);
        // 对话图标稍大
        m_navButtons.first()->setIconSize(QSize(28, 28));
    }

    layout->addStretch(1);

    // 底部设置按钮
    m_settingsButton = makeIconButton(QStringLiteral(":/icons/setting_light.png"),
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
                this, [this]() { applyThemeColors(); updateIcons(); update(); });
        applyThemeColors();
        updateIcons();
        update();
    }
}

void SidebarView::applyThemeColors()
{
    if (m_theme) {
        m_bgColor   = m_theme->color(QStringLiteral("sidebar"));
        m_indicator = m_theme->color(QStringLiteral("primary"));
        // 亮色主题用浅灰色背景，暗色主题用 surfaceVariant
        m_hoverBg = m_theme->isDark()
                     ? m_theme->color(QStringLiteral("surfaceVariant"))
                     : QColor("#E8E8E8");
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

void SidebarView::updateIcons()
{
    if (!m_theme) return;

    const bool isDark = m_theme->isDark();

    // 亮色主题用深色图标（与背景对比），暗色主题用浅色图标
    const QStringList lightBgIcons = {
        ":/icons/dialog_dark.png",
        ":/icons/file_dark.png",
        ":/icons/knowledge_dark.png",
    };
    const QStringList darkBgIcons = {
        ":/icons/dialog_light.png",
        ":/icons/file_light.png",
        ":/icons/knowledge_light.png",
    };
    const QStringList& icons = isDark ? darkBgIcons : lightBgIcons;

    for (int i = 0; i < m_navButtons.size() && i < icons.size(); ++i) {
        m_navButtons[i]->setIcon(QIcon(icons[i]));
        m_navIconPaths[i] = icons[i];
    }

    // 设置按钮图标
    const QString settingsIcon = isDark ? ":/icons/setting_light.png" : ":/icons/setting_dark.png";
    if (m_settingsButton) {
        m_settingsButton->setIcon(QIcon(settingsIcon));
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
