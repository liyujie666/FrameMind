#include "model/llmprovider.h"

#include <QJsonArray>

LLMProvider::LLMProvider()
    : type(LLMProviderType::Custom)
    , supportsVision(true)
    , supportsToolCalling(true)
    , requiresOrgId(false)
{
}

LLMProvider::LLMProvider(const QJsonObject& json)
    : id(json.value(QStringLiteral("id")).toString())
    , name(json.value(QStringLiteral("name")).toString())
    , type(stringToType(json.value(QStringLiteral("type")).toString()))
    , endpoint(json.value(QStringLiteral("endpoint")).toString())
    , defaultModel(json.value(QStringLiteral("defaultModel")).toString())
    , apiKeyName(json.value(QStringLiteral("apiKeyName")).toString(QStringLiteral("secret.llm.api_key")))
    , supportsVision(json.value(QStringLiteral("supportsVision")).toBool(true))
    , supportsToolCalling(json.value(QStringLiteral("supportsToolCalling")).toBool(true))
    , requiresOrgId(json.value(QStringLiteral("requiresOrgId")).toBool(false))
{
    const QJsonArray modelsArray = json.value(QStringLiteral("models")).toArray();
    for (const auto& v : modelsArray) {
        models.append(v.toString());
    }
}

QJsonObject LLMProvider::toJson() const
{
    QJsonObject json;
    json.insert(QStringLiteral("id"), id);
    json.insert(QStringLiteral("name"), name);
    json.insert(QStringLiteral("type"), typeToString(type));
    json.insert(QStringLiteral("endpoint"), endpoint);
    json.insert(QStringLiteral("defaultModel"), defaultModel);
    json.insert(QStringLiteral("apiKeyName"), apiKeyName);
    json.insert(QStringLiteral("supportsVision"), supportsVision);
    json.insert(QStringLiteral("supportsToolCalling"), supportsToolCalling);
    json.insert(QStringLiteral("requiresOrgId"), requiresOrgId);

    QJsonArray modelsArray;
    for (const QString& m : models) {
        modelsArray.append(m);
    }
    json.insert(QStringLiteral("models"), modelsArray);

    return json;
}

QString LLMProvider::fullEndpoint() const
{
    QString url = endpoint;
    while (url.endsWith('/')) {
        url.chop(1);
    }
    return url;
}

bool LLMProvider::isValid() const
{
    return !id.isEmpty() && !endpoint.isEmpty();
}

QString LLMProvider::typeToString(LLMProviderType type)
{
    switch (type) {
        case LLMProviderType::OpenAI:  return QStringLiteral("openai");
        case LLMProviderType::Azure:    return QStringLiteral("azure");
        case LLMProviderType::DeepSeek: return QStringLiteral("deepseek");
        case LLMProviderType::Qianfan:  return QStringLiteral("qianfan");
        case LLMProviderType::Zhipu:    return QStringLiteral("zhipu");
        case LLMProviderType::Ollama:   return QStringLiteral("ollama");
        case LLMProviderType::Custom:   return QStringLiteral("custom");
        default:                        return QStringLiteral("custom");
    }
}

LLMProviderType LLMProvider::stringToType(const QString& str)
{
    const QString lower = str.toLower();
    if (lower == QStringLiteral("openai"))    return LLMProviderType::OpenAI;
    if (lower == QStringLiteral("azure"))      return LLMProviderType::Azure;
    if (lower == QStringLiteral("deepseek"))   return LLMProviderType::DeepSeek;
    if (lower == QStringLiteral("qianfan"))    return LLMProviderType::Qianfan;
    if (lower == QStringLiteral("zhipu"))      return LLMProviderType::Zhipu;
    if (lower == QStringLiteral("ollama"))     return LLMProviderType::Ollama;
    return LLMProviderType::Custom;
}

// -------------------- 预设提供商 --------------------

LLMProvider LLMProviderPresets::openAI()
{
    LLMProvider p;
    p.id = QStringLiteral("openai");
    p.name = QStringLiteral("OpenAI");
    p.type = LLMProviderType::OpenAI;
    p.endpoint = QStringLiteral("https://api.openai.com/v1");
    p.defaultModel = QStringLiteral("gpt-4o");
    p.apiKeyName = QStringLiteral("secret.llm.openai");
    p.supportsVision = true;
    p.supportsToolCalling = true;
    p.requiresOrgId = false;
    p.models = {
        QStringLiteral("gpt-4o"),
        QStringLiteral("gpt-4o-mini"),
        QStringLiteral("gpt-4-turbo"),
        QStringLiteral("gpt-4"),
        QStringLiteral("gpt-3.5-turbo")
    };
    return p;
}

