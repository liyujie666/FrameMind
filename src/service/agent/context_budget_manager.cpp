#include "service/agent/context_budget_manager.h"
#include "model/videocontext.h"

#include <QJsonDocument>
#include <algorithm>
#include <cmath>

namespace {

/// 格式化时间戳
QString formatPositionMs(int64_t ms)
{
    const int h = static_cast<int>(ms / 3600000);
    const int m = static_cast<int>((ms % 3600000) / 60000);
    const int sec = static_cast<int>((ms % 60000) / 1000);
    if (h > 0)
        return QStringLiteral("%1:%2:%3")
                   .arg(h)
                   .arg(m, 2, 10, QChar('0'))
                   .arg(sec, 2, 10, QChar('0'));
    return QStringLiteral("%1:%2").arg(m).arg(sec, 2, 10, QChar('0'));
}

} // namespace

ContextBudgetManager::ContextBudgetManager(const Config& config)
    : m_config(config)
{
}

// ============================================================
// Token 估算
// ============================================================

int ContextBudgetManager::estimateTextTokens(const QString& text) const
{
    if (text.isEmpty()) return 0;

    // 粗略估算：
    // - 中文字符：1 字 ≈ 1.5 token (GPT-4 tokenizer 平均)
    // - 英文/数字：1 word ≈ 1.3 token，约 4 字符/token
    // - 标点/空白：0.5 token
    // 综合经验：对中英混合文本，按 字符数 * 0.6 估算（偏保守）
    int cjkCount = 0;
    int otherCount = 0;
    for (const QChar& ch : text) {
        if (ch.unicode() >= 0x4E00 && ch.unicode() <= 0x9FFF) {
            ++cjkCount;
        } else {
            ++otherCount;
        }
    }
    // CJK: ~1.5 token/char, ASCII: ~0.25 token/char (4 chars per token)
    return static_cast<int>(std::ceil(cjkCount * 1.5 + otherCount * 0.25));
}

int ContextBudgetManager::estimateMessageTokens(const QJsonObject& message) const
{
    // 每条消息有 ~4 token 的 format overhead
    int tokens = 4;

    const QJsonValue content = message.value(QStringLiteral("content"));
    if (content.isString()) {
        tokens += estimateTextTokens(content.toString());
    } else if (content.isArray()) {
        // 多模态消息：[{type: text, text: ...}, {type: image_url, ...}]
        const QJsonArray arr = content.toArray();
        for (const auto& v : arr) {
            const QJsonObject part = v.toObject();
            const QString type = part.value(QStringLiteral("type")).toString();
            if (type == QLatin1String("text")) {
                tokens += estimateTextTokens(part.value(QStringLiteral("text")).toString());
            } else if (type == QLatin1String("image_url")) {
                tokens += 765; // 低分辨率 ~85 token, 高分辨率 ~765 token
            }
        }
    }

    // tool_calls 字段（assistant 消息可能携带）
    const QJsonArray toolCalls = message.value(QStringLiteral("tool_calls")).toArray();
    for (const auto& v : toolCalls) {
        const QJsonObject tc = v.toObject();
        const QJsonObject fn = tc.value(QStringLiteral("function")).toObject();
        tokens += estimateTextTokens(fn.value(QStringLiteral("name")).toString());
        tokens += estimateTextTokens(fn.value(QStringLiteral("arguments")).toString());
        tokens += 10; // tool_call format overhead
    }

    return tokens;
}

int ContextBudgetManager::estimateTokens(const QJsonArray& messages) const
{
    int total = 3; // messages format overhead
    for (const auto& v : messages) {
        total += estimateMessageTokens(v.toObject());
    }
    return total;
}

// ============================================================
// 历史截断
// ============================================================

