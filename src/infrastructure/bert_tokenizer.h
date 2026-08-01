#ifndef FRAMEMIND_BERT_TOKENIZER_H
#define FRAMEMIND_BERT_TOKENIZER_H

#include <QString>
#include <QHash>
#include <QVector>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

#ifdef FRAMEMIND_HAS_ONNXRUNTIME

/**
 * BERT WordPiece Tokenizer (C++ 实现)。
 *
 * 用于 BGE-small-zh-v1.5 等 BERT 系模型：
 *   - 加载 vocab.txt（每行一个 token，行号即 token ID）
 *   - BasicTokenizer: unicode 清洗 + CJK 逐字符拆分 + 小写
 *   - WordPieceTokenizer: 贪心最长前缀匹配，未知词拆为 [UNK]
 *   - 特殊 token: [PAD]=0, [UNK]=100, [CLS]=101, [SEP]=102
 *
 * 词表文件：vocab.txt
 *   来源：HuggingFace BAAI/bge-small-zh-v1.5 模型目录
 *   放置于 models/ 目录
 */
class BertTokenizer {
public:
    BertTokenizer();

    /// 加载 vocab.txt
    bool load(const QString& vocabPath);

    /// 是否已加载
    bool isLoaded() const { return m_loaded; }

    /// 编码文本 → token IDs（含 [CLS]/[SEP]，padding 到 maxLen）
    std::vector<int64_t> encode(const QString& text, int maxLen = 512) const;

    /// 生成 attention mask
    std::vector<int64_t> attentionMask(const std::vector<int64_t>& inputIds) const;

    static constexpr int64_t PAD_TOKEN = 0;
    static constexpr int64_t UNK_TOKEN = 100;
    static constexpr int64_t CLS_TOKEN = 101;
    static constexpr int64_t SEP_TOKEN = 102;

private:
    /// BasicTokenizer: unicode 清洗 + CJK 拆字 + 小写 + 标点拆分
    QStringList basicTokenize(const QString& text) const;

    /// WordPiece: 贪心最长匹配
    std::vector<int64_t> wordPieceTokenize(const QString& token) const;

    /// CJK 统一汉字范围判断
    static bool isCjkChar(QChar ch);

    /// 标点/空白判断
    static bool isPunctuation(QChar ch);

    std::unordered_map<std::string, int64_t> m_vocab;  // token → id
    int m_vocabSize = 0;
    bool m_loaded = false;
};

#endif // FRAMEMIND_HAS_ONNXRUNTIME
#endif // FRAMEMIND_BERT_TOKENIZER_H
