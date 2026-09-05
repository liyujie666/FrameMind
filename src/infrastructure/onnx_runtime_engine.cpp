#include "infrastructure/onnx_runtime_engine.h"

#include <QDebug>
#include <QFile>

// ---------------------------------------------------------------------------
// OnnxRuntimeEngine 实现
//
// 依赖：
//   third_party/onnxruntime/include/onnxruntime_cxx_api.h
//   third_party/onnxruntime/lib/onnxruntime.lib
//   third_party/onnxruntime/bin/onnxruntime.dll  (运行时)
//
// 下载地址：https://github.com/microsoft/onnxruntime/releases
// 选择 onnxruntime-win-x64-1.18.1.zip 解压到 third_party/onnxruntime/
// ---------------------------------------------------------------------------

OnnxRuntimeEngine::OnnxRuntimeEngine(bool useGpu)
    : m_env(ORT_LOGGING_LEVEL_WARNING, "FrameMind")
{
    m_sessionOptions = Ort::SessionOptions();
    m_sessionOptions.SetIntraOpNumThreads(4);
    m_sessionOptions.SetGraphOptimizationLevel(
        GraphOptimizationLevel::ORT_ENABLE_ALL);

    if (useGpu) {
        try {
            OrtCUDAProviderOptions cudaOptions;
            cudaOptions.device_id = 0;
            m_sessionOptions.AppendExecutionProvider_CUDA(cudaOptions);
            qDebug() << "[OnnxRuntime] CUDA EP enabled";
        } catch (const Ort::Exception&) {
            qWarning() << "[OnnxRuntime] CUDA EP 不可用，回退 CPU";
        }
    }
}

OnnxRuntimeEngine::~OnnxRuntimeEngine() = default;

bool OnnxRuntimeEngine::loadModel(const QString& modelPath)
{
    if (!QFile::exists(modelPath)) {
        qWarning() << "[OnnxRuntime] 模型文件不存在:" << modelPath;
        return false;
    }

    try {
#ifdef Q_OS_WIN
        m_session = std::make_unique<Ort::Session>(
            m_env, modelPath.toStdWString().c_str(), m_sessionOptions);
#else
        m_session = std::make_unique<Ort::Session>(
            m_env, modelPath.toStdString().c_str(), m_sessionOptions);
#endif
        refreshIoNames();
        qDebug() << "[OnnxRuntime] 模型加载成功:" << modelPath
                 << "| inputs:" << m_inputNames.size()
                 << "| outputs:" << m_outputNames.size();
        return true;
    } catch (const Ort::Exception& e) {
        qWarning() << "[OnnxRuntime] 模型加载失败:" << modelPath
                    << "error:" << e.what();
        return false;
    }
}

bool OnnxRuntimeEngine::isLoaded() const
{
    return m_session != nullptr;
}

void OnnxRuntimeEngine::refreshIoNames()
{
    m_inputNames.clear();
    m_outputNames.clear();
    m_inputNamePtrs.clear();
    m_outputNamePtrs.clear();

    size_t numInputs = m_session->GetInputCount();
    for (size_t i = 0; i < numInputs; ++i) {
        auto name = m_session->GetInputNameAllocated(i, m_allocator);
        m_inputNames.emplace_back(name.get());
    }

    size_t numOutputs = m_session->GetOutputCount();
    for (size_t i = 0; i < numOutputs; ++i) {
        auto name = m_session->GetOutputNameAllocated(i, m_allocator);
        m_outputNames.emplace_back(name.get());
    }

    // 缓存 C 字符串指针（Ort::Session::Run 需要 const char** ）
    for (const auto& n : m_inputNames)  m_inputNamePtrs.push_back(n.c_str());
    for (const auto& n : m_outputNames) m_outputNamePtrs.push_back(n.c_str());
}

std::vector<int64_t> OnnxRuntimeEngine::getInputShape(size_t index) const
{
    if (!m_session || index >= m_inputNames.size()) return {};
    auto shapeInfo = m_session->GetInputTypeInfo(index)
                         .GetTensorTypeAndShapeInfo();
    return shapeInfo.GetShape();
}

std::vector<int64_t> OnnxRuntimeEngine::getOutputShape(size_t index) const
{
    if (!m_session || index >= m_outputNames.size()) return {};
    auto shapeInfo = m_session->GetOutputTypeInfo(index)
                         .GetTensorTypeAndShapeInfo();
    return shapeInfo.GetShape();
}

Ort::Value OnnxRuntimeEngine::createTensor(float* data,
                                            const std::vector<int64_t>& shape)
{
    auto memInfo = Ort::MemoryInfo::CreateCpu(
        OrtArenaAllocator, OrtMemTypeDefault);

    int64_t count = 1;
    for (auto s : shape) count *= s;

    return Ort::Value::CreateTensor<float>(
        memInfo, data, static_cast<size_t>(count),
        shape.data(), shape.size());
}

Ort::Value OnnxRuntimeEngine::createTensor(int64_t* data,
                                            const std::vector<int64_t>& shape)
{
    auto memInfo = Ort::MemoryInfo::CreateCpu(
        OrtArenaAllocator, OrtMemTypeDefault);

    int64_t count = 1;
    for (auto s : shape) count *= s;

    return Ort::Value::CreateTensor<int64_t>(
        memInfo, data, static_cast<size_t>(count),
        shape.data(), shape.size());
}

void OnnxRuntimeEngine::run(const std::vector<Ort::Value>& inputs,
                             std::vector<Ort::Value>& outputs)
{
    if (!m_session || inputs.empty()) {
        qWarning() << "[OnnxRuntime] run() called but session not loaded"
                    << "or no inputs";
        return;
    }

    if (inputs.size() != m_inputNames.size()) {
        qWarning() << "[OnnxRuntime] 输入数量与模型不匹配"
                   << "provided=" << inputs.size()
                   << "expected=" << m_inputNames.size();
        for (const auto& name : m_inputNames) {
            qWarning() << "[OnnxRuntime] expected input:"
                       << QString::fromStdString(name);
        }
        return;
    }

    try {
        auto result = m_session->Run(
            Ort::RunOptions{nullptr},
            m_inputNamePtrs.data(),
            inputs.data(), inputs.size(),
            m_outputNamePtrs.data(),
            m_outputNamePtrs.size());

        outputs.clear();
        outputs.reserve(result.size());
        for (auto& v : result) {
            outputs.push_back(std::move(v));
        }
    } catch (const Ort::Exception& e) {
        qWarning() << "[OnnxRuntime] 推理失败:" << e.what();
    }
}
