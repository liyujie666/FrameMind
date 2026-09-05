#include "app/dicontainer.h"

#include "infrastructure/eventbus.h"
#include "infrastructure/databasemanager.h"
#include "infrastructure/networkclient.h"
#include "service/settingsservice.h"
#include "service/themeservice.h"
#include "service/playerservice.h"
#include "service/agentservice.h"
#include "service/llmproviderservice.h"
#include "service/conversationservice.h"
#include "service/filemanagerservice.h"
#include "service/scene_detector.h"
#ifdef FRAMEMIND_HAS_ONNXRUNTIME
#include "service/clip_service.h"
#include "service/embedding_service.h"
#endif
#ifdef FRAMEMIND_HAS_WHISPER
#include "service/whisper_service.h"
#endif

// Video Agent 组件（M4）
#include "service/rag/video_rag_store.h"
#include "service/rag/qa_cache_manager.h"
#include "service/rag/video_rag_retriever.h"
#include "service/rag/entity_tracker.h"
#include "service/rag/audio_visual_aligner.h"
#include "service/agent/video_indexer.h"
#include "service/agent/one_shot_vlm_channel.h"
#include "service/agent/video_analysis_service.h"
#include "service/agent/perception_strategy.h"
#include "service/agent/reflection_engine.h"
#include "service/agent/tool_registry.h"
#include "service/agent/tool_orchestrator.h"
#include "service/agent/video_agent.h"
#include "service/agent/tools/seek_and_analyze_tool.h"
#include "service/agent/tools/analyze_time_range_tool.h"
#include "service/agent/tools/search_video_content_tool.h"
#include "service/agent/tools/get_transcript_tool.h"
#include "service/agent/tools/get_scene_info_tool.h"
#include "service/agent/tools/control_player_tool.h"

// Workflow Engine
#include "service/agent/workflow/workflow_executor.h"
#include "service/agent/workflow/workflow_factory.h"
#include "service/agent/workflow/workflow_checkpoint.h"
#include "service/agent/workflow/workflow_state.h"
#include "model/videocontext.h"
#include "model/agent_types.h"

#include "viewmodel/playerviewmodel.h"
#include "viewmodel/chatviewmodel.h"
#include "viewmodel/filelistviewmodel.h"
#include "viewmodel/videoanalysisviewmodel.h"
#include "viewmodel/knowledgeviewmodel.h"

#include <QStandardPaths>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

namespace {

/**
 * 解析模型权重目录，按优先级查找：
 *   1. 环境变量 FRAMEMIND_MODELS_DIR（最高优先级，便于测试/CI 覆盖）
 *   2. 可执行文件同级 ./models/    （发布版部署形态）
 *   3. 项目根 ./models/            （开发期：exe 在 build/Debug/，项目根在 ../../）
 *
 * 任何一级命中且目录存在即返回；找不到时返回兜底路径并创建。
 */
QString resolveModelsDir()
{
    const QStringList candidates = []() {
        QStringList list;
        // 1. 环境变量
        if (qEnvironmentVariableIsSet("FRAMEMIND_MODELS_DIR")) {
            list << QString::fromUtf8(qgetenv("FRAMEMIND_MODELS_DIR"));
        }
        // 2. 可执行文件同级 ./models/
        const QString exeDir = QCoreApplication::applicationDirPath();
        list << (exeDir + QStringLiteral("/models"));
        // 3. 项目根 ./models/（开发期：build/Debug/../../models）
        list << (exeDir + QStringLiteral("/../../models"));
        return list;
    }();

    for (const QString& path : candidates) {
        if (path.isEmpty()) continue;
        if (QDir(path).exists()) {
            return QDir(path).absolutePath();
        }
    }

    // 全部不存在 → 创建兜底目录并返回
    const QString fallback = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                             + QStringLiteral("/models");
    QDir().mkpath(fallback);
    return QDir(fallback).absolutePath();
}

} // namespace
#include <QDir>

