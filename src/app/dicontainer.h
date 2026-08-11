#ifndef FRAMEMIND_DICONTAINER_H
#define FRAMEMIND_DICONTAINER_H

#include <memory>

class EventBus;
class DatabaseManager;
class NetworkClient;
class SettingsService;
class ThemeService;
class PlayerService;
class AgentService;
class ConversationService;
class FileManagerService;
class PlayerViewModel;
class ChatViewModel;
class FileListViewModel;
class LLMProviderService;
class SceneDetector;
#ifdef FRAMEMIND_HAS_ONNXRUNTIME
class ClipService;
class EmbeddingService;
#endif
#ifdef FRAMEMIND_HAS_WHISPER
class WhisperService;
#endif

// Video Agent (M3/M4)
class VideoRAGStore;
class QACacheManager;
class VideoRAGRetriever;
class EntityTracker;
class AudioVisualAligner;
class VideoIndexer;
class OneShotVlmChannel;
class VideoAnalysisService;
class PerceptionStrategy;
class ReflectionEngine;
class ToolRegistry;
class ToolOrchestrator;
class VideoAgent;

// Workflow Engine
class WorkflowExecutor;
class WorkflowFactory;
class WorkflowCheckpoint;

// Analysis ViewModel (分析面板)
class VideoAnalysisViewModel;

// Knowledge base ViewModel
class KnowledgeViewModel;

/**
 * 简易依赖注入容器。负责装配 Infrastructure / Service / ViewModel 的生命周期。
 * 不引入新的全局单例（EventBus / DatabaseManager 的 instance() 为既有例外）。
 */
class DIContainer {
public:
    DIContainer();
    ~DIContainer();

    void initialize();

    // ---- 已有 ViewModel / Service ----
    PlayerViewModel*    playerVM() const { return m_playerVM.get(); }
    ChatViewModel*      chatVM() const { return m_chatVM.get(); }
    FileListViewModel*  fileListVM() const { return m_fileListVM.get(); }
    PlayerService*      playerService() const { return m_playerService.get(); }
    AgentService*       agentService() const { return m_agentService.get(); }
    SettingsService*    settingsService() const { return m_settingsService.get(); }
    ThemeService*       themeService() const { return m_themeService.get(); }
    FileManagerService* fileManagerService() const { return m_fileService.get(); }
    LLMProviderService* llmProviderService() const { return m_providerService.get(); }
    EventBus*           eventBus() const { return m_eventBus; }

    // Video RAG 服务（M3/M4）
    SceneDetector*      sceneDetector() const { return m_sceneDetector.get(); }
#ifdef FRAMEMIND_HAS_ONNXRUNTIME
    ClipService*        clipService() const { return m_clipService.get(); }
    EmbeddingService*   embeddingService() const { return m_embeddingService.get(); }
#endif
#ifdef FRAMEMIND_HAS_WHISPER
    WhisperService*     whisperService() const { return m_whisperService.get(); }
#endif

    // ---- Video Agent 组件（M4）----
    VideoRAGStore*        ragStore() const;
    QACacheManager*       qaCache() const;
    VideoRAGRetriever*    ragRetriever() const;
    EntityTracker*        entityTracker() const;
    AudioVisualAligner*   audioVisualAligner() const;
    VideoIndexer*         videoIndexer() const;
    VideoAnalysisService* videoAnalysisService() const;
    PerceptionStrategy*   perceptionStrategy() const;
    ReflectionEngine*     reflectionEngine() const;
    ToolRegistry*         toolRegistry() const;
    ToolOrchestrator*     toolOrchestrator() const;
    VideoAgent*           videoAgent() const;
    VideoAnalysisViewModel* videoAnalysisVM() const;
    KnowledgeViewModel*     knowledgeVM() const;

    // ---- Workflow Engine ----
    WorkflowExecutor*     workflowExecutor() const;
    WorkflowFactory*      workflowFactory() const;
    WorkflowCheckpoint*   workflowCheckpoint() const;

private:
    EventBus*        m_eventBus = nullptr;        // 不持有所有权
    DatabaseManager* m_db = nullptr;              // 不持有所有权（单例）

    std::unique_ptr<NetworkClient>       m_network;
    std::unique_ptr<NetworkClient>       m_vlmNetwork;
    std::unique_ptr<SettingsService>     m_settingsService;
    std::unique_ptr<ThemeService>       m_themeService;
    std::unique_ptr<PlayerService>      m_playerService;
    std::unique_ptr<LLMProviderService> m_providerService;
    std::unique_ptr<AgentService>       m_agentService;
    std::unique_ptr<AgentService>       m_vlmAgentService;
    std::unique_ptr<OneShotVlmChannel>  m_oneShotVlmChannel;
    std::unique_ptr<ConversationService> m_convService;
    std::unique_ptr<FileManagerService> m_fileService;

    // Video RAG 服务
    std::unique_ptr<SceneDetector>      m_sceneDetector;
#ifdef FRAMEMIND_HAS_ONNXRUNTIME
    std::unique_ptr<ClipService>       m_clipService;
    std::unique_ptr<EmbeddingService>  m_embeddingService;
#endif
#ifdef FRAMEMIND_HAS_WHISPER
    std::unique_ptr<WhisperService>    m_whisperService;
#endif

    // Video Agent 组件
    std::unique_ptr<VideoRAGStore>        m_ragStore;
    std::unique_ptr<QACacheManager>       m_qaCache;
    std::unique_ptr<VideoRAGRetriever>    m_ragRetriever;
    std::unique_ptr<EntityTracker>        m_entityTracker;
    std::unique_ptr<AudioVisualAligner>   m_avAligner;
    std::unique_ptr<VideoIndexer>         m_videoIndexer;
    std::unique_ptr<VideoAnalysisService> m_videoAnalysis;
    std::unique_ptr<PerceptionStrategy>   m_perception;
    std::unique_ptr<ReflectionEngine>     m_reflection;
    std::unique_ptr<ToolRegistry>         m_toolRegistry;
    std::unique_ptr<ToolOrchestrator>m_toolOrchestrator;
    std::unique_ptr<VideoAgent>           m_videoAgent;

    // Workflow Engine
    std::unique_ptr<WorkflowExecutor>     m_workflowExecutor;
    std::unique_ptr<WorkflowFactory>      m_workflowFactory;
    std::unique_ptr<WorkflowCheckpoint>   m_workflowCheckpoint;

    std::unique_ptr<PlayerViewModel>        m_playerVM;
    std::unique_ptr<ChatViewModel>          m_chatVM;
    std::unique_ptr<FileListViewModel>      m_fileListVM;
    std::unique_ptr<VideoAnalysisViewModel> m_videoAnalysisVM;
    std::unique_ptr<KnowledgeViewModel>     m_knowledgeVM;
};

#endif // FRAMEMIND_DICONTAINER_H