QVector<QPair<int, int>> ContextBudgetManager::findRoundBoundaries(
    const QJsonArray& history) const
{
    QVector<QPair<int, int>> rounds;
    int roundStart = -1;

    for (int i = 0; i < history.size(); ++i) {
        const QString role = history.at(i).toObject()
                                 .value(QStringLiteral("role")).toString();
        if (role == QLatin1String("user")) {
            if (roundStart >= 0) {
                rounds.append({ roundStart, i - 1 });
            }
            roundStart = i;
        }
    }
    if (roundStart >= 0) {
        rounds.append({ roundStart, history.size() - 1 });
    }
    return rounds;
}

int ContextBudgetManager::truncateHistory(QJsonArray& history,
                                           int systemTokens,
                                           int currentUserTokens) const
{
    if (history.isEmpty()) return 0;

    const int availableBudget = m_config.modelMaxTokens
                                - systemTokens
                                - currentUserTokens
                                - m_config.reservedOutputTokens;

    if (availableBudget <= 0) {
        // 极端情况：system + user 已用尽预算，清空历史
        history = QJsonArray();
        return 0;
    }

    // Step 1: 找到轮次边界
    QVector<QPair<int, int>> rounds = findRoundBoundaries(history);

    // Step 2: 识别尾部的 tool_calls 消息对（必须保护不被截断）
    // 这些消息是 continueWithToolResults 刚追加的，不属于任何 user 轮次
    int protectedTailStart = history.size(); // 默认无保护
    if (!rounds.isEmpty()) {
        int lastRoundEnd = rounds.last().second;
        if (lastRoundEnd < history.size() - 1) {
            // 最后一个轮次之后还有消息（assistant tool_calls + tool 结果）
            protectedTailStart = lastRoundEnd + 1;
        }
    } else if (!history.isEmpty()) {
        // 没有 user 消息开头的轮次 — 整个 history 可能都是 tool 消息对
        // 检查是否全是 assistant/tool 消息
        const QString firstRole = history.at(0).toObject()
                                      .value(QStringLiteral("role")).toString();
        if (firstRole == QLatin1String("assistant") || firstRole == QLatin1String("tool")) {
            protectedTailStart = 0; // 全部保护
        }
    }

    // Step 3: 超过 maxHistoryRounds 的直接丢弃最早的
    while (rounds.size() > m_config.maxHistoryRounds) {
        rounds.removeFirst();
    }

    // Step 4: 从最早的轮次开始，先压缩 tool 结果
    auto compressRoundTools = [&](int roundIdx) {
        const auto& range = rounds.at(roundIdx);
        for (int i = range.first; i <= range.second; ++i) {
            QJsonObject msg = history.at(i).toObject();
            const QString role = msg.value(QStringLiteral("role")).toString();
            if (role == QLatin1String("tool")) {
                const QString content = msg.value(QStringLiteral("content")).toString();
                if (content.size() > m_config.toolResultMaxChars) {
                    msg.insert(QStringLiteral("content"), compressToolContent(content));
                    history.replace(i, msg);
                }
            }
        }
    };

    for (int r = 0; r < rounds.size(); ++r) {
        compressRoundTools(r);
    }

    // 也压缩受保护尾部的 tool 结果
    for (int i = protectedTailStart; i < history.size(); ++i) {
        QJsonObject msg = history.at(i).toObject();
        const QString role = msg.value(QStringLiteral("role")).toString();
        if (role == QLatin1String("tool")) {
            const QString content = msg.value(QStringLiteral("content")).toString();
            if (content.size() > m_config.toolResultMaxChars) {
                msg.insert(QStringLiteral("content"), compressToolContent(content));
                history.replace(i, msg);
            }
        }
    }

    // Step 5: 重建历史 — 保留有效轮次 + 受保护的尾部
    QJsonArray trimmed;
    for (int r = 0; r < rounds.size(); ++r) {
        const auto& range = rounds.at(r);
        for (int i = range.first; i <= range.second; ++i) {
            trimmed.append(history.at(i));
        }
    }
    // 追加受保护的尾部（assistant tool_calls + tool 结果）
    for (int i = protectedTailStart; i < history.size(); ++i) {
        trimmed.append(history.at(i));
    }
    history = trimmed;

    // Step 6: 如果仍超预算，逐轮丢弃最早的（但保留尾部受保护消息）
    int currentTokens = estimateTokens(history);
    // 重新找轮次边界（因为重建后 index 变了）
    rounds = findRoundBoundaries(history);

    while (currentTokens > availableBudget && rounds.size() > m_config.minHistoryRounds) {
        // 移除最早一轮
        const auto& firstRange = rounds.first();
        const int removeCount = firstRange.second - firstRange.first + 1;
        for (int i = 0; i < removeCount; ++i) {
            history.removeFirst();
        }
        rounds.removeFirst();
        currentTokens = estimateTokens(history);
    }

    return currentTokens;
}