DIContainer::DIContainer() = default;
DIContainer::~DIContainer() = default;

// Video Agent 组件 getter（延后定义以确保完整类型可见）
VideoRAGStore*          DIContainer::ragStore() const              { return m_ragStore.get(); }
QACacheManager*         DIContainer::qaCache() const               { return m_qaCache.get(); }
VideoRAGRetriever*      DIContainer::ragRetriever() const          { return m_ragRetriever.get(); }
EntityTracker*          DIContainer::entityTracker() const         { return m_entityTracker.get(); }
AudioVisualAligner*     DIContainer::audioVisualAligner() const    { return m_avAligner.get(); }
VideoIndexer*           DIContainer::videoIndexer() const          { return m_videoIndexer.get(); }
VideoAnalysisService*   DIContainer::videoAnalysisService() const  { return m_videoAnalysis.get(); }
PerceptionStrategy*     DIContainer::perceptionStrategy() const    { return m_perception.get(); }
ReflectionEngine*       DIContainer::reflectionEngine() const      { return m_reflection.get(); }
ToolRegistry*           DIContainer::toolRegistry() const          { return m_toolRegistry.get(); }
ToolOrchestrator*       DIContainer::toolOrchestrator() const      { return m_toolOrchestrator.get(); }
VideoAgent*DIContainer::videoAgent() const            { return m_videoAgent.get(); }
VideoAnalysisViewModel* DIContainer::videoAnalysisVM() const       { return m_videoAnalysisVM.get(); }
KnowledgeViewModel*     DIContainer::knowledgeVM() const           { return m_knowledgeVM.get(); }
WorkflowExecutor*       DIContainer::workflowExecutor() const      { return m_workflowExecutor.get(); }
WorkflowFactory*        DIContainer::workflowFactory() const       { return m_workflowFactory.get(); }
WorkflowCheckpoint*     DIContainer::workflowCheckpoint() const    { return m_workflowCheckpoint.get(); }

