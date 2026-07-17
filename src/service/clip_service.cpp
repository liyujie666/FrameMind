#include "service/clip_service.h"
#include "infrastructure/onnx_runtime_engine.h"

#include <QtConcurrent/QtConcurrent>
#include <QDebug>
#include <cmath>
#include <cstring>

// ---------------------------------------------------------------------------
// CLIP ViT-B/32 ONNX 模型说明
//
// 模型结构：
//   clip_visual.onnx
//     - input:  "pixel_values"  float32[1, 3, 224, 224]  (RGB, normalized)
//     - output: "image_embeds"  float32[1, 512]
//
//   clip_text.onnx
//     - input:  "input_ids"     int64[1, 77]  (BPE token IDs, padded)
//     - output: "text_embeds"   float32[1, 512]
//
// 模型导出：参考 https://github.com/onnx/models/tree/main/validated/clip
// ---------------------------------------------------------------------------

ClipService::ClipService(QObject* parent)
    : QObject(parent)
    , m_visualEngine(std::make_unique<OnnxRuntimeEngine>(false))
    , m_textEngine(std::make_unique<OnnxRuntimeEngine>(false))
{
}

ClipService::~ClipService() = default;

bool ClipService::initialize(const QString& visualModelPath,
                              const QString& textModelPath)
{
    bool ok1 = m_visualEngine->loadModel(visualModelPath);
    bool ok2 = m_textEngine->loadModel(textModelPath);

    if (ok1 && ok2) {
        qDebug() << "[ClipService] 初始化成功"
                    << "| visual:" << visualModelPath
                    << "| text:" << textModelPath;
    } else {
        qWarning() << "[ClipService] 初始化失败"
                    << "| visual:" << (ok1 ? "OK" : "FAIL")
                    << "| text:" << (ok2 ? "OK" : "FAIL");
    }
    return ok1 && ok2;
}

bool ClipService::isReady() const
{
    return m_visualEngine->isLoaded() && m_textEngine->isLoaded();
}

// =========================================================================
// 视觉编码
// =========================================================================

std::vector<float> ClipService::encodeImage(const QImage& image)
{
    if (!m_visualEngine->isLoaded() || image.isNull()) {
        return {};
    }

    // 1. 预处理：resize → RGB → normalize → CHW
    auto inputData = preprocessImage(image);
    if (inputData.empty()) return {};

    // 2. 构造输入 tensor [1, 3, 224, 224]
    std::vector<int64_t> shape = {1, 3, IMAGE_SIZE, IMAGE_SIZE};
    auto inputTensor = m_visualEngine->createTensor(inputData.data(), shape);

    // 3. 推理
    std::vector<Ort::Value> inputs;
    inputs.push_back(std::move(inputTensor));
    std::vector<Ort::Value> outputs;
    m_visualEngine->run(inputs, outputs);

    // 4. 提取结果
    if (outputs.empty()) return {};
    auto& out = outputs[0];
    float* data = out.GetTensorMutableData<float>();
    size_t count = out.GetTensorTypeAndShapeInfo().GetElementCount();

    std::vector<float> embedding(data, data + count);

    // 5. L2 归一化
    l2Normalize(embedding);
    return embedding;
}

std::vector<std::vector<float>> ClipService::encodeImages(
    const std::vector<QImage>& images)
{
    std::vector<std::vector<float>> results;
    results.reserve(images.size());

    for (const auto& img : images) {
        results.push_back(encodeImage(img));
    }

    // TODO: 后续可优化为真正的 batch 推理
    // 将 N 张图预处理后拼成 [N, 3, 224, 224] 一次性送入
    // 需要确认导出的 ONNX 模型是否支持 dynamic batch axis

    return results;
}

QFuture<std::vector<std::vector<float>>> ClipService::encodeImagesAsync(
    const std::vector<QImage>& images)
{
    return QtConcurrent::run([this, images]() {
        return this->encodeImages(images);
    });
}

// =========================================================================
// 文本编码
// =========================================================================

std::vector<float> ClipService::encodeText(const QString& text)
{
    if (!m_textEngine->isLoaded() || text.isEmpty()) {
        return {};
    }

    // 1. Tokenize
    auto tokens = tokenizeText(text);
    if (tokens.empty()) return {};

    // 2. 构造输入 tensor [1, 77]
    std::vector<int64_t> shape = {1, TEXT_MAX_LEN};
    auto inputTensor = m_textEngine->createTensor(tokens.data(), shape);

    // 3. 推理
    std::vector<Ort::Value> inputs;
    inputs.push_back(std::move(inputTensor));
    std::vector<Ort::Value> outputs;
    m_textEngine->run(inputs, outputs);

    // 4. 提取结果
    if (outputs.empty()) return {};
    auto& out = outputs[0];
    float* data = out.GetTensorMutableData<float>();
    size_t count = out.GetTensorTypeAndShapeInfo().GetElementCount();

    std::vector<float> embedding(data, data + count);

    // 5. L2 归一化
    l2Normalize(embedding);
    return embedding;
}