LLMProvider LLMProviderPresets::deepSeek()
{
    LLMProvider p;
    p.id = QStringLiteral("deepseek");
    p.name = QStringLiteral("DeepSeek");
    p.type = LLMProviderType::DeepSeek;
    p.endpoint = QStringLiteral("https://api.deepseek.com/v1");
    p.defaultModel = QStringLiteral("deepseek-chat");
    p.apiKeyName = QStringLiteral("secret.llm.deepseek");
    p.supportsVision = false;
    p.supportsToolCalling = true;
    p.requiresOrgId = false;
    p.models = {
        QStringLiteral("deepseek-chat"),
        QStringLiteral("deepseek-coder")
    };
    return p;
}

LLMProvider LLMProviderPresets::qianfan()
{
    LLMProvider p;
    p.id = QStringLiteral("qianfan");
    p.name = QStringLiteral("阿里云百炼 (Qianfan)");
    p.type = LLMProviderType::Qianfan;
    p.endpoint = QStringLiteral("https://dashscope.aliyuncs.com/compatible-mode/v1");
    p.defaultModel = QStringLiteral("qwen-plus");
    p.apiKeyName = QStringLiteral("secret.llm.qianfan");
    p.supportsVision = true;
    p.supportsToolCalling = true;  // 注意：qwen-vl-* 模型不支持，需要动态判断
    p.requiresOrgId = false;
    p.models = {
        QStringLiteral("qwen-plus"),
        QStringLiteral("qwen-turbo"),
        QStringLiteral("qwen-max"),
        QStringLiteral("qwen-vl-plus"),
        QStringLiteral("qwen-vl-max"),
        QStringLiteral("qwen2.5-72b-instruct"),
        QStringLiteral("yi-large")
    };
    return p;
}

LLMProvider LLMProviderPresets::zhipu()
{
    LLMProvider p;
    p.id = QStringLiteral("zhipu");
    p.name = QStringLiteral("智谱 AI (GLM)");
    p.type = LLMProviderType::Zhipu;
    p.endpoint = QStringLiteral("https://open.bigmodel.cn/api/paas/v4");
    p.defaultModel = QStringLiteral("glm-4");
    p.apiKeyName = QStringLiteral("secret.llm.zhipu");
    p.supportsVision = true;
    p.supportsToolCalling = true;
    p.requiresOrgId = false;
    p.models = {
        QStringLiteral("glm-4"),
        QStringLiteral("glm-4v"),
        QStringLiteral("glm-4-plus"),
        QStringLiteral("glm-3-turbo")
    };
    return p;
}

LLMProvider LLMProviderPresets::ollama()
{
    LLMProvider p;
    p.id = QStringLiteral("ollama");
    p.name = QStringLiteral("Ollama (本地)");
    p.type = LLMProviderType::Ollama;
    p.endpoint = QStringLiteral("http://localhost:11434/v1");
    p.defaultModel = QStringLiteral("llama3");
    p.apiKeyName = QStringLiteral("secret.llm.ollama");
    p.supportsVision = false;  // 取决于具体模型
    p.supportsToolCalling = true;
    p.requiresOrgId = false;
    p.models = {
        QStringLiteral("llama3"),
        QStringLiteral("llama3.1"),
        QStringLiteral("mistral"),
        QStringLiteral("codellama"),
        QStringLiteral("qwen2.5"),
        QStringLiteral("deepseek-coder-v2")
    };
    return p;
}

LLMProvider LLMProviderPresets::custom()
{
    LLMProvider p;
    p.id = QStringLiteral("custom");
    p.name = QStringLiteral("自定义");
    p.type = LLMProviderType::Custom;
    p.endpoint = QStringLiteral("https://your-api.example.com/v1");
    p.defaultModel = QStringLiteral("gpt-4o");
    p.apiKeyName = QStringLiteral("secret.llm.custom");
    p.supportsVision = true;
    p.supportsToolCalling = true;
    p.requiresOrgId = false;
    p.models = {
        QStringLiteral("gpt-4o"),
        QStringLiteral("gpt-4o-mini")
    };
    return p;
}

QVector<LLMProvider> LLMProviderPresets::allPresets()
{
    return {
        openAI(),
        deepSeek(),
        qianfan(),
        zhipu(),
        ollama(),
        custom()
    };
}
