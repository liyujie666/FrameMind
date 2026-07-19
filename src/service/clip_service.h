#ifndef FRAMEMIND_CLIP_SERVICE_H
#define FRAMEMIND_CLIP_SERVICE_H

#include <QObject>
#include <QImage>
#include <QString>
#include <QFuture>
#include <vector>
#include <memory>

#ifdef FRAMEMIND_HAS_ONNXRUNTIME
class OnnxRuntimeEngine;
class ClipTokenizer;
#endif

/**
 * CLIP ViT-B/32 视觉 & 文本 Embedding 服务。
 *
 * 职责：
 *   - encodeImage(): QImage → 512 维视觉向量（用于索引阶段编码关键帧）
 *   - encodeText():  QString → 512 维文本向量（用于检索阶段编码用户查询）
 *
 * 两个编码器各自加载独立的 ONNX 模型文件：
 *   - clip_visual.onnx  (ViT 视觉编码器, ~350MB)
 *   - clip_text.onnx    (Transformer 文本编码器)
 *
 * 注意：实际功能仅在 FRAMEMIND_HAS_ONNXRUNTIME 宏定义时可用。
 */
class ClipService : public QObject {
    Q_OBJECT
public:
    explicit ClipService(QObject* parent = nullptr);
    ~ClipService() override;

    ClipService(const ClipService&) = delete;
    ClipService& operator=(const ClipService&) = delete;

#ifdef FRAMEMIND_HAS_ONNXRUNTIME

    /// 初始化：加载视觉和文本两个 ONNX 模型
    /// @param visualModelPath clip_visual.onnx 路径
    /// @param textModelPath   clip_text.onnx 路径
    /// @return 两个都加载成功才返回 true
    bool initialize(const QString& visualModelPath,
                    const QString& textModelPath);

    /// 是否已初始化
    bool isReady() const;

    // ---- 视觉编码 ----

    /// 单张图片 → 512 维视觉 embedding（已 L2 归一化）
    std::vector<float> encodeImage(const QImage& image);

    /// 批量图片 → 批量 embedding
    std::vector<std::vector<float>> encodeImages(const std::vector<QImage>& images);

    /// 异步批量编码
    QFuture<std::vector<std::vector<float>>> encodeImagesAsync(
        const std::vector<QImage>& images);

    // ---- 文本编码 ----

    /// 文本 → 512 维文本 embedding（已 L2 归一化）
    std::vector<float> encodeText(const QString& text);

    /// 异步文本编码
    QFuture<std::vector<float>> encodeTextAsync(const QString& text);

    // ---- 常量 ----
    static constexpr int EMBEDDING_DIM = 512;
    static constexpr int IMAGE_SIZE    = 224;
    static constexpr int TEXT_MAX_LEN  = 77;

private:
    /// 图像预处理：resize 224x224 → RGB → normalize → CHW 排布
    std::vector<float> preprocessImage(const QImage& image);

    /// CLIP BPE 文本 tokenize
    std::vector<int64_t> tokenizeText(const QString& text);

    /// L2 归一化
    static void l2Normalize(std::vector<float>& vec);

    std::unique_ptr<OnnxRuntimeEngine> m_visualEngine;
    std::unique_ptr<OnnxRuntimeEngine> m_textEngine;
    std::unique_ptr<ClipTokenizer>     m_tokenizer;
#endif // FRAMEMIND_HAS_ONNXRUNTIME
};

#endif // FRAMEMIND_CLIP_SERVICE_H
