#ifndef FRAMEMIND_EMBEDDING_SERVICE_H
#define FRAMEMIND_EMBEDDING_SERVICE_H

#include <QObject>
#include <QString>
#include <QVector>
#include <QFuture>
#include <vector>
#include <memory>

class OnnxRuntimeEngine;

/**
 * BGE-small-zh-v1.5 文本 Embedding 服务。
 *
 * 职责：
 *   - embed(): 单条文本 → 512 维向量
 *   - embedBatch(): 批量文本 → 批量向量
 *
 * 与 CLIP 的文本编码器互补：
 *   - CLIP text encoder：偏视觉语义（"红衣服的人" → 找画面中匹配的人）
 *   - BGE：偏自然语言语义（转写文本、场景描述的精确语义搜索）
 *
 * 模型：BAAI/bge-small-zh-v1.5 ONNX 导出 (~100MB)
 * 下载：HuggingFace BAAI/bge-small-zh-v1.5
 */
class EmbeddingService : public QObject {
    Q_OBJECT
public:
    explicit EmbeddingService(QObject* parent = nullptr);
    ~EmbeddingService() override;

    EmbeddingService(const EmbeddingService&) = delete;
    EmbeddingService& operator=(const EmbeddingService&) = delete;

    /// 加载 BGE-small ONNX 模型
    /// @param modelPath bge-small-zh.onnx 路径
    bool initialize(const QString& modelPath);

    /// 是否已初始化
    bool isReady() const;

    /// 单条文本 → 512 维 embedding（已 L2 归一化）
    std::vector<float> embed(const QString& text);

    /// 批量文本 → 批量 embedding
    std::vector<std::vector<float>> embedBatch(const QVector<QString>& texts);

    /// 异步批量编码
    QFuture<std::vector<std::vector<float>>> embedBatchAsync(
        const QVector<QString>& texts);

    // ---- 常量 ----
    static constexpr int EMBEDDING_DIM = 512;
    static constexpr int MAX_SEQ_LEN   = 512;   // BGE 最大 token 长度

private:
    /// BGE (BERT-based) WordPiece tokenizer
    /// TODO: 接入完整的 BERT WordPiece tokenizer
    std::vector<int64_t> tokenize(const QString& text);

    /// 构建 attention mask（有效 token 位置为 1，padding 为 0）
    std::vector<int64_t> buildAttentionMask(const std::vector<int64_t>& inputIds);

    /// L2 归一化
    static void l2Normalize(std::vector<float>& vec);

    std::unique_ptr<OnnxRuntimeEngine> m_engine;
};

#endif // FRAMEMIND_EMBEDDING_SERVICE_H
