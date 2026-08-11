#include "workflow_factory.h"

#include "nodes/llm_node.h"
#include "nodes/condition_node.h"
#include "nodes/function_node.h"
#include "nodes/tool_node.h"
#include "nodes/human_node.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDebug>

WorkflowFactory::WorkflowFactory()
{
}

void WorkflowFactory::setDependencies(Dependencies deps)
{
    m_deps = std::move(deps);
}

WorkflowGraph WorkflowFactory::fromJson(const QJsonObject& json)
{
    WorkflowGraph graph;

    // 设置最大迭代次数
    int maxIter = json["maxIterations"].toInt(20);
    graph.setMaxIterations(maxIter);

    // 设置入口节点
    QString entry = json["entry"].toString();
    graph.setEntryNode(entry);

    // 构建节点
    QJsonArray nodesArray = json["nodes"].toArray();
    for (const auto& nodeVal : nodesArray) {
        QJsonObject nodeDef = nodeVal.toObject();
        auto node = createNode(nodeDef);
        if (node) {
            graph.addNode(std::move(node));
        } else {
            qWarning() << "[WorkflowFactory] Failed to create node:"
                       << nodeDef["id"].toString();
        }
    }

    // 构建边
    QJsonArray edgesArray = json["edges"].toArray();
    for (const auto& edgeVal : edgesArray) {
        QJsonObject edgeDef = edgeVal.toObject();
        QString from = edgeDef["from"].toString();
        QString to = edgeDef["to"].toString();
        QString condition = edgeDef["condition"].toString();
        int priority = edgeDef["priority"].toInt(0);
        graph.addEdge(from, to, condition, priority);
    }

    return graph;
}

WorkflowGraph WorkflowFactory::fromFile(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "[WorkflowFactory] Cannot open file:" << filePath;
        return WorkflowGraph();
    }

    QByteArray data = file.readAll();
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject()) {
        qWarning() << "[WorkflowFactory] Invalid JSON in file:" << filePath;
        return WorkflowGraph();
    }

    return fromJson(doc.object());
}

void WorkflowFactory::registerNodeFactory(const QString& type, NodeCreator creator)
{
    m_customFactories[type] = std::move(creator);
}

void WorkflowFactory::registerFunctionHandler(
    const QString& name,
    std::function<void(WorkflowState&, NodeCallback)> handler)
{
    m_functionHandlers[name] = std::move(handler);
}

//───内部实现 ────────────────────────────────────────────────────

std::shared_ptr<IWorkflowNode> WorkflowFactory::createNode(const QJsonObject& nodeDef)
{
    QString id = nodeDef["id"].toString();
    QString type = nodeDef["type"].toString();

    if (id.isEmpty() || type.isEmpty()) {
        return nullptr;
    }

    // 优先使用自定义工厂
    if (m_customFactories.contains(type)) {
        return m_customFactories[type](id, nodeDef, m_deps);
    }

    // 内置类型
    if (type == "llm") return createLLMNode(id, nodeDef);
    if (type == "condition") return createConditionNode(id, nodeDef);
    if (type == "function") return createFunctionNode(id, nodeDef);
    if (type == "tool") return createToolNode(id, nodeDef);
    if (type == "human") return createHumanNode(id, nodeDef);

    qWarning() << "[WorkflowFactory] Unknown node type:" << type;
    return nullptr;
}

std::shared_ptr<IWorkflowNode> WorkflowFactory::createLLMNode(
    const QString& id, const QJsonObject& config)
{
    LLMNode::Config cfg;
    cfg.systemPrompt = config["systemPrompt"].toString();
    cfg.modelOverride = config["model"].toString();
    cfg.enableToolCalling = config["enableTools"].toBool(false);
    cfg.maxToolRounds = config["maxToolRounds"].toInt(5);
    cfg.questionKey = config["questionKey"].toString("question");
    cfg.answerKey = config["answerKey"].toString("answer");
    cfg.timeout = config["timeout"].toInt(120000);

    QJsonArray allowedArr = config["allowedTools"].toArray();
    for (const auto& v : allowedArr) {
        cfg.allowedTools.append(v.toString());
    }

    return std::make_shared<LLMNode>(id, std::move(cfg),
                                      m_deps.agentService,
                                      m_deps.orchestrator);
}

std::shared_ptr<IWorkflowNode> WorkflowFactory::createConditionNode(
    const QString& id, const QJsonObject& config)
{
    // JSON 条件节点使用简单的 state key比较
    // 格式: { "expression": "state.key op value" }
    // 支持: "state.key == value" → 返回 "true"/"false"
    // 或者: "state.key" → 直接返回 state中该key 的字符串值作为路由
    QString expression = config["expression"].toString();
    QString stateKey = config["stateKey"].toString();

    if (!stateKey.isEmpty()) {
        // 直接以state value作为路由名
        return std::make_shared<ConditionNode>(id,
            [stateKey](const WorkflowState& state) -> QString {
                return state.get(stateKey).toString();
            });
    }

    if (!expression.isEmpty()) {
        // 简单表达式: "key >= threshold"
        // 解析 key、op、value
        QStringList parts = expression.split(' ', Qt::SkipEmptyParts);
        if (parts.size() == 3) {
            QString key = parts[0];
            QString op = parts[1];
            double threshold = parts[2].toDouble();

            return std::make_shared<ConditionNode>(id,
                [key, op, threshold](const WorkflowState& state) -> QString {
                    double val = state.get(key).toDouble();
                    if (op == ">=" && val >= threshold) return "true";
                    if (op == ">" && val > threshold) return "true";
                    if (op == "<=" && val <= threshold) return "true";
                    if (op == "<" && val < threshold) return "true";
                    if (op == "==" && qFuzzyCompare(val, threshold)) return "true";
                    return "false";
                });
        }
    }

    // 默认：总是返回空路由（走默认边）
    return std::make_shared<ConditionNode>(id,
        [](const WorkflowState&) -> QString { return {}; });
}

std::shared_ptr<IWorkflowNode> WorkflowFactory::createFunctionNode(
    const QString& id, const QJsonObject& config)
{
    QString handler = config["handler"].toString();
    int timeout = config["timeout"].toInt(0);
    int retries = config["maxRetries"].toInt(0);

    // 查找已注册的 handler
    auto it = m_functionHandlers.find(handler);
    if (it != m_functionHandlers.end()) {
        return std::make_shared<FunctionNode>(id, it.value(), timeout, retries);
    }

    // handler 未注册，创建pass-through 节点
    qWarning() << "[WorkflowFactory] Function handler not registered:" << handler;
    return std::make_shared<FunctionNode>(id, nullptr, timeout, retries);
}

std::shared_ptr<IWorkflowNode> WorkflowFactory::createToolNode(
    const QString& id, const QJsonObject& config)
{
    QString toolName = config["toolName"].toString();
    QString argsKey = config["argsKey"].toString("tool_args");
    QString resultKey = config["resultKey"].toString();

    return std::make_shared<ToolNode>(id, toolName, m_deps.toolRegistry,
                                      argsKey, resultKey);
}

std::shared_ptr<IWorkflowNode> WorkflowFactory::createHumanNode(
    const QString& id, const QJsonObject& config)
{
    QString prompt = config["prompt"].toString();
    QString inputKey = config["inputKey"].toString("human_input");

    return std::make_shared<HumanNode>(id, prompt, inputKey);
}
