#include "view/common/customtitlebar.h"
#include "service/themeservice.h"

#include <QHBoxLayout>
#include <QToolButton>
#include <QLabel>
#include <QMouseEvent>
#include <QIcon>
#include <QPainter>
#include <QApplication>
#include <QSvgRenderer>
#include <QFile>

CustomTitleBar::CustomTitleBar(ThemeService* theme, QWidget* parent)
    : QWidget(parent)
    , m_theme(theme)
{
    setFixedHeight(40);
    setAutoFillBackground(false);
    setAttribute(Qt::WA_StyledBackground, false);
    setCursor(Qt::SizeAllCursor);

    // 获取父窗口用于窗口控制
    m_parentWindow = parent;

    // 默认暗色 fallback
    m_bgColor       = QColor("#161622");
    m_iconColor     = QColor("#8B8B8B");
    m_iconHoverColor = QColor("#FFFFFF");
    m_closeHoverColor = QColor("#E81123");

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 0, 8, 0);
    layout->setSpacing(16);

    // 左侧 Logo
    m_logoLabel = new QLabel(this);
    m_logoLabel->setFixedSize(120, 32);
    m_logoLabel->setStyleSheet("background:transparent;");
    layout->addWidget(m_logoLabel);
    layout->addStretch(1);

    // 主题切换按钮
    m_themeToggleBtn = new QToolButton(this);
    m_themeToggleBtn->setFixedSize(44, 40);
    m_themeToggleBtn->setIconSize(QSize(32, 32));
    m_themeToggleBtn->setCursor(Qt::PointingHandCursor);
    m_themeToggleBtn->setStyleSheet(
        "QToolButton { border:none; border-radius:6px; background:transparent; padding:4px; }"
        "QToolButton:hover { background:rgba(255,255,255,10%); }");
    connect(m_themeToggleBtn, &QToolButton::clicked, this, &CustomTitleBar::onThemeToggle);
    layout->addWidget(m_themeToggleBtn);

    // 分隔线
    auto* separator = new QWidget(this);
    separator->setFixedSize(1, 20);
    separator->setStyleSheet("background:rgba(255,255,255,10%); border-radius:0.5px;");
    layout->addWidget(separator);

    // 最小化按钮
    m_minimizeBtn = new QToolButton(this);
    m_minimizeBtn->setFixedSize(36, 32);
    m_minimizeBtn->setIconSize(QSize(14, 14));
    m_minimizeBtn->setCursor(Qt::PointingHandCursor);
    m_minimizeBtn->setStyleSheet(
        "QToolButton { border:none; border-radius:0px; background:transparent; }"
        "QToolButton:hover { background:rgba(255,255,255,15%); }");
    connect(m_minimizeBtn, &QToolButton::clicked, this, &CustomTitleBar::onMinimize);
    layout->addWidget(m_minimizeBtn);

    // 最大化按钮
    m_maximizeBtn = new QToolButton(this);
    m_maximizeBtn->setFixedSize(36, 32);
    m_maximizeBtn->setIconSize(QSize(14, 14));
    m_maximizeBtn->setCursor(Qt::PointingHandCursor);
    m_maximizeBtn->setStyleSheet(
        "QToolButton { border:none; border-radius:0px; background:transparent; }"
        "QToolButton:hover { background:rgba(255,255,255,15%); }");
    connect(m_maximizeBtn, &QToolButton::clicked, this, &CustomTitleBar::onMaximize);
    layout->addWidget(m_maximizeBtn);

    // 关闭按钮
    m_closeBtn = new QToolButton(this);
    m_closeBtn->setFixedSize(46, 32);
    m_closeBtn->setIconSize(QSize(14, 14));
    m_closeBtn->setCursor(Qt::PointingHandCursor);
    m_closeBtn->setStyleSheet(
        "QToolButton { border:none; border-radius:0px; background:transparent; }"
        "QToolButton:hover { background:#E81123; }");
    connect(m_closeBtn, &QToolButton::clicked, this, &CustomTitleBar::onClose);
    layout->addWidget(m_closeBtn);

    // 连接主题服务
    if (m_theme) {
        connect(m_theme, &ThemeService::themeChanged,
                this, [this]() { applyThemeColors(); updateWindowButtonStates(); updateThemeToggleIcon(); updateLogo(); });
        applyThemeColors();
        updateWindowButtonStates();
        updateThemeToggleIcon();
        updateLogo();
    }
}

void CustomTitleBar::setThemeService(ThemeService* theme)
{
    if (m_theme == theme) return;
    if (m_theme) disconnect(m_theme, nullptr, this, nullptr);
    m_theme = theme;
    if (m_theme) {
        connect(m_theme, &ThemeService::themeChanged,
                this, [this]() { applyThemeColors(); updateWindowButtonStates(); updateThemeToggleIcon(); updateLogo(); });
        applyThemeColors();
        updateWindowButtonStates();
        updateThemeToggleIcon();
        updateLogo();
    }
}