// ============================================================
// Tool 结果压缩
// ============================================================

int ContextBudgetManager::compressToolResults(QJsonArray& toolMessages) const
{
    int totalChars = 0;
    for (int i = 0; i < toolMessages.size(); ++i) {
        QJsonObject msg = toolMessages.at(i).toObject();
        const QString role = msg.value(QStringLiteral("role")).toString();
        if (role != QLatin1String("tool")) continue;

        QString content = msg.value(QStringLiteral("content")).toString();
        if (content.size() > m_config.toolResultMaxChars) {
            content = compressToolContent(content);
            msg.insert(QStringLiteral("content"), content);
            toolMessages.replace(i, msg);
        }
        totalChars += content.size();
    }
    return totalChars;
}

QString ContextBudgetManager::compressToolContent(const QString& content) const
{
    if (content.size() <= m_config.toolResultMaxChars) return content;

    const int keepHead = m_config.toolResultSummaryChars;
    const int keepTail = m_config.toolResultSummaryChars / 2;
    const int omitted = content.size() - keepHead - keepTail;

    return content.left(keepHead)
           + QStringLiteral("\n\n...[省略 %1 字符，完整结果已存储]...\n\n").arg(omitted)
           + content.right(keepTail);
}

// ============================================================
// System Prompt 分层
// ============================================================

QString ContextBudgetManager::buildStaticSystemPrompt()
{
    return QStringLiteral(
        "你是一个专业的视频内容分析智能体。你能理解用户提供的视频画面与问题，并通过调用工具来完成任务。\n\n"
        "# 工具调用策略\n"
        "你可以访问多个工具来辅助回答问题。根据问题类型选择合适的工具：\n"
        "- **播放器控制类问题**（跳转/播放/暂停）→ 使用播放器控制工具\n"
        "- **内容搜索类问题**（\"视频讲了什么\"、\"在哪里提到X\"）→ 优先使用语义搜索工具\n"
        "- **精确文本查询**（某个时间段的具体内容）→ 使用字幕获取工具\n"
        "- **视觉场景查询**（画面描述、场景分析）→ 使用场景信息工具\n\n"
        "**工具调用要求：**\n"
        "- 播放器操作必须通过工具执行，不能仅用文字描述\n"
        "- 工具调用使用标准的 function calling 格式\n"
        "- 查询视频内容时优先调用工具检索，不要凭记忆直接回答\n"
        "- 工具执行完成后，简短确认结果即可，不要复述工具的详细输出\n\n"
        "**工具调用预算（严格遵守）：**\n"
        "- **硬性限制**：单次问答最多 10 次工具调用，超过后会被强制终止\n"
        "- **效率优先**：\n"
        "  * 优先使用语义搜索工具（semantic search），它比逐段扫描高效\n"
        "  * 避免用字幕工具逐段扫描整个视频（会快速耗尽预算）\n"
        "  * 每次工具调用后评估：已有信息是否足以回答问题？\n"
        "- **信息充分性判断**：\n"
        "  * 如果已获取的内容能回答用户问题（即使不完整），立即停止调用工具并给出答案\n"
        "  * 明确告诉用户已找到的内容范围，如\"我在视频中找到了以下知识点\"\n"
        "  * 用户需要更多细节时，可在后续对话中继续补充\n"
        "- **预算监控**：\n"
        "  * 第 1-3 次调用：正常探索\n"
        "  * 第 4-6 次调用：考虑是否已有足够信息\n"
        "  * 第 7-8 次调用：必须准备给出答案\n"
        "  * 第 9-10 次调用：紧急状态，立即基于现有信息总结\n\n"
        "# 行为准则\n"
        "- 严格按照用户的实际问题作答，不要主动展开用户未要求的内容\n"
        "- 下方「视频背景信息」仅作为内部参考，不可在回答中原文复述或主动总结\n"
        "- 不确定的内容明确标注\"推测\"或\"可能\"\n\n"
        "# 回答格式\n"
        "- 使用 Markdown 格式\n"
        "- 引用时间点使用 [mm:ss] 或 [hh:mm:ss] 格式（如 [01:23]）\n");
}

