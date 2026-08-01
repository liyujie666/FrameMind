#ifndef FRAMEMIND_CLIP_TOKENIZER_H
#define FRAMEMIND_CLIP_TOKENIZER_H

#include <QString>
#include <QHash>
#include <QVector>
#include <QPair>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

#ifdef FRAMEMIND_HAS_ONNXRUNTIME

/**
 * CLIP Byte-level BPE Tokenizer (C++ 移植版)。
 *
 * 对应 OpenAI CLIP 的 simple_tokenizer.py：
 *   - byte-level BPE，词表 49152 tokens
 *   - 特殊 token: <|startoftext|> (49406), <|endoftext|> (49407)
 *   - 最大序列长度 77（含 SOS/EOS）
 *
 * 词表文件：bpe_simple_vocab_16e6.txt
 *   下载：https://github.com/openai/CLIP/blob/main/clip/bpe_simple_vocab_16e6.txt.gz
 *   放置于 models/ 目录（与 ONNX 模型同级）
 */
class ClipTokenizer {
public:
    ClipTokenizer();

    /// 加载 BPE 词表文件
    /// mergesPath: clip_merges.txt（merge rules，格式同 bpe_simple_vocab_16e6.txt）
    /// vocabJsonPath: clip_vocab.json（token→id 映射，可选；为空时自动重建）
    bool load(const QString& mergesPath, const QString& vocabJsonPath = QString());

    /// 是否已加载
    bool isLoaded() const { return m_loaded; }

    /// 编码文本 → token IDs（含 SOS/EOS，padding 到 maxLen）
    std::vector<int64_t> encode(const QString& text, int maxLen = 77) const;

    static constexpr int64_t SOS_TOKEN = 49406;
    static constexpr int64_t EOS_TOKEN = 49407;

private:
    using BPEPair = QPair<std::string, std::string>;

    /// 文本预处理：小写 + 分词（按空白和标点拆分）
    std::vector<std::string> preTokenize(const QString& text) const;

    /// 对单个词执行 BPE 编码
    std::vector<std::string> bpe(const std::string& token) const;

    /// byte → unicode 映射（CLIP 特有的 byte-level 编码表）
    void buildByteEncoder();

    std::unordered_map<std::string, int64_t> m_encoder;   // token string → id
    std::unordered_map<std::string, int>     m_bpeRanks;  // "a b" → merge priority
    std::unordered_map<uint8_t, char32_t>    m_byteToUnicode;
    std::unordered_map<char32_t, uint8_t>    m_unicodeToByte;

    bool m_loaded = false;
};

#endif // FRAMEMIND_HAS_ONNXRUNTIME
#endif // FRAMEMIND_CLIP_TOKENIZER_H