void CustomTitleBar::applyThemeColors()
{
    if (m_theme) {
        m_bgColor = m_theme->color(QStringLiteral("sidebar"));
        // 亮色主题用深色图标，暗色主题用浅色图标
        m_iconColor = m_theme->isDark() ? QColor("#A0A0A0") : QColor("#6B6B6B");
        m_iconHoverColor = m_theme->isDark() ? QColor("#FFFFFF") : QColor("#1A1A1A");
    }

    // 更新背景
    QPalette pal = palette();
    pal.setColor(QPalette::Window, m_bgColor);
    setPalette(pal);

    // 更新整体样式
    const QString bgHex = m_bgColor.name();
    const QString iconHex = m_iconColor.name();
    const QString hoverHex = m_iconHoverColor.name();

    setStyleSheet(QString(
        "QWidget { background:%1; }"
        "QLabel { color:%2; }"
    ).arg(bgHex, iconHex));

    update();
}

void CustomTitleBar::updateWindowButtonStates()
{
    if (!m_theme) return;

    const bool isDark = m_theme->isDark();

    // 更新最小化图标
    QIcon minIcon;
    if (isDark) {
        minIcon = QIcon(":/icons/minimize_light.png");
    } else {
        minIcon = QIcon(":/icons/minimize_dark.png");
    }
    m_minimizeBtn->setIcon(minIcon);

    // 更新最大化图标
    QIcon maxIcon;
    if (isDark) {
        maxIcon = QIcon(":/icons/maxsize_light.png");
    } else {
        maxIcon = QIcon(":/icons/maxsize_dark.png");
    }
    m_maximizeBtn->setIcon(maxIcon);

    // 更新关闭图标
    QIcon closeIcon;
    if (isDark) {
        closeIcon = QIcon(":/icons/close_light.png");
    } else {
        closeIcon = QIcon(":/icons/close_dark.png");
    }
    m_closeBtn->setIcon(closeIcon);
}

void CustomTitleBar::updateThemeToggleIcon()
{
    if (!m_theme) return;

    const bool isDark = m_theme->isDark();
    const QString iconPath = isDark ? ":/icons/sun.png" : ":/icons/moon.png";

    // 根据图标类型设置不同的大小
    const QSize iconSize = isDark ? QSize(46, 46) : QSize(22, 22);
    QPixmap pixmap(iconSize);
    pixmap.load(iconPath);
    if (!pixmap.isNull()) {
        m_themeToggleBtn->setIcon(QIcon(pixmap.scaled(iconSize, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    }
    m_themeToggleBtn->setToolTip(isDark ? tr("切换到亮色模式") : tr("切换到暗色模式"));
}

void CustomTitleBar::updateLogo()
{
    if (!m_logoLabel) return;

    const QString svgPath = m_theme && m_theme->isDark()
        ? QStringLiteral(":/icons/logo_word_dark.svg")
        : QStringLiteral(":/icons/logo_word_light.svg");

    QSvgRenderer renderer(svgPath);
    if (renderer.isValid()) {
        QPixmap pix(120, 32);
        pix.fill(Qt::transparent);
        QPainter painter(&pix);
        renderer.render(&painter);
        m_logoLabel->setPixmap(pix);
    }
}

void CustomTitleBar::onThemeToggle()
{
    if (!m_theme) return;

    // 切换主题：暗→亮，亮→暗
    ThemeService::ThemeMode newMode = m_theme->isDark()
        ? ThemeService::ThemeMode::Light
        : ThemeService::ThemeMode::Dark;

    m_theme->setThemeMode(newMode);
}

void CustomTitleBar::onMinimize()
{
    if (m_parentWindow) {
        m_parentWindow->showMinimized();
    }
}

void CustomTitleBar::onMaximize()
{
    if (!m_parentWindow) return;

    if (m_parentWindow->isMaximized()) {
        m_parentWindow->showNormal();
        m_isMaximized = false;
    } else {
        m_parentWindow->showMaximized();
        m_isMaximized = true;
    }
}

void CustomTitleBar::onClose()
{
    if (m_parentWindow) {
        m_parentWindow->close();
    }
}

void CustomTitleBar::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragging = true;
        m_dragStartPos = event->globalPosition().toPoint();
        if (m_parentWindow) {
            m_windowStartPos = m_parentWindow->pos();
        }
        event->accept();
    }
}

void CustomTitleBar::mouseMoveEvent(QMouseEvent* event)
{
    if (m_dragging && (event->buttons() & Qt::LeftButton)) {
        const QPoint delta = event->globalPosition().toPoint() - m_dragStartPos;
        if (m_parentWindow) {
            m_parentWindow->move(m_windowStartPos + delta);
        }
        event->accept();
    }
}

void CustomTitleBar::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragging = false;
        event->accept();
    }
}

void CustomTitleBar::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        onMaximize();
        event->accept();
    }
}
