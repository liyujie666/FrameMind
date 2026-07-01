#include "view/common/settingsdialog.h"
#include "service/themeservice.h"
#include "service/settingsservice.h"
#include "service/agentservice.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QListWidget>
#include <QStackedWidget>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QStandardPaths>
#include <QCheckBox>
#include <QRadioButton>
#include <QButtonGroup>
#include <QScrollArea>
#include <QFileInfo>
#include <QApplication>

SettingsDialog::SettingsDialog(ThemeService* theme,
                               SettingsService* settings,
                               AgentService* agent,
                               QWidget* parent)
    : QDialog(parent)
    , m_theme(theme)
    , m_settings(settings)
    , m_agent(agent)
{
    setWindowTitle(tr("设置"));
    setMinimumSize(700, 480);
    setMaximumSize(900, 600);
    resize(720, 500);

    // 窗口无边框
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, false);

    // 默认暗色 fallback
    m_bgColor = QColor("#1E1E2E");
    m_navBgColor = QColor("#161622");
    m_navHoverColor = QColor("#252538");
    m_navActiveColor = QColor("#2979FF");
    m_textColor = QColor("#E0E0E0");
    m_textSecondaryColor = QColor("#8B8B8B");
    m_borderColor = QColor("#2D2D3D");

    auto* mainLayout = new QHBoxLayout(this);
    mainLayout->setContentsMargins(1, 1, 1, 1);
    mainLayout->setSpacing(0);

    // 外层圆角边框容器
    auto* outerFrame = new QFrame(this);
    outerFrame->setObjectName("outerFrame");
    mainLayout->addWidget(outerFrame);

    auto* frameLayout = new QHBoxLayout(outerFrame);
    frameLayout->setContentsMargins(0, 0, 0, 0);
    frameLayout->setSpacing(0);

    // 左侧导航
    m_navList = new QListWidget(outerFrame);
    m_navList->setFixedWidth(180);
    m_navList->setObjectName("navList");
    m_navList->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_navList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_navList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_navList->setSpacing(4);

    // 添加导航项
    struct NavItem { const char* name; const char* icon; };
    const NavItem items[] = {
        { "常规", ":/icons/setting_light.png" },
        { "主题", ":/icons/sun.png" },
        { "AI", ":/icons/chat.svg" },
    };

    for (const auto& item : items) {
        auto* itemWidget = new QWidget();
        itemWidget->setAttribute(Qt::WA_StyledBackground, true);
        itemWidget->setFixedHeight(44);

        auto* itemLayout = new QHBoxLayout(itemWidget);
        itemLayout->setContentsMargins(16, 0, 12, 0);
        itemLayout->setSpacing(12);

        auto* iconLabel = new QLabel(itemWidget);
        iconLabel->setFixedSize(20, 20);
        iconLabel->setPixmap(QIcon(QString::fromUtf8(item.icon)).pixmap(20, 20));
        iconLabel->setScaledContents(true);
        iconLabel->setObjectName("navIcon");
        itemLayout->addWidget(iconLabel);

        auto* nameLabel = new QLabel(tr(item.name), itemWidget);
        nameLabel->setObjectName("navLabel");
        nameLabel->setStyleSheet("background:transparent; border:none;");
        itemLayout->addWidget(nameLabel);

        itemLayout->addStretch();

        auto* listItem = new QListWidgetItem();
        listItem->setSizeHint(QSize(160, 44));
        listItem->setData(Qt::UserRole, QVariant::fromValue(reinterpret_cast<QObject*>(itemWidget)));
        m_navList->addItem(listItem);
        m_navList->setItemWidget(listItem, itemWidget);
    }

    m_navList->setCurrentRow(0);
    frameLayout->addWidget(m_navList);

    // 分隔线
    auto* separator = new QFrame(outerFrame);
    separator->setFrameShape(QFrame::VLine);
    separator->setObjectName("separator");
    separator->setFixedWidth(1);
    frameLayout->addWidget(separator);

    // 右侧内容区
    m_contentStack = new QStackedWidget(outerFrame);
    frameLayout->addWidget(m_contentStack, 1);

    // 构建各页面
    QWidget* generalPage = new QWidget(outerFrame);
    generalPage->setObjectName("generalPage");
    buildGeneralPage(generalPage);
    m_contentStack->addWidget(generalPage);

    QWidget* themePage = new QWidget(outerFrame);
    themePage->setObjectName("themePage");
    buildThemePage(themePage);
    m_contentStack->addWidget(themePage);

    QWidget* aiPage = new QWidget(outerFrame);
    aiPage->setObjectName("aiPage");
    buildAiPage(aiPage);
    m_contentStack->addWidget(aiPage);

    // 信号连接
    connect(m_navList, &QListWidget::currentRowChanged,
            this, &SettingsDialog::onNavItemClicked);

    // 连接主题服务
    if (m_theme) {
        connect(m_theme, &ThemeService::themeChanged,
                this, [this]() { applyThemeColors(); updateNavIcons(); });
        applyThemeColors();
        updateNavIcons();
    }
}

