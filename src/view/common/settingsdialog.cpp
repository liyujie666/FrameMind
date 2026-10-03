#include "view/common/settingsdialog.h"
#include "service/themeservice.h"
#include "service/settingsservice.h"
#include "service/agentservice.h"
#include "service/llmproviderservice.h"
#include "model/llmprovider.h"
#include "model/video_presentation_types.h"
#include "infrastructure/generation_tokenizer.h"

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
#include <QComboBox>
#include <QSpinBox>
#include <QMessageBox>
#include <QCryptographicHash>
#include <QJsonDocument>

SettingsDialog::SettingsDialog(ThemeService* theme,
                               SettingsService* settings,
                               AgentService* agent,
                               LLMProviderService* providers,
                               QWidget* parent)
    : QDialog(parent)
    , m_theme(theme)
    , m_settings(settings)
    , m_agent(agent)
    , m_providers(providers)
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

    // 监听提供商变更以更新 UI
    if (m_providers) {
        connect(m_providers, &LLMProviderService::activeProviderChanged,
                this, &SettingsDialog::refreshProviderFields);
        connect(m_providers, &LLMProviderService::connectionTestResult,
                this, &SettingsDialog::onConnectionTestResult);
    }

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
    auto* mainLayout = new QVBoxLayout(parent);
    mainLayout->setContentsMargins(24, 24, 24, 24);
    mainLayout->setSpacing(20);

    // 页面标题
    auto* titleLabel = new QLabel(tr("AI 设置"), parent);
    titleLabel->setObjectName("pageTitle");
    mainLayout->addWidget(titleLabel);

    // 提供商选择
    auto* providerLayout = new QHBoxLayout();
    providerLayout->setSpacing(12);

    auto* providerLabel = new QLabel(tr("提供商"), parent);
    providerLabel->setObjectName("formLabel");
    providerLayout->addWidget(providerLabel);

    m_providerCombo = new QComboBox(parent);
    m_providerCombo->setObjectName("providerCombo");
    m_providerCombo->setFixedHeight(36);
    m_providerCombo->setMinimumWidth(200);

    // 填充提供商列表
    if (m_providers) {
        const QVector<LLMProvider> providers = m_providers->allProviders();
        for (const LLMProvider& p : providers) {
            m_providerCombo->addItem(p.name, p.id);
        }
        // 设置当前激活的提供商
        const QString activeId = m_providers->activeProviderId();
        const int idx = m_providerCombo->findData(activeId);
        if (idx >= 0) {
            m_providerCombo->setCurrentIndex(idx);
        }
    }

    connect(m_providerCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SettingsDialog::onProviderChanged);

    providerLayout->addWidget(m_providerCombo, 1);
    mainLayout->addLayout(providerLayout);

    // 配置表单
    auto* form = new QFormLayout();
    form->setSpacing(16);
    form->setLabelAlignment(Qt::AlignLeft);

    // Endpoint
    auto* endpointLabel = new QLabel(tr("API 端点"), parent);
    endpointLabel->setObjectName("formLabel");
    m_endpointEdit = new QLineEdit(parent);
    m_endpointEdit->setObjectName("inputField");
    m_endpointEdit->setFixedHeight(36);
    m_endpointEdit->setPlaceholderText(tr("https://api.openai.com/v1"));
    form->addRow(endpointLabel, m_endpointEdit);

    // 模型
    auto* modelLabel = new QLabel(tr("模型"), parent);
    modelLabel->setObjectName("formLabel");
    m_modelCombo = new QComboBox(parent);
    m_modelCombo->setObjectName("modelCombo");
    m_modelCombo->setFixedHeight(36);
    m_modelCombo->setEditable(true);
    m_modelCombo->setMinimumWidth(180);
    form->addRow(modelLabel, m_modelCombo);

    // API Key
    auto* keyLabel = new QLabel(tr("API Key"), parent);
    keyLabel->setObjectName("formLabel");
    m_apiKeyEdit = new QLineEdit(parent);
    m_apiKeyEdit->setObjectName("inputField");
    m_apiKeyEdit->setEchoMode(QLineEdit::Password);
    m_apiKeyEdit->setFixedHeight(36);
    m_apiKeyEdit->setPlaceholderText(tr("sk-...（仅存于系统密钥库）"));
    form->addRow(keyLabel, m_apiKeyEdit);

    mainLayout->addLayout(form);

    auto* budgetButton = new QPushButton(tr("视频分析输入预算…"), parent);
    budgetButton->setObjectName("secondaryButton");
    connect(budgetButton, &QPushButton::clicked, this, [this] {
        if (!m_settings || m_modelCombo->currentText().trimmed().isEmpty() || m_endpointEdit->text().trimmed().isEmpty()) return;
        const auto signature = QString::fromLatin1(QCryptographicHash::hash(
            (m_modelCombo->currentText().trimmed() + "\n" + m_endpointEdit->text().trimmed()).toUtf8(),
            QCryptographicHash::Sha256).toHex());
        const auto budgetKey = "video_rag.presentation_budget." + signature;
        const auto tokenizerKey = "video_rag.generation_tokenizer." + signature;
        VideoPresentationBudget budget;
        const auto saved = m_settings->get(budgetKey);
        if (!saved.isEmpty()) {
            QJsonParseError parse;
            const auto document = QJsonDocument::fromJson(saved.toUtf8(), &parse);
            if (parse.error != QJsonParseError::NoError || !document.isObject()) {
                QMessageBox::warning(this, tr("预算配置"), tr("当前模型的预算配置格式无效，请先修正配置。")); return;
            }
            budget = VideoPresentationBudget::fromJson(document.object());
            if (!budget.validationError().isEmpty()) {
                QMessageBox::warning(this, tr("预算配置"), budget.validationError()); return;
            }
        }
        QDialog dialog(this);
        dialog.setWindowTitle(tr("视频分析输入预算"));
        auto* layout = new QVBoxLayout(&dialog);
        auto* fields = new QFormLayout;
        auto* context = new QSpinBox(&dialog); context->setRange(0, 2097152); context->setValue(budget.modelContextTokens);
        context->setSpecialValueText(tr("未知，采用保守预算"));
        auto* output = new QSpinBox(&dialog); output->setRange(1, 131072); output->setValue(budget.reservedOutputTokens);
        auto* overhead = new QSpinBox(&dialog); overhead->setRange(0, 65536); overhead->setValue(budget.protocolOverheadTokens);
        overhead->setToolTip(tr("预留聊天模板、协议和图片输入占用；请按当前模型与服务的实际限制配置。"));
        fields->addRow(tr("模型上下文 tokens"), context);
        fields->addRow(tr("输出预留 tokens"), output);
        fields->addRow(tr("协议与图片预留 tokens"), overhead);
        auto* characterGuard = new QSpinBox(&dialog); characterGuard->setRange(256, 262144); characterGuard->setValue(budget.chapterWindowChars);
        characterGuard->setToolTip(tr("独立的字符保护上限；已知模型上下文并配置匹配分词文件时，实际容量仍按 token 预算计算。"));
        auto* overviewLength = new QSpinBox(&dialog); overviewLength->setRange(128, budget.maxOutputChars); overviewLength->setValue(budget.overviewOutputChars);
        auto* reductionLength = new QSpinBox(&dialog); reductionLength->setRange(128, budget.maxOutputChars); reductionLength->setValue(budget.reductionOutputChars);
        auto* reductionTokens = new QSpinBox(&dialog); reductionTokens->setRange(32, 131072); reductionTokens->setValue(budget.reductionOutputTokens);
        fields->addRow(tr("章节与总结请求字符保护上限"), characterGuard);
        auto* pageGuard = new QSpinBox(&dialog); pageGuard->setRange(256, 262144); pageGuard->setValue(budget.pageInputChars);
        auto* unitGuard = new QSpinBox(&dialog); unitGuard->setRange(256, 262144); unitGuard->setValue(budget.synthesisInputChars);
        fields->addRow(tr("页面请求字符保护上限"), pageGuard);
        fields->addRow(tr("单元综合字符保护上限"), unitGuard);
        fields->addRow(tr("全片概览输出字符上限"), overviewLength);
        fields->addRow(tr("每组中间综合输出字符上限"), reductionLength);
        fields->addRow(tr("每组中间综合输出 tokens"), reductionTokens);
        auto* path = new QLineEdit(&dialog);
        path->setText(QJsonDocument::fromJson(m_settings->get(tokenizerKey).toUtf8()).object()["path"].toString());
        path->setPlaceholderText(tr("当前生成模型的 tokenizer.json（可留空）"));
        auto* picker = new QPushButton(tr("选择…"), &dialog);
        connect(picker, &QPushButton::clicked, &dialog, [&dialog, path] {
            const auto file = QFileDialog::getOpenFileName(&dialog, QObject::tr("选择生成模型分词文件"), path->text(), "JSON (*.json)");
            if (!file.isEmpty()) path->setText(file);
        });
        auto* row = new QHBoxLayout; row->addWidget(path); row->addWidget(picker);
        fields->addRow(tr("生成模型分词文件"), row); layout->addLayout(fields);
        auto* description = new QLabel(tr("配置绑定当前模型和端点。支持 HF ByteLevel BPE；缺少匹配分词文件或已知上下文时使用保守预算。"), &dialog);
        description->setWordWrap(true); layout->addWidget(description);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, [&, path, context, output, overhead] {
            auto updated = budget;
            updated.modelContextTokens = context->value(); updated.reservedOutputTokens = output->value();
            updated.protocolOverheadTokens = overhead->value();
            updated.chapterWindowChars = characterGuard->value();
            updated.pageInputChars = pageGuard->value(); updated.synthesisInputChars = unitGuard->value();
            updated.overviewOutputChars = overviewLength->value();
            updated.reductionOutputChars = reductionLength->value();
            updated.reductionOutputTokens = reductionTokens->value();
            const auto error = updated.validationError();
            if (!error.isEmpty()) { QMessageBox::warning(&dialog, tr("预算配置"), error); return; }
            QJsonObject tokenizerConfig;
            if (!path->text().trimmed().isEmpty()) {
                GenerationTokenizer tokenizer;
                if (!tokenizer.load(path->text().trimmed())) {
                    QMessageBox::warning(&dialog, tr("分词文件不可用"), tokenizer.error()); return;
                }
                tokenizerConfig = {{"model_signature", signature}, {"path", QFileInfo(path->text().trimmed()).absoluteFilePath()},
                    {"sha256", tokenizer.fingerprint().section(':', -1)}};
            }
            m_settings->set(budgetKey, QString::fromUtf8(QJsonDocument(updated.toJson()).toJson(QJsonDocument::Compact)));
            m_settings->set(tokenizerKey, tokenizerConfig.isEmpty() ? QString{} : QString::fromUtf8(QJsonDocument(tokenizerConfig).toJson(QJsonDocument::Compact)));
            dialog.accept();
        });
        dialog.exec();
    });
    mainLayout->addWidget(budgetButton);

    // 状态提示
    m_providerStatusLabel = new QLabel(parent);
    m_providerStatusLabel->setObjectName("statusLabel");
    mainLayout->addWidget(m_providerStatusLabel);

    // 测试连接按钮
    auto* testBtnLayout = new QHBoxLayout();
    testBtnLayout->addStretch();

    m_testConnectionBtn = new QPushButton(tr("测试连接"), parent);
    m_testConnectionBtn->setObjectName("secondaryButton");
    m_testConnectionBtn->setFixedHeight(36);
    m_testConnectionBtn->setFixedWidth(100);
    m_testConnectionBtn->setCursor(Qt::PointingHandCursor);
    connect(m_testConnectionBtn, &QPushButton::clicked, this, &SettingsDialog::onTestConnection);
    testBtnLayout->addWidget(m_testConnectionBtn);

    mainLayout->addLayout(testBtnLayout);

    mainLayout->addStretch(1);

    // 保存按钮
    auto* buttonLayout = new QHBoxLayout();
    buttonLayout->addStretch();

    auto* saveBtn = new QPushButton(tr("保存"), parent);
    saveBtn->setObjectName("primaryButton");
    saveBtn->setFixedHeight(40);
    saveBtn->setFixedWidth(120);
    saveBtn->setCursor(Qt::PointingHandCursor);
    connect(saveBtn, &QPushButton::clicked, this, &SettingsDialog::onSaveAiSettings);
    buttonLayout->addWidget(saveBtn);

    auto* cancelBtn = new QPushButton(tr("取消"), parent);
    cancelBtn->setObjectName("secondaryButton");
    cancelBtn->setFixedHeight(40);
    cancelBtn->setFixedWidth(100);
    cancelBtn->setCursor(Qt::PointingHandCursor);
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    buttonLayout->addWidget(cancelBtn);

    mainLayout->addLayout(buttonLayout);

    // 初始加载当前配置
    refreshProviderFields();
}