void DIContainer::initialize()
{
    // Infrastructure
    m_eventBus = EventBus::instance();

    const QString appData =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(appData);
    m_db = DatabaseManager::instance();
    m_db->initialize(appData + QStringLiteral("/agent.db"));

    m_network = std::make_unique<NetworkClient>();

    // Services
    m_settingsService = std::make_unique<SettingsService>(m_db);
    m_themeService    = std::make_unique<ThemeService>(m_settingsService.get());
    m_playerService   = std::make_unique<PlayerService>();
    m_providerService = std::make_unique<LLMProviderService>(m_settingsService.get());
    m_providerService->setNetworkClient(m_network.get());
    m_agentService   = std::make_unique<AgentService>(m_network.get(),
                                                      m_settingsService.get(),
                                                      m_providerService.get());
    // 后台/局部视觉分析使用专属网络与 AgentService，避免抢占用户问答 SSE 状态。
    m_vlmNetwork = std::make_unique<NetworkClient>();
    m_vlmAgentService = std::make_unique<AgentService>(m_vlmNetwork.get(),
                                                       m_settingsService.get(),
                                                       m_providerService.get());
    m_oneShotVlmChannel = std::make_unique<OneShotVlmChannel>(
        m_vlmAgentService.get());
    m_convService    = std::make_unique<ConversationService>(m_db);
    m_fileService    = std::make_unique<FileManagerService>(m_db);

    // ---- Video RAG 小模型服务 ----
    // 模型目录优先级：环境变量 > exe 同级 ./models/ > 项目根 ./models/ > <AppData>/models/
    // 详见匿名命名空间里的 resolveModelsDir()
    const QString modelsDir = resolveModelsDir();
    QDir().mkpath(modelsDir);
    qDebug() << "[DIContainer] modelsDir =" << modelsDir;

    m_sceneDetector = std::make_unique<SceneDetector>();
#ifdef FRAMEMIND_HAS_ONNXRUNTIME
    m_sceneDetector->loadTransNetV2(modelsDir + QStringLiteral("/transnetv2.onnx"));
#endif

#ifdef FRAMEMIND_HAS_ONNXRUNTIME
    m_clipService = std::make_unique<ClipService>();
    m_clipService->initialize(
        modelsDir + QStringLiteral("/clip_visual.onnx"),
        modelsDir + QStringLiteral("/clip_text.onnx"));

    m_embeddingService = std::make_unique<EmbeddingService>();
    m_embeddingService->initialize(
        modelsDir + QStringLiteral("/bge-small-zh.onnx"));
#endif

#ifdef FRAMEMIND_HAS_WHISPER
    m_whisperService = std::make_unique<WhisperService>();
    m_whisperService->initialize(
        modelsDir + QStringLiteral("/ggml-medium.bin"));
#endif

    // ---- Video Agent 装配（M4）----

    // 1. RAG 存储层
    m_ragStore = std::make_unique<VideoRAGStore>(m_db);
    m_ragStore->initialize();

    // 2. QA 缓存
    m_qaCache = std::make_unique<QACacheManager>(m_ragStore.get(),
#ifdef FRAMEMIND_HAS_ONNXRUNTIME
                                                  m_embeddingService.get()
#else
                                                  nullptr
#endif
                                                  );

    // 3. 检索器
    m_ragRetriever = std::make_unique<VideoRAGRetriever>(m_ragStore.get());
#ifdef FRAMEMIND_HAS_ONNXRUNTIME
    m_ragRetriever->setClipService(m_clipService.get());
    m_ragRetriever->setEmbeddingService(m_embeddingService.get());
#endif

    // 4. 实体追踪
    m_entityTracker = std::make_unique<EntityTracker>(m_ragStore.get(),
#ifdef FRAMEMIND_HAS_ONNXRUNTIME
                                                       m_embeddingService.get()
#else
                                                       nullptr
#endif
                                                       );

    // 5. 索引器
    m_videoIndexer = std::make_unique<VideoIndexer>(m_playerService.get(),
                                                     m_sceneDetector.get(),
                                                     m_ragStore.get(),
                                                     m_db);
#ifdef FRAMEMIND_HAS_ONNXRUNTIME
    m_videoIndexer->setClipService(m_clipService.get());
    m_videoIndexer->setEmbeddingService(m_embeddingService.get());
#endif
#ifdef FRAMEMIND_HAS_WHISPER
    m_videoIndexer->setWhisperService(m_whisperService.get());
#endif

    // 6. 音画对齐 + 语义门控（音视频融合第一期）
    m_avAligner = std::make_unique<AudioVisualAligner>();
#ifdef FRAMEMIND_HAS_ONNXRUNTIME
    m_avAligner->setEmbeddingService(m_embeddingService.get());
#endif

    // 7. 分析服务
    m_videoAnalysis = std::make_unique<VideoAnalysisService>(
        m_oneShotVlmChannel.get(), m_videoIndexer.get(),
        m_ragStore.get(), m_playerService.get(), m_db);
    m_videoAnalysis->setAudioVisualAligner(m_avAligner.get());
#ifdef FRAMEMIND_HAS_ONNXRUNTIME
    m_videoAnalysis->setEmbeddingService(m_embeddingService.get());
#endif

    // 8. 决策组件
    m_perception = std::make_unique<PerceptionStrategy>(m_ragRetriever.get());
    m_reflection = std::make_unique<ReflectionEngine>();

    // 9. Tool 层
    m_toolRegistry = std::make_unique<ToolRegistry>();
    m_toolRegistry->registerTool(std::make_unique<SeekAndAnalyzeTool>(
        m_playerService.get(), m_videoAnalysis.get()));
    m_toolRegistry->registerTool(std::make_unique<AnalyzeTimeRangeTool>(
        m_videoAnalysis.get()));
    m_toolRegistry->registerTool(std::make_unique<SearchVideoContentTool>(
        m_ragRetriever.get()));
    m_toolRegistry->registerTool(std::make_unique<GetTranscriptTool>(
        m_ragStore.get()));
    m_toolRegistry->registerTool(std::make_unique<GetSceneInfoTool>(
        m_videoAnalysis.get()));
    m_toolRegistry->registerTool(std::make_unique<ControlPlayerTool>(m_eventBus));

    m_toolOrchestrator = std::make_unique<ToolOrchestrator>(
        m_agentService.get(), m_toolRegistry.get());

    // 10. 顶层协调器
    m_videoAgent = std::make_unique<VideoAgent>(
        m_agentService.get(),
        m_videoAnalysis.get(),
        m_ragRetriever.get(),
        m_qaCache.get(),
        m_toolOrchestrator.get(),
        m_perception.get(),
        m_reflection.get(),
        m_entityTracker.get());

    // 11. Workflow 引擎
    m_workflowCheckpoint = std::make_unique<WorkflowCheckpoint>();
    m_workflowCheckpoint->initialize();

    m_workflowFactory = std::make_unique<WorkflowFactory>();
    m_workflowFactory->setDependencies({
        m_agentService.get(),
        m_toolOrchestrator.get(),
        m_toolRegistry.get()
    });

    // 注册 Workflow Function Handlers
    m_workflowFactory->registerFunctionHandler(
        QStringLiteral("PerceptionStrategy::decide"),
        [this](WorkflowState& state, NodeCallback done) {
            const QString question = state.get("question").toString();
            const QString videoPath = state.get("video_path").toString();
            const int64_t posMs = state.get("current_pos_ms").toLongLong();

            QSharedPointer<VideoRepresentation> repr;
            if (m_videoAnalysis)
                repr = m_videoAnalysis->representation(videoPath);

            if (m_perception) {
                QuestionType qType = m_perception->classifyQuestion(question);
                SamplingPlan plan = m_perception->decideSampling(question, repr, posMs);
                SufficiencyCheck suff = m_perception->checkSufficiency(question, repr);

                state.set("question_type", static_cast<int>(qType));
                state.set("sampling_density", static_cast<int>(plan.density));
                state.set("frame_budget", plan.frameBudget);
                state.set("sufficiency", suff.isEnough ? 1.0 : 0.3);

                if (!plan.timeRanges.isEmpty()) {
                    state.set("sample_start_ms", plan.timeRanges.first().first);
                    state.set("sample_end_ms", plan.timeRanges.last().second);
                }
            } else {
                state.set("sufficiency", 1.0);
            }

            done(NodeResult{.nextRoute = {}, .success = true, .error = {}});
        });

    m_workflowFactory->registerFunctionHandler(
        QStringLiteral("VideoRAGRetriever::retrieve"),
        [this](WorkflowState& state, NodeCallback done) {
            const QString question = state.get("question").toString();
            const QString videoId = state.get("video_id").toString();

            if (!m_ragRetriever || videoId.isEmpty()) {
                state.set("sufficiency", 1.0);
                done(NodeResult{.nextRoute = {}, .success = true, .error = {}});
                return;
            }

            VideoRAGRetriever::Constraints c;
            c.videoId = videoId;

            int64_t startMs = state.get("sample_start_ms").toLongLong();
            int64_t endMs = state.get("sample_end_ms").toLongLong();
            if (startMs > 0 && endMs > startMs) {
                c.startMsGte = startMs;
                c.endMsLte = endMs;
            }

            int topK = 5;
            int qType = state.get("question_type").toInt();
            if (qType == static_cast<int>(QuestionType::GlobalSummary))
                topK = 8;
            else if (qType == static_cast<int>(QuestionType::CurrentFrame))
                topK = 3;
            else if (qType == static_cast<int>(QuestionType::EntityQuery)) {
                topK = 6;
                c.preferPath = QStringLiteral("entity");
            }

            auto evidence = m_ragRetriever->retrieve(question, c, topK);

            // 将检索结果格式化写入 state 的 video_context
            QVariant ctxVar = state.get("video_context");
            VideoContext videoCtx;
            if (ctxVar.canConvert<VideoContext>())
                videoCtx = ctxVar.value<VideoContext>();

            QString evidenceText;
            int idx = 1;
            for (const auto& r : evidence) {
                evidenceText += QString("## 证据 %1\n时间: %2ms-%3ms (来源: %4, 分数: %5)\n%6\n\n")
                    .arg(idx)
                    .arg(r.chunk.startMs).arg(r.chunk.endMs)
                    .arg(r.hitPath)
                    .arg(r.score, 0, 'f', 2)
                    .arg(r.chunk.textContent.left(200));
                ++idx;
            }
            videoCtx.retrievalEvidence = evidenceText;
            state.set("video_context", QVariant::fromValue(videoCtx));

            // 如果有证据，标记为充分
            double sufficiency = evidence.isEmpty() ? 0.3 : 0.8;
            state.set("sufficiency", sufficiency);

            done(NodeResult{.nextRoute = {}, .success = true, .error = {}});
        });

    m_workflowFactory->registerFunctionHandler(
        QStringLiteral("ReflectionEngine::check"),
        [this](WorkflowState& state, NodeCallback done) {
            const QString answer = state.get("answer").toString();

            if (!m_reflection || answer.isEmpty()) {
                state.set("confidence", 0.8);
                done(NodeResult{.nextRoute = {}, .success = true, .error = {}});
                return;
            }

            const QString videoPath = state.get("video_path").toString();
            QSharedPointer<VideoRepresentation> repr;
            if (m_videoAnalysis)
                repr = m_videoAnalysis->representation(videoPath);

            QVector<RetrievalResult> emptyEvidence;
            auto rr = m_reflection->reflect(answer, emptyEvidence, repr);

            state.set("confidence", static_cast<double>(rr.confidence));
            if (!rr.fixSuggestion.isEmpty())
                state.set("fix_suggestion", rr.fixSuggestion);

            done(NodeResult{.nextRoute = {}, .success = true, .error = {}});
        });

    m_workflowExecutor = std::make_unique<WorkflowExecutor>();
    m_workflowExecutor->setCheckpoint(m_workflowCheckpoint.get());

    // 注入 Workflow 引擎到 VideoAgent
    m_videoAgent->setWorkflowExecutor(m_workflowExecutor.get());
    m_videoAgent->setWorkflowFactory(m_workflowFactory.get());
    m_videoAgent->setWorkflowCheckpoint(m_workflowCheckpoint.get());

    // ViewModels
    m_playerVM = std::make_unique<PlayerViewModel>(m_playerService.get(),
                                                  m_eventBus);
    m_chatVM   = std::make_unique<ChatViewModel>(m_agentService.get(),
                                                m_convService.get(),
                                                m_eventBus);
    m_chatVM->setPlayerViewModel(m_playerVM.get());
    m_chatVM->setVideoAgent(m_videoAgent.get());
    m_chatVM->setVideoAnalysisService(m_videoAnalysis.get());
    m_fileListVM = std::make_unique<FileListViewModel>(m_fileService.get(),
                                                      m_eventBus,
                                                      m_playerService.get());

    m_videoAnalysisVM = std::make_unique<VideoAnalysisViewModel>(
        m_videoAnalysis.get(), m_videoIndexer.get());

    m_knowledgeVM = std::make_unique<KnowledgeViewModel>(
        m_ragStore.get(), m_ragRetriever.get(), m_db);
#ifdef FRAMEMIND_HAS_ONNXRUNTIME
    m_knowledgeVM->setEmbeddingService(m_embeddingService.get());
#endif
}