void SettingsDialog::resizeEvent(QResizeEvent* event)
{
    QDialog::resizeEvent(event);
}

void SettingsDialog::onNavItemClicked(int row)
{
    m_contentStack->setCurrentIndex(row);
}

void SettingsDialog::onOpenVideo()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    const QString path = QFileDialog::getOpenFileName(
        this, tr("打开视频"), dir,
        tr("视频文件 (*.mp4 *.mkv *.avi *.mov *.flv *.ts *.webm);;所有文件 (*.*)"));
    if (!path.isEmpty()) {
        emit openVideoRequested(path);
    }
}

void SettingsDialog::buildGeneralPage(QWidget* parent)
{
    auto* layout = new QVBoxLayout(parent);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(20);

    // 页面标题
    auto* titleLabel = new QLabel(tr("常规设置"), parent);
    titleLabel->setObjectName("pageTitle");
    layout->addWidget(titleLabel);

    // 打开视频按钮
    auto* openVideoBtn = new QPushButton(tr("打开视频..."), parent);
    openVideoBtn->setObjectName("primaryButton");
    openVideoBtn->setFixedHeight(40);
    openVideoBtn->setCursor(Qt::PointingHandCursor);
    connect(openVideoBtn, &QPushButton::clicked, this, &SettingsDialog::onOpenVideo);
    layout->addWidget(openVideoBtn);

    layout->addStretch(1);
}

void SettingsDialog::buildThemePage(QWidget* parent)
{
    auto* layout = new QVBoxLayout(parent);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(20);

    // 页面标题
    auto* titleLabel = new QLabel(tr("主题设置"), parent);
    titleLabel->setObjectName("pageTitle");
    layout->addWidget(titleLabel);

    // 主题模式选择
    auto* modeGroup = new QButtonGroup(parent);

    struct ModeItem { const char* text; int mode; };
    const ModeItem modes[] = {
        { "跟随系统", 0 },
        { "亮色模式", 1 },
        { "暗色模式", 2 },
    };

    for (const auto& mode : modes) {
        auto* radio = new QRadioButton(tr(mode.text), parent);
        radio->setObjectName("themeRadio");
        radio->setFixedHeight(36);
        modeGroup->addButton(radio, mode.mode);

        if (m_theme) {
            const int cur = static_cast<int>(m_theme->themeMode());
            radio->setChecked(cur == mode.mode);
        } else if (mode.mode == 2) {
            radio->setChecked(true);
        }

        connect(radio, &QRadioButton::clicked, this, [this, mode]() {
            if (m_theme) {
                m_theme->setThemeMode(static_cast<ThemeService::ThemeMode>(mode.mode));
            }
        });

        layout->addWidget(radio);
    }

    layout->addStretch(1);
}

void SettingsDialog::buildAiPage(QWidget* parent)
{
    auto* layout = new QVBoxLayout(parent);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(20);

    // 页面标题
    auto* titleLabel = new QLabel(tr("AI 设置"), parent);
    titleLabel->setObjectName("pageTitle");
    layout->addWidget(titleLabel);

    // 表单布局
    auto* form = new QFormLayout();
    form->setSpacing(16);
    form->setLabelAlignment(Qt::AlignLeft);

    // Endpoint
    auto* endpointEdit = new QLineEdit(parent);
    endpointEdit->setObjectName("inputField");
    endpointEdit->setFixedHeight(36);
    endpointEdit->setPlaceholderText(tr("https://api.openai.com/v1"));
    if (m_settings) {
        endpointEdit->setText(m_settings->get(QStringLiteral("llm.endpoint"),
                                             QStringLiteral("https://api.openai.com/v1")));
    }
    form->addRow(tr("Endpoint"), endpointEdit);

    // 模型
    auto* modelEdit = new QLineEdit(parent);
    modelEdit->setObjectName("inputField");
    modelEdit->setFixedHeight(36);
    modelEdit->setPlaceholderText(tr("gpt-4o"));
    if (m_settings) {
        modelEdit->setText(m_settings->get(QStringLiteral("llm.model"),
                                          QStringLiteral("gpt-4o")));
    }
    form->addRow(tr("模型"), modelEdit);

    // API Key
    auto* keyEdit = new QLineEdit(parent);
    keyEdit->setObjectName("inputField");
    keyEdit->setEchoMode(QLineEdit::Password);
    keyEdit->setFixedHeight(36);
    keyEdit->setPlaceholderText(tr("sk-...（仅存于系统密钥库）"));
    if (m_settings) {
        keyEdit->setText(m_settings->secretGet(QStringLiteral("secret.llm.api_key")));
    }
    form->addRow(tr("API Key"), keyEdit);

    layout->addLayout(form);

    // 保存按钮
    auto* saveBtn = new QPushButton(tr("保存"), parent);
    saveBtn->setObjectName("primaryButton");
    saveBtn->setFixedHeight(40);
    saveBtn->setCursor(Qt::PointingHandCursor);
    connect(saveBtn, &QPushButton::clicked, this, [this, endpointEdit, modelEdit, keyEdit]() {
        if (m_settings) {
            m_settings->set(QStringLiteral("llm.endpoint"), endpointEdit->text().trimmed());
            m_settings->set(QStringLiteral("llm.model"), modelEdit->text().trimmed());
            m_settings->secretSet(QStringLiteral("secret.llm.api_key"), keyEdit->text().trimmed());
        }
        if (m_agent) {
            m_agent->setEndpoint(endpointEdit->text().trimmed());
            m_agent->setModel(modelEdit->text().trimmed());
        }
        accept();
    });
    layout->addWidget(saveBtn);

    layout->addStretch(1);
}

