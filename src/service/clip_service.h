#ifndef FRAMEMIND_CLIP_SERVICE_H
#define FRAMEMIND_CLIP_SERVICE_H

#include <QObject>
#include <QImage>
#include <QString>
#include <QFuture>
#include <vector>
#include <memory>

class OnnxRuntimeEngine;

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
 * 模型下载：HuggingFace openai/clip-vit-base-patch32 社区 ONNX 导出
 */
class ClipService : public QObject {
    Q_OBJECT
public:
    explicit ClipService(QObject* parent = nullptr);
    ~ClipService() override;

    ClipService(const ClipService&) = delete;
    ClipService& operator=(const ClipService&) = delete;

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
    static constexpr int IMAGE_SIZE    = 224;   // CLIP 输入分辨率
    static constexpr int TEXT_MAX_LEN  = 77;    // CLIP 最大 token 长度

private:
    /// 图像预处理：resize 224x224 → RGB → normalize → CHW 排布
    std::vector<float> preprocessImage(const QImage& image);

    /// CLIP BPE 文本 tokenize（简化实现，返回 token IDs）
    /// TODO: 接入完整的 CLIP BPE tokenizer（可移植 Python 版 clip/simple_tokenizer.py）
    std::vector<int64_t> tokenizeText(const QString& text);

    /// L2 归一化
    static void l2Normalize(std::vector<float>& vec);

    std::unique_ptr<OnnxRuntimeEngine> m_visualEngine;
    std::unique_ptr<OnnxRuntimeEngine> m_textEngine;
};

#endif // FRAMEMIND_CLIP_SERVICE_H
