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
#include "service/agent/video_indexer.h"
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

#include "viewmodel/playerviewmodel.h"
#include "viewmodel/chatviewmodel.h"
#include "viewmodel/filelistviewmodel.h"

#include <QStandardPaths>
#include <QDir>

DIContainer::DIContainer() = default;
DIContainer::~DIContainer() = default;

// Video Agent 组件 getter（延后定义以确保完整类型可见）
VideoRAGStore*        DIContainer::ragStore() const              { return m_ragStore.get(); }
QACacheManager*       DIContainer::qaCache() const               { return m_qaCache.get(); }
VideoRAGRetriever*    DIContainer::ragRetriever() const          { return m_ragRetriever.get(); }
EntityTracker*        DIContainer::entityTracker() const         { return m_entityTracker.get(); }
VideoIndexer*         DIContainer::videoIndexer() const          { return m_videoIndexer.get(); }
VideoAnalysisService* DIContainer::videoAnalysisService() const  { return m_videoAnalysis.get(); }
PerceptionStrategy*   DIContainer::perceptionStrategy() const    { return m_perception.get(); }
ReflectionEngine*     DIContainer::reflectionEngine() const      { return m_reflection.get(); }
ToolRegistry*         DIContainer::toolRegistry() const          { return m_toolRegistry.get(); }
ToolOrchestrator*     DIContainer::toolOrchestrator() const      { return m_toolOrchestrator.get(); }
VideoAgent*           DIContainer::videoAgent() const            { return m_videoAgent.get(); }

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
    m_convService    = std::make_unique<ConversationService>(m_db);
    m_fileService    = std::make_unique<FileManagerService>(m_db);

    // ---- Video RAG 小模型服务 ----
    const QString modelsDir = appData + QStringLiteral("/models");
    QDir().mkpath(modelsDir);

    m_sceneDetector = std::make_unique<SceneDetector>();

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
        modelsDir + QStringLiteral("/ggml-small.bin"));
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
                                                     m_ragStore.get());
#ifdef FRAMEMIND_HAS_ONNXRUNTIME
    m_videoIndexer->setClipService(m_clipService.get());
    m_videoIndexer->setEmbeddingService(m_embeddingService.get());
#endif
#ifdef FRAMEMIND_HAS_WHISPER
    m_videoIndexer->setWhisperService(m_whisperService.get());
#endif

    // 6. 分析服务
    m_videoAnalysis = std::make_unique<VideoAnalysisService>(
        m_agentService.get(), m_videoIndexer.get(),
        m_ragStore.get(), m_playerService.get());
#ifdef FRAMEMIND_HAS_ONNXRUNTIME
    m_videoAnalysis->setEmbeddingService(m_embeddingService.get());
#endif

    // 7. 决策组件
    m_perception = std::make_unique<PerceptionStrategy>(m_ragRetriever.get());
    m_reflection = std::make_unique<ReflectionEngine>();

    // 8. Tool 层
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

    // 9. 顶层协调器
    m_videoAgent = std::make_unique<VideoAgent>(
        m_agentService.get(),
        m_videoAnalysis.get(),
        m_ragRetriever.get(),
        m_qaCache.get(),
        m_toolOrchestrator.get(),
        m_perception.get(),
        m_reflection.get(),
        m_entityTracker.get());

    // ViewModels
    m_playerVM = std::make_unique<PlayerViewModel>(m_playerService.get(),
                                                  m_eventBus);
    m_chatVM   = std::make_unique<ChatViewModel>(m_agentService.get(),
                                                m_convService.get(),
                                                m_eventBus);
    m_chatVM->setPlayerViewModel(m_playerVM.get());
    m_fileListVM = std::make_unique<FileListViewModel>(m_fileService.get(),
                                                      m_eventBus,
                                                      m_playerService.get());
}
