#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

/**
 * @brief 上下文预算管理器
 *
 * 解决的核心问题：
 *   - 对话历史无限增长导致超出模型 Context Window
 *   - Tool 结果体积过大导致历史膨胀
 *   - System Prompt 静态/动态未分离导致无法利用 Prompt Caching
 *
 * 策略：
 *   1. Token 预算计算：model_max - system - current_user - reserved_output = history_budget
 *   2. 历史截断：保留最近 N 轮 + 对中间轮次可选摘要压缩
 *   3. Tool 结果摘要：超过阈值的 tool 结果截断保留头尾 + 摘要标记
 *   4. System Prompt 分层：静态规则（可缓存）与动态背景（每次变化）分离
 */
class ContextBudgetManager
{
public:
    struct Config {
        int modelMaxTokens = 128000;       ///< 模型最大上下文窗口
        int reservedOutputTokens = 4096;   ///< 保留给输出的 token 预算
        int minHistoryRounds = 2;          ///< 最少保留的对话轮次
        int maxHistoryRounds = 20;         ///< 最多保留的对话轮次
        int toolResultMaxChars = 1500;     ///< 单个 tool 结果的最大字符数
        int toolResultSummaryChars = 300;  ///< 截断后保留的头尾字符
    };

    explicit ContextBudgetManager(const Config& config = {});

    void setConfig(const Config& config) { m_config = config; }
    Config config() const { return m_config; }

    // ─── Token 估算 ───────────────────────────────────────────

    /// 估算 JSON message 数组的 token 数（粗略：中文 1 字 ≈ 1.5 token，英文 1 word ≈ 1.3 token）
    int estimateTokens(const QJsonArray& messages) const;

    /// 估算单条消息的 token 数
    int estimateMessageTokens(const QJsonObject& message) const;

    /// 估算纯文本的 token 数
    int estimateTextTokens(const QString& text) const;

    // ─── 历史截断 ───────────────────────────────────────────────

    /**
     * 对对话历史进行截断，使其满足 token 预算。
     *
     * 策略（按优先级）：
     *   1. 超过 maxHistoryRounds 的轮次直接丢弃最早的
     *   2. 丢弃早期 tool 结果（只保留 assistant 文本结论）
     *   3. 从最早轮次开始逐步丢弃，直到满足预算
     *   4. 保证最少 minHistoryRounds 轮
     *
     * @param history 原始历史（会被就地修改）
     * @param systemTokens system prompt 已占用的 token 数
     * @param currentUserTokens 当前 user 消息的 token 数
     * @return 截断后的历史实际 token 数
     */
    int truncateHistory(QJsonArray& history,
                        int systemTokens,
                        int currentUserTokens) const;

    // ─── Tool 结果摘要 ─────────────────────────────────────────

    /**
     * 对 Tool 结果消息列表进行体积压缩。
     * 超过 toolResultMaxChars 的 content 会被截断为头尾摘要。
     *
     * @param toolMessages role=tool 的消息数组（会被就地修改）
     * @return 压缩后的总字符数
     */
    int compressToolResults(QJsonArray& toolMessages) const;

    /// 压缩单条 tool 结果的 content
    QString compressToolContent(const QString& content) const;

    // ─── System Prompt 分层 ────────────────────────────────────

    /// 生成静态 System Prompt（角色/规则/格式，可缓存）
    static QString buildStaticSystemPrompt();

    /// 生成动态 System Prompt（视频背景/检索证据，每次变化）
    static QString buildDynamicSystemPrompt(const struct VideoContext& ctx);

    /**
     * 组装分层 System Prompt 消息数组。
     * 返回 2 条 system 消息：[0]=静态（可缓存），[1]=动态。
     * 若后端不支持多条 system，可通过 mergeSystemPrompts() 合并。
     */
    static QJsonArray buildLayeredSystemMessages(const struct VideoContext& ctx);

    /// 合并为单条 system（兼容不支持多条 system 的后端）
    static QJsonObject buildMergedSystemMessage(const struct VideoContext& ctx);

private:
    Config m_config;

    /// 从历史中找到轮次边界（user 消息为轮次开始）
    QVector<QPair<int, int>> findRoundBoundaries(const QJsonArray& history) const;
};