void SettingsDialog::onProviderChanged(int index)
{
    if (index < 0 || !m_providers) return;

    const QString providerId = m_providerCombo->itemData(index).toString();
    const LLMProvider provider = m_providers->providerById(providerId);
    if (provider.id.isEmpty()) return;

    // 更新端点和模型列表
    m_endpointEdit->setText(provider.endpoint);

    m_modelCombo->clear();
    if (provider.models.isEmpty()) {
        m_modelCombo->addItem(provider.defaultModel);
    } else {
        for (const QString& model : provider.models) {
            m_modelCombo->addItem(model);
        }
        // 设置默认模型
        const int defaultIdx = m_modelCombo->findText(provider.defaultModel);
        if (defaultIdx >= 0) {
            m_modelCombo->setCurrentIndex(defaultIdx);
        }
    }

    // 清除 API Key 输入（用户需要重新输入）
    m_apiKeyEdit->clear();

    // 更新状态提示
    updateProviderStatus(provider);
}

void SettingsDialog::onSaveAiSettings()
{
    if (!m_providers || !m_settings) return;

    const int idx = m_providerCombo->currentIndex();
    if (idx < 0) return;

    const QString providerId = m_providerCombo->itemData(idx).toString();

    // 保存提供商选择
    m_providers->setActiveProvider(providerId);

    // 保存端点
    const QString endpoint = m_endpointEdit->text().trimmed();
    if (!endpoint.isEmpty()) {
        m_providers->setEndpoint(providerId, endpoint);
    }

    // 保存模型
    const QString model = m_modelCombo->currentText().trimmed();
    if (!model.isEmpty()) {
        m_providers->setModel(providerId, model);
    }

    // 保存 API Key（跳过占位符，避免覆盖已保存的真实 key）
    const QString apiKey = m_apiKeyEdit->text().trimmed();
    if (!apiKey.isEmpty() && apiKey != QStringLiteral("********")) {
        m_providers->setApiKey(providerId, apiKey);
    }

    // 更新 AgentService 配置
    if (m_agent) {
        const LLMProvider provider = m_providers->activeProvider();
        m_agent->setEndpoint(m_providers->getEndpoint(provider.id));
        m_agent->setModel(m_providers->getModel(provider.id));
    }

    accept();
}