void SettingsDialog::applyThemeColors()
{
    if (m_theme) {
        m_bgColor = m_theme->color(QStringLiteral("background"));
        m_navBgColor = m_theme->color(QStringLiteral("sidebar"));
        m_navHoverColor = m_theme->color(QStringLiteral("surfaceVariant"));
        m_textColor = m_theme->color(QStringLiteral("textPrimary"));
        m_textSecondaryColor = m_theme->color(QStringLiteral("textSecondary"));
        m_borderColor = m_theme->color(QStringLiteral("border"));
        m_navActiveColor = m_theme->color(QStringLiteral("primary"));
    }

    const QString bgHex = m_bgColor.name();
    const QString navBgHex = m_navBgColor.name();
    const QString navHoverHex = m_navHoverColor.name();
    const QString textHex = m_textColor.name();
    const QString textSecHex = m_textSecondaryColor.name();
    const QString borderHex = m_borderColor.name();
    const QString activeHex = m_navActiveColor.name();

    // 主样式
    setStyleSheet(QString(
        "QDialog { background:%1; }"
        "#outerFrame { background:%1; border:1px solid %2; border-radius:12px; }"
        "#navList { background:%3; border:none; border-radius:0px; outline:none; }"
        "#navList::item { background:transparent; border:none; border-radius:8px; margin:2px 4px; padding:0px; }"
        "#navList::item:selected { background:%4; }"
        "#navList::item:hover { background:%5; }"
        "#separator { background:%2; }"
        "#pageTitle { color:%6; font-size:18px; font-weight:600; background:transparent; border:none; }"
        "#primaryButton { background:%7; color:#FFFFFF; border:none; border-radius:8px; font-size:14px; font-weight:500; padding:0 24px; }"
        "#primaryButton:hover { background:%8; }"
        "#primaryButton:pressed { background:%9; }"
        "#inputField { background:%10; color:%6; border:1px solid %2; border-radius:8px; padding:0 12px; font-size:14px; selection-background-color:%7; }"
        "#inputField:focus { border-color:%7; }"
        "#inputField::placeholder { color:%11; }"
        "QLabel { color:%6; background:transparent; border:none; }"
        "QFormLayout { spacing:16px; }"
        "QFormLayout::label { color:%6; font-size:14px; min-width:80px; }"
        "#themeRadio { color:%6; background:transparent; border:none; font-size:14px; spacing:8px; padding:8px 12px; border-radius:8px; }"
        "#themeRadio:hover { background:%5; }"
        "#themeRadio::indicator { width:18px; height:18px; border:2px solid %2; border-radius:9px; background:transparent; }"
        "#themeRadio::indicator:checked { background:%7; border-color:%7; }"
        "QPushButton { background:%5; color:%6; border:1px solid %2; border-radius:8px; padding:8px 16px; font-size:14px; }"
        "QPushButton:hover { background:%4; }"
    ).arg(bgHex, borderHex, navBgHex, navHoverHex, navHoverHex,
          textHex, activeHex, "#4499FF", "#1177CC",
          m_theme ? m_theme->color("inputBg").name() : "#1A1A2A",
          textSecHex));

    // 更新列表项样式
    for (int i = 0; i < m_navList->count(); ++i) {
        auto* item = m_navList->item(i);
        auto* widget = m_navList->itemWidget(item);
        if (widget) {
            widget->setStyleSheet(QString(
                "QWidget { background:transparent; border:none; border-radius:8px; }"
                "#navIcon { background:transparent; border:none; }"
                "#navLabel { color:%1; background:transparent; border:none; font-size:14px; }"
            ).arg(textHex));
        }
    }
}

void SettingsDialog::updateNavIcons()
{
    if (!m_theme) return;

    const bool isDark = m_theme->isDark();

    // 更新导航图标
    const QStringList icons = {
        isDark ? ":/icons/setting_light.png" : ":/icons/setting_dark.png",
        isDark ? ":/icons/sun.png" : ":/icons/moon.png",
        ":/icons/chat.svg"
    };

    for (int i = 0; i < m_navList->count() && i < icons.size(); ++i) {
        auto* item = m_navList->item(i);
        auto* widget = m_navList->itemWidget(item);
        if (widget) {
            auto* iconLabel = widget->findChild<QLabel*>("navIcon");
            if (iconLabel) {
                iconLabel->setPixmap(QIcon(icons[i]).pixmap(20, 20));
            }
        }
    }
}