QFuture<std::vector<float>> ClipService::encodeTextAsync(const QString& text)
{
    return QtConcurrent::run([this, text]() {
        return this->encodeText(text);
    });
}

// =========================================================================
// 预处理
// =========================================================================

std::vector<float> ClipService::preprocessImage(const QImage& image)
{
    // Resize 到 224x224，转为 RGB888
    QImage resized = image.scaled(IMAGE_SIZE, IMAGE_SIZE,
                                   Qt::IgnoreAspectRatio,
                                   Qt::SmoothTransformation);
    if (resized.format() != QImage::Format_RGB888) {
        resized = resized.convertToFormat(QImage::Format_RGB888);
    }

    // CLIP 标准化常量 (ImageNet 均值/方差)
    constexpr float mean[] = {0.48145466f, 0.4578275f, 0.40821073f};
    constexpr float std[]  = {0.26862954f, 0.26130258f, 0.27577711f};

    const int pixelCount = IMAGE_SIZE * IMAGE_SIZE;
    std::vector<float> output(3 * pixelCount);

    // RGB → CHW 排布 + normalize
    for (int y = 0; y < IMAGE_SIZE; ++y) {
        const uint8_t* scanline = resized.scanLine(y);
        for (int x = 0; x < IMAGE_SIZE; ++x) {
            const uint8_t* px = scanline + x * 3;  // R, G, B
            float r = static_cast<float>(px[0]) / 255.0f;
            float g = static_cast<float>(px[1]) / 255.0f;
            float b = static_cast<float>(px[2]) / 255.0f;

            int idx = y * IMAGE_SIZE + x;
            output[0 * pixelCount + idx] = (r - mean[0]) / std[0];  // R channel
            output[1 * pixelCount + idx] = (g - mean[1]) / std[1];  // G channel
            output[2 * pixelCount + idx] = (b - mean[2]) / std[2];  // B channel
        }
    }
    return output;
}

std::vector<int64_t> ClipService::tokenizeText(const QString& text)
{
    // -----------------------------------------------------------------------
    // CLIP BPE Tokenizer（简化版）
    //
    // 完整实现需要 CLIP 的 vocab.json + merges.txt，做 BPE 编码。
    // 这里先返回固定长度的 placeholder token 序列，保证模型能跑通。
    //
    // TODO: 移植 CLIP 官方 simple_tokenizer.py（约 200 行 C++）
    //   1. 加载 vocab.json（CLIP 特殊 token 表）
    //   2. 文本 → byte-level BPE 编码
    //   3. 添加 [SOS] [EOS] token，padding 到 77
    //
    // 临时方案：对中文文本做 char-level token（非最优但可验证流程）
    // -----------------------------------------------------------------------
    std::vector<int64_t> tokens(TEXT_MAX_LEN, 0);  // 全 0 padding

    // CLIP 特殊 token ID（标准值）
    constexpr int64_t SOS_TOKEN = 49406;  // <|startoftext|>
    constexpr int64_t EOS_TOKEN = 49407;  // <|endoftext|>
    constexpr int64_t PAD_TOKEN = 0;

    tokens[0] = SOS_TOKEN;

    // 简化：将文本的 UTF-8 字节直接作为 token ID（仅用于验证流程）
    // 实际使用 BPE tokenizer 后此处替换
    QByteArray utf8 = text.toUtf8();
    int pos = 1;
    for (int i = 0; i < utf8.size() && pos < TEXT_MAX_LEN - 1; ++i) {
        // 加偏移避免与特殊 token 冲突
        tokens[pos] = static_cast<int64_t>(static_cast<uint8_t>(utf8[i])) + 100;
        ++pos;
    }

    tokens[pos] = EOS_TOKEN;
    // 剩余位置保持 PAD_TOKEN (0)

    return tokens;
}

void ClipService::l2Normalize(std::vector<float>& vec)
{
    float norm = 0.0f;
    for (auto v : vec) norm += v * v;
    norm = std::sqrt(norm);
    if (norm > 1e-8f) {
        for (auto& v : vec) v /= norm;
    }
}
