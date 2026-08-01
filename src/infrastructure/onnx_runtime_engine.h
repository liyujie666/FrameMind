#ifndef FRAMEMIND_ONNX_RUNTIME_ENGINE_H
#define FRAMEMIND_ONNX_RUNTIME_ENGINE_H

#include <QString>
#include <vector>
#include <memory>
#include <cstdint>

// ONNX Runtime C++ API
// 安装方式：将 onnxruntime-win-x64-1.18.1 解压到 third_party/onnxruntime/
// CMake 通过 find_package 或手动 IMPORTED target 引入
#include <onnxruntime_cxx_api.h>

/**
 * ONNX Runtime 统一推理引擎。
 *
 * 封装 Ort::Session 生命周期、内存分配器、tensor 构造与推理调用。
 * CLIP / BGE / TransNetV2 三个模型各自创建独立的 OnnxRuntimeEngine 实例
 * （各自加载不同的 .onnx 文件，互不干扰）。
 *
 * 典型用法：
 *   OnnxRuntimeEngine engine(false);  // CPU
 *   engine.loadModel("clip_visual.onnx");
 *   auto tensor = engine.createTensor(data, {1, 3, 224, 224});
 *   std::vector<Ort::Value> inputs; inputs.push_back(std::move(tensor));
 *   std::vector<Ort::Value> outputs;
 *   engine.run(inputs, outputs);
 *   float* result = outputs[0].GetTensorMutableData<float>();
 */
class OnnxRuntimeEngine {
public:
    /// @param useGpu 是否启用 CUDA EP（不可用时自动回退 CPU）
    explicit OnnxRuntimeEngine(bool useGpu = false);
    ~OnnxRuntimeEngine();

    OnnxRuntimeEngine(const OnnxRuntimeEngine&) = delete;
    OnnxRuntimeEngine& operator=(const OnnxRuntimeEngine&) = delete;

    /// 加载 ONNX 模型文件
    /// @return 成功返回 true
    bool loadModel(const QString& modelPath);

    /// 是否已加载模型
    bool isLoaded() const;

    /// 获取模型输入名称列表
    const std::vector<std::string>& inputNames() const { return m_inputNames; }

    /// 获取模型输出名称列表
    const std::vector<std::string>& outputNames() const { return m_outputNames; }

    /// 获取输入张量的形状 [name → shape]
    std::vector<int64_t> getInputShape(size_t index) const;

    /// 获取输出张量的形状 [index → shape]
    std::vector<int64_t> getOutputShape(size_t index) const;

    /// 创建 float32 张量（data 生命周期由调用方管理，需在 run() 前保持有效）
    Ort::Value createTensor(float* data, const std::vector<int64_t>& shape);

    /// 创建 int64 张量
    Ort::Value createTensor(int64_t* data, const std::vector<int64_t>& shape);

    /// 执行推理
    /// @param inputs  输入张量列表（顺序与 inputNames 对应）
    /// @param outputs [out] 输出张量列表
    void run(const std::vector<Ort::Value>& inputs,
             std::vector<Ort::Value>& outputs);

private:
    Ort::Env                            m_env;
    Ort::SessionOptions                 m_sessionOptions;
    std::unique_ptr<Ort::Session>      m_session;
    Ort::AllocatorWithDefaultOptions   m_allocator;

    std::vector<std::string>            m_inputNames;
    std::vector<std::string>            m_outputNames;

    /// 缓存输入/输出名称的 C 字符串指针（run 时需要）
    std::vector<const char*>            m_inputNamePtrs;
    std::vector<const char*>            m_outputNamePtrs;

    void refreshIoNames();
};

#endif // FRAMEMIND_ONNX_RUNTIME_ENGINE_H
