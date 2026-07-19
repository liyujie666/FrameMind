#include "service/embedding_service.h"

#ifndef FRAMEMIND_HAS_ONNXRUNTIME

EmbeddingService::EmbeddingService(QObject* parent) : QObject(parent) {}
EmbeddingService::~EmbeddingService() = default;

#else

#include "infrastructure/onnx_runtime_engine.h"
#include "infrastructure/bert_tokenizer.h"

#include <QtConcurrent/QtConcurrent>
#include <QFileInfo>
#include <QDebug>
#include <cmath>

// ---------------------------------------------------------------------------
// BGE-small-zh-v1.5 ONNX 模型说明
//
// 模型结构（BERT-base 架构）：
//   bge-small-zh.onnx
//     - input_ids:      int64[1, 512]    (WordPiece token IDs)
//     - attention_mask: int64[1, 512]    (1=有效, 0=padding)
//     - token_type_ids: int64[1, 512]    (全 0，单句模式)
//     - output:         float32[1, 512]  (last_hidden_state[:, 0] pooled)
//
// 导出方式：使用 HuggingFace optimum-cli
//   optimum-cli export onnx --model BAAI/bge-small-zh-v1.5 bge-small-zh.onnx
//
// 注意：BGE 模型需要查询前缀 "为这个句子生成表示以用于检索相关文章："
//       在 embed() 中自动添加。
// ---------------------------------------------------------------------------

EmbeddingService::EmbeddingService(QObject* parent)
    : QObject(parent)
    , m_engine(std::make_unique<OnnxRuntimeEngine>(false))
    , m_tokenizer(std::make_unique<BertTokenizer>())
{
}

EmbeddingService::~EmbeddingService() = default;

bool EmbeddingService::initialize(const QString& modelPath)
{
    bool ok = m_engine->loadModel(modelPath);

    // 词表文件与模型同目录
    QString vocabPath = QFileInfo(modelPath).absolutePath()
                        + QStringLiteral("/vocab.txt");
    bool ok2 = m_tokenizer->load(vocabPath);

    if (ok && ok2) {
        qDebug() << "[EmbeddingService] 初始化成功:" << modelPath
                 << "| vocab:" << vocabPath;
    } else {
        qWarning() << "[EmbeddingService] 初始化失败:"
                   << "| model:" << (ok ? "OK" : "FAIL")
                   << "| vocab:" << (ok2 ? "OK" : "FAIL");
    }
    return ok;
}

bool EmbeddingService::isReady() const
{
    return m_engine->isLoaded();
}

std::vector<float> EmbeddingService::embed(const QString& text)
{
    if (!m_engine->isLoaded() || text.isEmpty()) {
        return {};
    }

    // BGE 检索时需要为查询添加前缀
    // 对于 passages（入库的文本）不需要前缀
    // 这里统一用查询前缀；入库时调用方可传 raw text
    QString prefixed = QStringLiteral("为这个句子生成表示以用于检索相关文章：") + text;

    // 1. Tokenize
    auto inputIds = tokenize(prefixed);
    if (inputIds.empty()) return {};

    auto attentionMask = buildAttentionMask(inputIds);
    std::vector<int64_t> tokenTypeIds(MAX_SEQ_LEN, 0);  // 单句模式全 0

    // 2. 构造输入 tensors
    std::vector<int64_t> shape = {1, MAX_SEQ_LEN};

    auto inputIdsTensor = m_engine->createTensor(inputIds.data(), shape);
    auto attnMaskTensor = m_engine->createTensor(attentionMask.data(), shape);
    auto tokenTypeTensor = m_engine->createTensor(tokenTypeIds.data(), shape);

    // 3. 推理
    std::vector<Ort::Value> inputs;
    inputs.push_back(std::move(inputIdsTensor));
    inputs.push_back(std::move(attnMaskTensor));
    inputs.push_back(std::move(tokenTypeTensor));

    std::vector<Ort::Value> outputs;
    m_engine->run(inputs, outputs);

    // 4. 提取结果（取 [CLS] token 的 pooled output）
    if (outputs.empty()) return {};
    auto& out = outputs[0];
    float* data = out.GetTensorMutableData<float>();
    size_t count = out.GetTensorTypeAndShapeInfo().GetElementCount();

    // BGE 输出可能是 [1, seq_len, hidden] 或 [1, hidden]
    // 如果是 [1, hidden] 直接用；如果是 [1, seq_len, hidden] 取第 0 个 token
    std::vector<float> embedding;
    if (count == EMBEDDING_DIM) {
        embedding.assign(data, data + count);
    } else {
        // 取 [CLS] (第一个 token) 的 hidden state
        embedding.assign(data, data + EMBEDDING_DIM);
    }

    // 5. L2 归一化
    l2Normalize(embedding);
    return embedding;
}

std::vector<std::vector<float>> EmbeddingService::embedBatch(
    const QVector<QString>& texts)
{
    std::vector<std::vector<float>> results;
    results.reserve(texts.size());

    for (const auto& text : texts) {
        results.push_back(embed(text));
    }

    // TODO: 后续可优化为真正的 batch 推理 [N, seq_len]
    return results;
}

QFuture<std::vector<std::vector<float>>> EmbeddingService::embedBatchAsync(
    const QVector<QString>& texts)
{
    return QtConcurrent::run([this, texts]() {
        return this->embedBatch(texts);
    });
}

// =========================================================================
// Tokenizer（简化版）
// =========================================================================

std::vector<int64_t> EmbeddingService::tokenize(const QString& text)
{
    if (m_tokenizer && m_tokenizer->isLoaded()) {
        return m_tokenizer->encode(text, MAX_SEQ_LEN);
    }

    // 降级：tokenizer 未加载时用字符级编码（质量差，但不崩溃）
    qWarning() << "[EmbeddingService] tokenizer 未加载，使用降级编码";
    std::vector<int64_t> tokens(MAX_SEQ_LEN, 0);
    constexpr int64_t CLS_TOKEN = 101;
    constexpr int64_t SEP_TOKEN = 102;

    tokens[0] = CLS_TOKEN;
    int pos = 1;
    for (const QChar& ch : text) {
        if (pos >= MAX_SEQ_LEN - 1) break;
        uint32_t cp = ch.unicode();
        if (cp < 0x4E00) {
            tokens[pos] = static_cast<int64_t>(cp);
        } else {
            tokens[pos] = static_cast<int64_t>(cp - 0x4E00 + 1000);
        }
        ++pos;
    }
    tokens[pos] = SEP_TOKEN;
    return tokens;
}

std::vector<int64_t> EmbeddingService::buildAttentionMask(
    const std::vector<int64_t>& inputIds)
{
    if (m_tokenizer && m_tokenizer->isLoaded()) {
        return m_tokenizer->attentionMask(inputIds);
    }
    std::vector<int64_t> mask(inputIds.size(), 0);
    for (size_t i = 0; i < inputIds.size(); ++i) {
        if (inputIds[i] != 0) mask[i] = 1;
    }
    return mask;
}

void EmbeddingService::l2Normalize(std::vector<float>& vec)
{
    float norm = 0.0f;
    for (auto v : vec) norm += v * v;
    norm = std::sqrt(norm);
    if (norm > 1e-8f) {
        for (auto& v : vec) v /= norm;
    }
}

#endif // FRAMEMIND_HAS_ONNXRUNTIME