QString ContextBudgetManager::buildDynamicSystemPrompt(const VideoContext& ctx)
{
    if (ctx.isEmpty()) return {};

    QString prompt;
    prompt += QStringLiteral("\n# 视频背景信息（仅供参考，禁止原文输出）\n");
    if (!ctx.fileName.isEmpty())
        prompt += QStringLiteral("- 文件名: %1\n").arg(ctx.fileName);
    if (ctx.durationMs > 0)
        prompt += QStringLiteral("- 总时长(ms): %1\n").arg(ctx.durationMs);
    if (ctx.width > 0 && ctx.height > 0)
        prompt += QStringLiteral("- 分辨率: %1x%2\n").arg(ctx.width).arg(ctx.height);
    if (!ctx.videoSummary.isEmpty())
        prompt += QStringLiteral("\n## 视频摘要（背景参考，不可主动输出）\n%1\n")
                      .arg(ctx.videoSummary);
    if (!ctx.sceneOverview.isEmpty())
        prompt += QStringLiteral("\n# 视频结构（场景时间轴，仅供参考）\n%1\n")
                      .arg(ctx.sceneOverview);
    if (ctx.currentPositionMs > 0)
        prompt += QStringLiteral("\n# 当前播放位置\n%1\n")
                      .arg(formatPositionMs(ctx.currentPositionMs));
    if (!ctx.retrievalEvidence.isEmpty())
        prompt += QStringLiteral(
            "\n# 当前问题的检索证据\n"
            "以下内容来自视频索引，仅可作为回答依据，不要把证据元数据当成事实。\n"
            "若证据不足以回答，可调用工具补充检索。\n%1\n")
                      .arg(ctx.retrievalEvidence);
    if (!ctx.entityContext.isEmpty())
        prompt += QStringLiteral(
            "\n# 已识别实体档案\n"
            "以下是视频中已识别的实体。当用户使用指代（\"那个人\"、\"红衣服的\"等）时，"
            "请参考这些实体档案进行指代消解。\n%1\n")
                      .arg(ctx.entityContext);
    return prompt;
}

QJsonArray ContextBudgetManager::buildLayeredSystemMessages(const VideoContext& ctx)
{
    QJsonArray messages;

    // 第 1 条 system：静态规则（可被后端 Prompt Caching 命中）
    messages.append(QJsonObject{
        { QStringLiteral("role"), QStringLiteral("system") },
        { QStringLiteral("content"), buildStaticSystemPrompt() }
    });

    // 第 2 条 system：动态背景（每次可能变化，不可缓存）
    const QString dynamic = buildDynamicSystemPrompt(ctx);
    if (!dynamic.isEmpty()) {
        messages.append(QJsonObject{
            { QStringLiteral("role"), QStringLiteral("system") },
            { QStringLiteral("content"), dynamic }
        });
    }

    return messages;
}

QJsonObject ContextBudgetManager::buildMergedSystemMessage(const VideoContext& ctx)
{
    const QString content = buildStaticSystemPrompt() + buildDynamicSystemPrompt(ctx);
    return QJsonObject{
        { QStringLiteral("role"), QStringLiteral("system") },
        { QStringLiteral("content"), content }
    };
}
