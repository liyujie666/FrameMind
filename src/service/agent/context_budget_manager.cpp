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

    // P0修复：改进的token估算算法
    // 基于对GPT-4/Claude tokenizer的实际测试结果
    
    int cjkCount = 0;
    int asciiCount = 0;
    int punctCount = 0;
    int digitCount = 0;
    int whitespaceCount = 0;
    
    for (const QChar& ch : text) {
        const ushort code = ch.unicode();
        
        // CJK统一表意文字 (中日韩汉字)
        if ((code >= 0x4E00 && code <= 0x9FFF) ||   // CJK基本区
            (code >= 0x3400 && code <= 0x4DBF) ||   // CJK扩展A
            (code >= 0x20000 && code <= 0x2A6DF)) { // CJK扩展B
            ++cjkCount;
        }
        // 数字
        else if (ch.isDigit()) {
            ++digitCount;
        }
        // 空白字符
        else if (ch.isSpace()) {
            ++whitespaceCount;
        }
        // 标点符号
        else if (ch.isPunct()) {
            ++punctCount;
        }
        // ASCII字母
        else if (code < 128) {
            ++asciiCount;
        }
        // 其他字符按CJK处理
        else {
            ++cjkCount;
        }
    }
    
    // 更精确的token估算系数（基于实测）
    // CJK字符: 平均1.6 token/char (包含常见词组效应)
    // 英文单词: 平均4个字符一个token，但考虑空格，约0.3 token/char
    // 数字: 连续数字约0.3 token/char
    // 标点: 大多数1个标点1个token，但某些可能合并
    // 空格: 通常被合并到相邻token
    
    const double cjkTokens = cjkCount * 1.6;
    const double asciiTokens = asciiCount * 0.3;
    const double digitTokens = digitCount * 0.3;
    const double punctTokens = punctCount * 0.8;
    const double whitespaceTokens = whitespaceCount * 0.1;
    
    // JSON格式额外开销（如果包含大量引号、括号、逗号）
    int jsonOverhead = 0;
    if (text.contains('{') || text.contains('[')) {
        // 估算JSON结构字符数
        int structChars = text.count('{') + text.count('}') + 
                         text.count('[') + text.count(']') + 
                         text.count(':') + text.count(',');
        jsonOverhead = structChars / 4; // JSON结构约增加25%开销
    }
    
    int totalTokens = static_cast<int>(std::ceil(
        cjkTokens + asciiTokens + digitTokens + punctTokens + whitespaceTokens
    )) + jsonOverhead;
    
    // 基础开销：任何非空文本至少1个token
    return std::max(1, totalTokens);
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
                // P0修复：根据实际图片信息估算token
                const QJsonObject imageUrl = part.value(QStringLiteral("image_url")).toObject();
                const QString detail = imageUrl.value(QStringLiteral("detail")).toString("auto");
                
                // 尝试从metadata获取图片尺寸
                int width = 1024;  // 默认假设
                int height = 768;
                
                // 检查是否有size信息
                if (part.contains(QStringLiteral("width")) && part.contains(QStringLiteral("height"))) {
                    width = part.value(QStringLiteral("width")).toInt(1024);
                    height = part.value(QStringLiteral("height")).toInt(768);
                }
                
                tokens += estimateImageTokens(width, height, detail);
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

    // P1修复：Step 0 - 检查并限制tool消息对数量
    const int toolPairCount = countToolMessagePairs(history);
    if (toolPairCount > m_config.maxToolMessagesTotal) {
        const int toRemove = toolPairCount - m_config.maxToolMessagesTotal;
        qDebug() << "[ContextBudgetManager] Tool消息对超限，需要移除"
                 << "当前数量=" << toolPairCount
                 << "限制=" << m_config.maxToolMessagesTotal
                 << "需移除=" << toRemove;
        removeEarlyToolMessages(history, toRemove);
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
    // P0修复：如果启用了对话摘要，将早期轮次压缩为摘要而非直接丢弃
    if (m_config.enableConversationSummary && rounds.size() > m_config.summaryThresholdRounds) {
        // 计算需要摘要的轮次范围
        const int keepRecentRounds = m_config.summaryThresholdRounds;
        if (rounds.size() > keepRecentRounds) {
            // 提取早期轮次生成摘要
            const int summaryEndIdx = rounds.size() - keepRecentRounds;
            QString summary = summarizeEarlyRounds(history, 0, summaryEndIdx);
            
            // 移除早期轮次
            for (int i = 0; i < summaryEndIdx; ++i) {
                rounds.removeFirst();
            }
            
            // 在历史开头插入摘要消息
            QJsonObject summaryMsg;
            summaryMsg.insert(QStringLiteral("role"), QStringLiteral("system"));
            summaryMsg.insert(QStringLiteral("content"), summary);
            history.prepend(summaryMsg);
            
            qDebug() << "[ContextBudgetManager] 生成对话摘要"
                     << "摘要轮次=" << summaryEndIdx
                     << "保留轮次=" << keepRecentRounds
                     << "摘要字符数=" << summary.size();
        }
    }
    
    // 传统截断：超过 maxHistoryRounds 的直接丢弃最早的
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
                // P1修复：根据内容大小采用不同的压缩策略
                int maxChars = m_config.toolResultMaxChars;
                int summaryChars = m_config.toolResultSummaryChars;
                
                // 对于早期轮次（非最近3轮），使用更激进的压缩
                const int roundsFromEnd = rounds.size() - roundIdx - 1;
                if (roundsFromEnd > 3 && content.size() > m_config.toolMessageAggressiveCompressThreshold) {
                    maxChars = m_config.toolMessageAggressiveCompressThreshold;
                    summaryChars = m_config.toolMessageAggressiveSummaryChars;
                }
                
                if (content.size() > maxChars) {
                    const QString compressed = (summaryChars == m_config.toolResultSummaryChars)
                        ? compressToolContent(content)
                        : compressToolContentAggressive(content, maxChars, summaryChars);
                    msg.insert(QStringLiteral("content"), compressed);
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

QString ContextBudgetManager::compressToolContentAggressive(const QString& content,
                                                             int maxChars,
                                                             int summaryChars) const
{
    if (content.size() <= maxChars) return content;

    const int keepHead = summaryChars;
    const int keepTail = summaryChars / 2;
    const int omitted = content.size() - keepHead - keepTail;

    return content.left(keepHead)
           + QStringLiteral("\n...[省略 %1 字符]...\n").arg(omitted)
           + content.right(keepTail);
}

// ============================================================
// P1修复：Tool消息管理
// ============================================================

int ContextBudgetManager::countToolMessagePairs(const QJsonArray& history) const
{
    int count = 0;
    bool foundAssistantToolCall = false;
    
    for (int i = 0; i < history.size(); ++i) {
        const QJsonObject msg = history.at(i).toObject();
        const QString role = msg.value(QStringLiteral("role")).toString();
        
        if (role == QLatin1String("assistant")) {
            const QJsonArray toolCalls = msg.value(QStringLiteral("tool_calls")).toArray();
            if (!toolCalls.isEmpty()) {
                foundAssistantToolCall = true;
                count += toolCalls.size();  // 每个tool_call算一对
            }
        } else if (role == QLatin1String("tool")) {
            // tool消息通常跟在assistant tool_calls之后
            if (!foundAssistantToolCall) {
                // 孤立的tool消息（不应该出现，但防御性计数）
                count++;
            }
        } else if (role == QLatin1String("user")) {
            // 新的用户轮次开始，重置标志
            foundAssistantToolCall = false;
        }
    }
    
    return count;
}

void ContextBudgetManager::removeEarlyToolMessages(QJsonArray& history, int targetRemoveCount) const
{
    if (targetRemoveCount <= 0) return;
    
    int removed = 0;
    QVector<int> indicesToRemove;
    
    // 从头开始扫描，找到早期的tool消息对并标记删除
    for (int i = 0; i < history.size() && removed < targetRemoveCount; ++i) {
        const QJsonObject msg = history.at(i).toObject();
        const QString role = msg.value(QStringLiteral("role")).toString();
        
        if (role == QLatin1String("assistant")) {
            const QJsonArray toolCalls = msg.value(QStringLiteral("tool_calls")).toArray();
            if (!toolCalls.isEmpty()) {
                // 这是一个assistant tool_calls消息，标记删除
                indicesToRemove.append(i);
                
                // 继续往后找对应的tool结果消息
                for (int j = i + 1; j < history.size(); ++j) {
                    const QJsonObject nextMsg = history.at(j).toObject();
                    const QString nextRole = nextMsg.value(QStringLiteral("role")).toString();
                    
                    if (nextRole == QLatin1String("tool")) {
                        indicesToRemove.append(j);
                    } else if (nextRole == QLatin1String("assistant") || nextRole == QLatin1String("user")) {
                        // 遇到下一个assistant或user，停止
                        break;
                    }
                }
                
                removed += toolCalls.size();
            }
        }
    }
    
    // 从后往前删除，避免索引变化
    std::sort(indicesToRemove.begin(), indicesToRemove.end(), std::greater<int>());
    for (int idx : indicesToRemove) {
        history.removeAt(idx);
    }
    
    qDebug() << "[ContextBudgetManager] 移除早期Tool消息"
             << "目标移除=" << targetRemoveCount
             << "实际移除=" << removed
             << "删除消息数=" << indicesToRemove.size();
}

// ============================================================
// P0修复：对话摘要生成
// ============================================================

QString ContextBudgetManager::summarizeEarlyRounds(const QJsonArray& history, 
                                                    int startIdx, 
                                                    int endIdx) const
{
    QStringList topics;
    QStringList entities;
    QStringList conclusions;
    QStringList timeReferences;
    int totalRounds = 0;
    
    // 遍历早期轮次，提取关键信息
    bool inRound = false;
    QString currentUserMsg;
    
    for (int i = startIdx; i < qMin(endIdx, history.size()); ++i) {
        const QJsonObject msg = history.at(i).toObject();
        const QString role = msg.value(QStringLiteral("role")).toString();
        const QJsonValue content = msg.value(QStringLiteral("content"));
        
        if (role == QLatin1String("user")) {
            if (inRound) {
                totalRounds++;
            }
            inRound = true;
            currentUserMsg = content.toString();
            
            // 提取话题关键词（简单启发式）
            if (currentUserMsg.contains("什么") || currentUserMsg.contains("哪") || 
                currentUserMsg.contains("是谁") || currentUserMsg.contains("做了")) {
                // 提取问句主题
                QString topic = currentUserMsg.left(50).trimmed();
                if (!topic.isEmpty() && topics.size() < 5) {
                    topics.append(topic);
                }
            }
            
            // 提取时间引用
            QRegularExpression timePattern(R"(\[?\d{1,2}:\d{2}(?::\d{2})?\]?|\d+分\d+秒|前半部分|后半段|开头|结尾)");
            auto timeMatch = timePattern.match(currentUserMsg);
            if (timeMatch.hasMatch() && timeReferences.size() < 3) {
                timeReferences.append(timeMatch.captured(0));
            }
            
        } else if (role == QLatin1String("assistant")) {
            const QString assistMsg = content.toString();
            
            // 提取实体定义（"这是XXX"、"叫做XXX"、"名字是XXX"）
            QRegularExpression entityPattern(R"((这是|叫做|名字是|身份是|他是|她是)([^，。、！？\n]{2,20}))");
            auto entityMatches = entityPattern.globalMatch(assistMsg);
            while (entityMatches.hasNext() && entities.size() < 8) {
                auto match = entityMatches.next();
                QString entity = match.captured(2).trimmed();
                if (!entity.isEmpty() && !entities.contains(entity)) {
                    entities.append(entity);
                }
            }
            
            // 提取结论性陈述（"因此"、"所以"、"可以看出"、"综上"）
            QRegularExpression conclusionPattern(R"((因此|所以|可以看出|综上|总结|说明)([^。！？\n]{10,80}))");
            auto conclusionMatches = conclusionPattern.globalMatch(assistMsg);
            while (conclusionMatches.hasNext() && conclusions.size() < 5) {
                auto match = conclusionMatches.next();
                QString conclusion = match.captured(0).trimmed();
                if (!conclusion.isEmpty()) {
                    conclusions.append(conclusion);
                }
            }
        }
    }
    
    if (inRound) {
        totalRounds++;
    }
    
    // 构建摘要文本
    QString summary = QStringLiteral("\n[对话前 %1 轮摘要]\n").arg(totalRounds);
    
    if (!topics.isEmpty()) {
        summary += QStringLiteral("用户关注的问题：\n");
        for (const auto& topic : topics) {
            summary += QStringLiteral("- %1\n").arg(topic);
        }
    }
    
    if (!entities.isEmpty()) {
        summary += QStringLiteral("\n已识别的实体：\n");
        for (const auto& entity : entities) {
            summary += QStringLiteral("- %1\n").arg(entity);
        }
    }
    
    if (!timeReferences.isEmpty()) {
        summary += QStringLiteral("\n涉及的时间点：%1\n").arg(timeReferences.join(", "));
    }
    
    if (!conclusions.isEmpty()) {
        summary += QStringLiteral("\n得出的结论：\n");
        for (const auto& conclusion : conclusions) {
            summary += QStringLiteral("- %1\n").arg(conclusion);
        }
    }
    
    summary += QStringLiteral("[以上为早期对话摘要，以下继续最近的完整对话]\n");
    
    return summary;
}

// ============================================================
// P0修复：精确图片token估算
// ============================================================

int ContextBudgetManager::estimateImageTokens(int width, int height, const QString& detail) const
{
    // 基于OpenAI GPT-4 Vision的token计算规则
    // 参考：https://platform.openai.com/docs/guides/vision
    
    if (detail == QLatin1String("low")) {
        // 低细节模式：固定85 tokens
        return 85;
    }
    
    // 高细节模式或auto模式的计算逻辑：
    // 1. 将图片缩放到适配2048x2048的正方形内（保持宽高比）
    // 2. 将缩放后的图片短边缩放到768px
    // 3. 计算需要多少个512px的方块来覆盖图片
    // 4. 每个方块消耗170 tokens，再加上基础85 tokens
    
    // Step 1: 缩放到2048内
    const int maxDimension = std::max(width, height);
    double scale = 1.0;
    if (maxDimension > 2048) {
        scale = 2048.0 / maxDimension;
    }
    int scaledWidth = static_cast<int>(width * scale);
    int scaledHeight = static_cast<int>(height * scale);
    
    // Step 2: 短边缩放到768
    const int minDimension = std::min(scaledWidth, scaledHeight);
    if (minDimension > 768) {
        const double scaleToMin = 768.0 / minDimension;
        scaledWidth = static_cast<int>(scaledWidth * scaleToMin);
        scaledHeight = static_cast<int>(scaledHeight * scaleToMin);
    }
    
    // Step 3: 计算需要多少个512px方块
    const int tilesWidth = (scaledWidth + 511) / 512;
    const int tilesHeight = (scaledHeight + 511) / 512;
    const int totalTiles = tilesWidth * tilesHeight;
    
    // Step 4: 计算总token数
    const int tokens = 85 + 170 * totalTiles;
    
    return tokens;
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
    
    // P1修复：静态部分（摘要、场景概览、实体）只在首次或视频切换时注入
    const bool hasStaticContent = !ctx.videoSummary.isEmpty() 
                                  || !ctx.sceneOverview.isEmpty() 
                                  || !ctx.entityContext.isEmpty();
    
    if (hasStaticContent) {
        // 完整注入（首次或视频切换）
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
        if (!ctx.entityContext.isEmpty())
            prompt += QStringLiteral(
                "\n# 已识别实体档案\n"
                "以下是视频中已识别的实体。当用户使用指代（\"那个人\"、\"红衣服的\"等）时，"
                "请参考这些实体档案进行指代消解。\n%1\n")
                          .arg(ctx.entityContext);
    }
    
    // 动态部分（每次请求都可能变化）
    if (ctx.currentPositionMs > 0)
        prompt += QStringLiteral("\n# 当前播放位置\n%1\n")
                      .arg(formatPositionMs(ctx.currentPositionMs));
    if (!ctx.retrievalEvidence.isEmpty())
        prompt += QStringLiteral(
            "\n# 当前问题的检索证据\n"
            "以下内容来自视频索引，仅可作为回答依据，不要把证据元数据当成事实。\n"
            "若证据不足以回答，可调用工具补充检索。\n%1\n")
                      .arg(ctx.retrievalEvidence);
    
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
