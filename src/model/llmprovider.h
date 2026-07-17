#ifndef FRAMEMIND_LLMPROVIDER_H
#define FRAMEMIND_LLMPROVIDER_H

#include <QString>
#include <QJsonObject>
#include <QVector>

/**
 * 支持的大模型提供商类型
 */
enum class LLMProviderType {
    OpenAI,      // OpenAI 官方 API
    Azure,       // Azure OpenAI Service
    DeepSeek,    // 深度求索
    Qianfan,     // 百度文心千帆（阿里云百炼）
    Zhipu,       // 智谱 AI
    Ollama,      // Ollama 本地模型
    Custom       // 自定义兼容 OpenAI API 的服务
};

/**
 * LLM 提供商配置信息
 */
struct LLMProvider {
    QString id;           // 唯一标识符
    QString name;         // 显示名称
    LLMProviderType type; // 提供商类型
    QString endpoint;     // API 端点
    QString defaultModel; // 默认模型
    QString apiKeyName;   // 密钥存储名称（用于 secretGet）
    bool supportsVision;  // 是否支持视觉（多模态）
    bool requiresOrgId;   // 是否需要组织 ID
    QStringList models;   // 支持的模型列表

    // 默认构造
    LLMProvider();

    // 从 JSON 构造
    explicit LLMProvider(const QJsonObject& json);

    // 转为 JSON
    QJsonObject toJson() const;

    // 获取完整 API URL
    QString fullEndpoint() const;

    // 是否为有效的提供商配置
    bool isValid() const;

    // 获取提供商类型的显示名称
    static QString typeToString(LLMProviderType type);

    // 从字符串获取提供商类型
    static LLMProviderType stringToType(const QString& str);
};

/**
 * 预定义的提供商配置
 */
class LLMProviderPresets {
public:
    // 获取所有预设提供商
    static QVector<LLMProvider> allPresets();

    // 获取 OpenAI 提供商
    static LLMProvider openAI();

    // 获取 DeepSeek 提供商
    static LLMProvider deepSeek();

    // 获取阿里云百炼提供商
    static LLMProvider qianfan();

    // 获取智谱 AI 提供商
    static LLMProvider zhipu();

    // 获取 Ollama 提供商
    static LLMProvider ollama();

    // 获取自定义提供商模板
    static LLMProvider custom();
};

#endif // FRAMEMIND_LLMPROVIDER_H