void SettingsDialog::refreshProviderFields()
{
    if (!m_providers) return;

    const LLMProvider provider = m_providers->activeProvider();
    if (provider.id.isEmpty()) return;

    // 更新提供商下拉框
    const int idx = m_providerCombo->findData(provider.id);
    if (idx >= 0 && idx != m_providerCombo->currentIndex()) {
        m_providerCombo->blockSignals(true);
        m_providerCombo->setCurrentIndex(idx);
        m_providerCombo->blockSignals(false);
    }

    // 更新端点
    const QString endpoint = m_providers->getEndpoint(provider.id);
    m_endpointEdit->setText(endpoint);

    // 更新模型列表
    m_modelCombo->clear();
    if (!provider.models.isEmpty()) {
        m_modelCombo->addItems(provider.models);
    } else {
        m_modelCombo->addItem(provider.defaultModel);
    }

    const QString currentModel = m_providers->getModel(provider.id);
    if (!currentModel.isEmpty()) {
        const int modelIdx = m_modelCombo->findText(currentModel);
        if (modelIdx >= 0) {
            m_modelCombo->setCurrentIndex(modelIdx);
        } else {
            // 如果当前模型不在列表中，添加到列表并选中
            m_modelCombo->addItem(currentModel);
            m_modelCombo->setCurrentIndex(m_modelCombo->count() - 1);
        }
    }

    // 检查 API Key 是否已配置
    const QString apiKey = m_providers->getApiKey(provider.id);
    if (!apiKey.isEmpty()) {
        m_apiKeyEdit->setText(QStringLiteral("********"));  // 不显示实际 key
        m_apiKeyEdit->setProperty("hasValue", true);
    } else {
        m_apiKeyEdit->clear();
        m_apiKeyEdit->setProperty("hasValue", false);
    }

    updateProviderStatus(provider);
}

