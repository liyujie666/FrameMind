#ifndef FRAMEMIND_LLMPROVIDERSERVICE_H
#define FRAMEMIND_LLMPROVIDERSERVICE_H

#include <QObject>
#include <QString>
#include <QVector>
#include <QUrl>

#include "model/llmprovider.h"

class SettingsService;
class NetworkClient;

/**
 * LLM 提供商配置服务
 *
 * 负责管理多个 LLM 提供商的配置，包括：
 * - 预设提供商的配置管理
 * - 自定义提供商的增删改查
 * - 当前激活的提供商
 * - 与 SettingsService 集成进行持久化
 */
class LLMProviderService : public QObject {
    Q_OBJECT
public:
    explicit LLMProviderService(SettingsService* settings, QObject* parent = nullptr);
    ~LLMProviderService() override;

    /// 设置网络客户端（用于测试连接）
    void setNetworkClient(NetworkClient* network);

    // ---- 提供商列表管理 ----

    /// 获取所有可用的提供商（包括预设和自定义）
    QVector<LLMProvider> allProviders() const;

    /// 获取预设提供商列表
    QVector<LLMProvider> presetProviders() const;

    /// 获取自定义提供商列表
    QVector<LLMProvider> customProviders() const;

    /// 根据 ID 获取提供商
    LLMProvider providerById(const QString& id) const;

    /// 是否存在指定 ID 的提供商
    bool hasProvider(const QString& id) const;

    // ---- 激活提供商管理 ----

    /// 获取当前激活的提供商
    LLMProvider activeProvider() const;

    /// 获取当前激活的提供商 ID
    QString activeProviderId() const;

    /// 设置激活的提供商
    void setActiveProvider(const QString& providerId);

    // ---- 提供商配置读写 ----

    /// 获取提供商的 API Key
    QString getApiKey(const QString& providerId) const;

    /// 设置提供商的 API Key
    bool setApiKey(const QString& providerId, const QString& apiKey);

    /// 删除提供商的 API Key
    bool deleteApiKey(const QString& providerId);

    /// 获取提供商的自定义端点
    QString getEndpoint(const QString& providerId) const;

    /// 设置提供商的自定义端点
    void setEndpoint(const QString& providerId, const QString& endpoint);

    /// 获取提供商的默认模型
    QString getModel(const QString& providerId) const;

    /// 设置提供商的默认模型
    void setModel(const QString& providerId, const QString& model);

    // ---- 自定义提供商管理 ----

    /// 添加自定义提供商
    bool addCustomProvider(const LLMProvider& provider);

    /// 更新自定义提供商
    bool updateCustomProvider(const LLMProvider& provider);

    /// 删除自定义提供商
    bool removeCustomProvider(const QString& providerId);

    // ---- 配置导入导出 ----

    /// 导出所有提供商配置为 JSON
    QJsonObject exportConfig() const;

    /// 从 JSON 导入提供商配置
    void importConfig(const QJsonObject& config);

    // ---- 连接测试 ----

    /// 测试指定提供商的连接
    /// @param overrideKey 若非空，使用该 key 而不是从存储读取（测试时避免提前写入）
    void testProviderConnection(const QString& providerId,
                                const QString& overrideKey = {});

    // ---- 模型能力检测 ----

    /// 检查指定提供商和模型是否支持 Tool Calling
    bool supportsToolCalling(const QString& providerId, const QString& modelName) const;

signals:
    /// 提供商配置变更信号
    void providerUpdated(const QString& providerId);

    /// 激活提供商变更信号
    void activeProviderChanged(const QString& providerId);

    /// 提供商列表变更信号
    void providersChanged();

    /// 连接测试结果信号
    void connectionTestResult(const QString& providerId, bool success, const QString& message);

private:
    void loadProviders();
    void saveProviders() const;
    void loadActiveProvider();

    static QString providerConfigKey();
    static QString activeProviderKey();

    SettingsService* m_settings = nullptr;
    NetworkClient* m_network = nullptr;
    QVector<LLMProvider> m_customProviders;
    QString m_activeProviderId;
};

#endif // FRAMEMIND_LLMPROVIDERSERVICE_H
