#include "service/llmproviderservice.h"

#include "service/settingsservice.h"
#include "infrastructure/networkclient.h"

#include <QJsonDocument>
#include <QJsonArray>

LLMProviderService::LLMProviderService(SettingsService* settings, QObject* parent)
    : QObject(parent)
    , m_settings(settings)
{
    loadProviders();
    loadActiveProvider();
}

LLMProviderService::~LLMProviderService() = default;

void LLMProviderService::setNetworkClient(NetworkClient* network)
{
    m_network = network;
}

QString LLMProviderService::providerConfigKey()
{
    return QStringLiteral("llm.providers");
}

QString LLMProviderService::activeProviderKey()
{
    return QStringLiteral("llm.active_provider");
}

QVector<LLMProvider> LLMProviderService::allProviders() const
{
    QVector<LLMProvider> result;
    // 添加所有预设提供商
    for (const LLMProvider& p : LLMProviderPresets::allPresets()) {
        result.append(p);
    }
    // 添加自定义提供商
    result.append(m_customProviders);
    return result;
}

QVector<LLMProvider> LLMProviderService::presetProviders() const
{
    return LLMProviderPresets::allPresets();
}

QVector<LLMProvider> LLMProviderService::customProviders() const
{
    return m_customProviders;
}

LLMProvider LLMProviderService::providerById(const QString& id) const
{
    // 先查找预设提供商
    for (const LLMProvider& p : LLMProviderPresets::allPresets()) {
        if (p.id == id) {
            return p;
        }
    }
    // 再查找自定义提供商
    for (const LLMProvider& p : m_customProviders) {
        if (p.id == id) {
            return p;
        }
    }
    return LLMProvider();
}

bool LLMProviderService::hasProvider(const QString& id) const
{
    return !providerById(id).id.isEmpty();
}

LLMProvider LLMProviderService::activeProvider() const
{
    if (m_activeProviderId.isEmpty()) {
        // 默认返回 OpenAI
        return LLMProviderPresets::openAI();
    }
    return providerById(m_activeProviderId);
}

QString LLMProviderService::activeProviderId() const
{
    if (m_activeProviderId.isEmpty()) {
        return QStringLiteral("openai");
    }
    return m_activeProviderId;
}

void LLMProviderService::setActiveProvider(const QString& providerId)
{
    if (!hasProvider(providerId)) {
        return;
    }
    if (m_activeProviderId == providerId) {
        return;
    }
    m_activeProviderId = providerId;
    if (m_settings) {
        m_settings->set(activeProviderKey(), providerId);
    }
    emit activeProviderChanged(providerId);
}

QString LLMProviderService::getApiKey(const QString& providerId) const
{
    if (!m_settings) {
        return QString();
    }
    const LLMProvider p = providerById(providerId);
    if (p.apiKeyName.isEmpty()) {
        return QString();
    }
    return m_settings->secretGet(p.apiKeyName);
}

bool LLMProviderService::setApiKey(const QString& providerId, const QString& apiKey)
{
    if (!m_settings) {
        return false;
    }
    const LLMProvider p = providerById(providerId);
    if (p.apiKeyName.isEmpty()) {
        return false;
    }
    const bool success = m_settings->secretSet(p.apiKeyName, apiKey);
    if (success) {
        emit providerUpdated(providerId);
    }
    return success;
}

bool LLMProviderService::deleteApiKey(const QString& providerId)
{
    if (!m_settings) {
        return false;
    }
    const LLMProvider p = providerById(providerId);
    if (p.apiKeyName.isEmpty()) {
        return false;
    }
    const bool success = m_settings->secretDelete(p.apiKeyName);
    if (success) {
        emit providerUpdated(providerId);
    }
    return success;
}

QString LLMProviderService::getEndpoint(const QString& providerId) const
{
    if (!m_settings) {
        return QString();
    }
    // 先检查是否有自定义端点配置
    const QString customEndpoint = m_settings->get(
        QStringLiteral("llm.provider.%1.endpoint").arg(providerId));
    if (!customEndpoint.isEmpty()) {
        return customEndpoint;
    }
    // 返回默认端点
    return providerById(providerId).endpoint;
}

void LLMProviderService::setEndpoint(const QString& providerId, const QString& endpoint)
{
    if (!m_settings) {
        return;
    }
    m_settings->set(QStringLiteral("llm.provider.%1.endpoint").arg(providerId), endpoint);
    emit providerUpdated(providerId);
}

QString LLMProviderService::getModel(const QString& providerId) const
{
    if (!m_settings) {
        return QString();
    }
    const QString model = m_settings->get(
        QStringLiteral("llm.provider.%1.model").arg(providerId));
    if (!model.isEmpty()) {
        return model;
    }
    return providerById(providerId).defaultModel;
}