void SettingsDialog::updateProviderStatus(const LLMProvider& provider)
{
    if (!m_providers) return;

    const QString apiKey = m_providers->getApiKey(provider.id);
    const bool hasKey = !apiKey.isEmpty();

    QString status;
    if (hasKey) {
        status = QStringLiteral("✓ %1 已配置").arg(provider.name);
        m_providerStatusLabel->setStyleSheet("color: #4CAF50;");
    } else {
        status = QStringLiteral("⚠ 请填写 %1 的 API Key").arg(provider.name);
        m_providerStatusLabel->setStyleSheet("color: #FF9800;");
    }

    if (provider.supportsVision) {
        status = status + QStringLiteral(" · 支持视觉");
    } else {
        status = status + QStringLiteral(" · 不支持视觉");
    }

    m_providerStatusLabel->setText(status);
}

void SettingsDialog::onTestConnection()
{
    if (!m_providers || !m_testConnectionBtn) return;

    const int idx = m_providerCombo->currentIndex();
    if (idx < 0) return;

    const QString providerId = m_providerCombo->itemData(idx).toString();
    const QString apiKey = m_apiKeyEdit->text().trimmed();

    // 如果 API Key 输入框显示的是占位符，使用已保存的 key
    QString keyToTest = apiKey;
    if (apiKey == QStringLiteral("********") || apiKey.isEmpty()) {
        keyToTest = m_providers->getApiKey(providerId);
    }

    if (keyToTest.isEmpty()) {
        m_providerStatusLabel->setText(QStringLiteral("⚠ 请先填写 API Key"));
        m_providerStatusLabel->setStyleSheet("color: #FF9800;");
        return;
    }

    // 更新 UI 状态
    m_connectionTesting = true;
    m_testConnectionBtn->setEnabled(false);
    m_testConnectionBtn->setText(QStringLiteral("测试中..."));
    m_providerStatusLabel->setText(QStringLiteral("正在测试连接..."));
    m_providerStatusLabel->setStyleSheet("color: #2196F3;");

    // 直接传入 key 执行测试，不提前写入持久化，避免触发 providerUpdated 信号
    m_providers->testProviderConnection(providerId, keyToTest);
}