void LLMProviderService::setModel(const QString& providerId, const QString& model)
{
    if (!m_settings) {
        return;
    }
    m_settings->set(QStringLiteral("llm.provider.%1.model").arg(providerId), model);
    emit providerUpdated(providerId);
}

bool LLMProviderService::addCustomProvider(const LLMProvider& provider)
{
    if (provider.id.isEmpty() || hasProvider(provider.id)) {
        return false;
    }
    m_customProviders.append(provider);
    saveProviders();
    emit providersChanged();
    return true;
}

bool LLMProviderService::updateCustomProvider(const LLMProvider& provider)
{
    for (int i = 0; i < m_customProviders.size(); ++i) {
        if (m_customProviders[i].id == provider.id) {
            m_customProviders[i] = provider;
            saveProviders();
            emit providerUpdated(provider.id);
            return true;
        }
    }
    return false;
}

bool LLMProviderService::removeCustomProvider(const QString& providerId)
{
    for (int i = 0; i < m_customProviders.size(); ++i) {
        if (m_customProviders[i].id == providerId) {
            // 如果删除的是当前激活的提供商，切换到默认
            if (m_activeProviderId == providerId) {
                setActiveProvider(QStringLiteral("openai"));
            }
            m_customProviders.removeAt(i);
            saveProviders();
            emit providersChanged();
            return true;
        }
    }
    return false;
}

QJsonObject LLMProviderService::exportConfig() const
{
    QJsonObject config;
    config.insert(QStringLiteral("activeProvider"), m_activeProviderId);

    QJsonArray providers;
    for (const LLMProvider& p : m_customProviders) {
        providers.append(p.toJson());
    }
    config.insert(QStringLiteral("customProviders"), providers);

    return config;
}

void LLMProviderService::importConfig(const QJsonObject& config)
{
    const QString activeId = config.value(QStringLiteral("activeProvider")).toString();
    if (!activeId.isEmpty() && hasProvider(activeId)) {
        m_activeProviderId = activeId;
    }

    const QJsonArray providers =
        config.value(QStringLiteral("customProviders")).toArray();
    m_customProviders.clear();
    for (const auto& v : providers) {
        m_customProviders.append(LLMProvider(v.toObject()));
    }

    saveProviders();
    emit providersChanged();
    emit activeProviderChanged(m_activeProviderId);
}

void LLMProviderService::loadProviders()
{
    if (!m_settings) {
        return;
    }
    const QString jsonStr = m_settings->get(providerConfigKey());
    if (jsonStr.isEmpty()) {
        return;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(jsonStr.toUtf8());
    if (!doc.isArray()) {
        return;
    }
    m_customProviders.clear();
    const QJsonArray arr = doc.array();
    for (const auto& v : arr) {
        m_customProviders.append(LLMProvider(v.toObject()));
    }
}

void LLMProviderService::saveProviders() const
{
    if (!m_settings) {
        return;
    }
    QJsonArray arr;
    for (const LLMProvider& p : m_customProviders) {
        arr.append(p.toJson());
    }
    const QString jsonStr = QString::fromUtf8(QJsonDocument(arr).toJson());
    m_settings->set(providerConfigKey(), jsonStr);
}

void LLMProviderService::loadActiveProvider()
{
    if (!m_settings) {
        m_activeProviderId = QStringLiteral("openai");
        return;
    }
    const QString id = m_settings->get(activeProviderKey());
    if (id.isEmpty() || !hasProvider(id)) {
        m_activeProviderId = QStringLiteral("openai");
    } else {
        m_activeProviderId = id;
    }
}

void LLMProviderService::testProviderConnection(const QString& providerId)
{
    if (!m_network) {
        emit connectionTestResult(providerId, false, QStringLiteral("网络组件未初始化"));
        return;
    }

    const LLMProvider provider = providerById(providerId);
    if (provider.id.isEmpty()) {
        emit connectionTestResult(providerId, false, QStringLiteral("未找到提供商: %1").arg(providerId));
        return;
    }

    const QString apiKey = getApiKey(providerId);
    if (apiKey.isEmpty()) {
        emit connectionTestResult(providerId, false, QStringLiteral("请先填写 API Key"));
        return;
    }

    // 构建测试 URL
    QString endpoint = getEndpoint(providerId);
    while (endpoint.endsWith('/')) {
        endpoint.chop(1);
    }

    QUrl url(endpoint + QStringLiteral("/models"));

    // 设置 API Key
    m_network->setAuthToken(apiKey);

    // 执行测试
    QString errorMsg;
    bool success = m_network->testConnection(url, &errorMsg);

    if (success) {
        emit connectionTestResult(providerId, true, QStringLiteral("连接成功"));
    } else {
        // 如果 /models 端点不支持，尝试其他方式检测
        // 可能是 API Key 格式问题或端点不支持
        emit connectionTestResult(providerId, false, errorMsg);
    }
}