void SettingsDialog::onConnectionTestResult(const QString& providerId, bool success, const QString& message)
{
    // 恢复按钮状态
    m_connectionTesting = false;
    if (m_testConnectionBtn) {
        m_testConnectionBtn->setEnabled(true);
        m_testConnectionBtn->setText(QStringLiteral("测试连接"));
    }

    // 更新状态显示
    if (success) {
        m_providerStatusLabel->setText(QStringLiteral("✓ 连接成功"));
        m_providerStatusLabel->setStyleSheet("color: #4CAF50;");
    } else {
        m_providerStatusLabel->setText(QStringLiteral("✗ 连接失败: %1").arg(message));
        m_providerStatusLabel->setStyleSheet("color: #F44336;");
    }
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
        "#secondaryButton { background:%10; color:%6; border:1px solid %2; border-radius:8px; font-size:14px; padding:0 16px; }"
        "#secondaryButton:hover { background:%5; }"
        "#inputField { background:%11; color:%6; border:1px solid %2; border-radius:8px; padding:0 12px; font-size:14px; selection-background-color:%7; }"
        "#inputField:focus { border-color:%7; }"
        "#inputField::placeholder { color:%12; }"
        "#providerCombo, #modelCombo { background:%11; color:%6; border:1px solid %2; border-radius:8px; padding:0 12px; font-size:14px; selection-background-color:%7; }"
        "#providerCombo:focus, #modelCombo:focus { border-color:%7; }"
        "#providerCombo::drop-down, #modelCombo::drop-down { border:none; padding-right:8px; }"
        "#providerCombo::down-arrow, #modelCombo::down-arrow { width:10px; height:10px; border:none; image:none; }"
        "QComboBox QAbstractItemView { background:%11; color:%6; border:1px solid %2; selection-background-color:%7; }"
        "#formLabel { color:%6; font-size:14px; min-width:80px; padding:8px 0; }"
        "#statusLabel { color:%6; font-size:12px; background:transparent; border:none; padding:4px 0; }"
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
          m_theme ? m_theme->color("surfaceVariant").name() : "#2D2D3D",
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
