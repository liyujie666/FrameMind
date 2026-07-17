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

/**
 * 简易依赖注入容器。负责装配 Infrastructure / Service / ViewModel 的生命周期。
 * 不引入新的全局单例（EventBus / DatabaseManager 的 instance() 为既有例外）。
 */
class DIContainer {
public:
    DIContainer();
    ~DIContainer();

    void initialize();

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

private:
    EventBus*        m_eventBus = nullptr;        // 不持有所有权
    DatabaseManager* m_db = nullptr;              // 不持有所有权（单例）

    std::unique_ptr<NetworkClient>       m_network;
    std::unique_ptr<SettingsService>     m_settingsService;
    std::unique_ptr<ThemeService>       m_themeService;
    std::unique_ptr<PlayerService>      m_playerService;
    std::unique_ptr<LLMProviderService> m_providerService;
    std::unique_ptr<AgentService>       m_agentService;
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

    std::unique_ptr<PlayerViewModel>   m_playerVM;
    std::unique_ptr<ChatViewModel>     m_chatVM;
    std::unique_ptr<FileListViewModel> m_fileListVM;
};

#endif // FRAMEMIND_DICONTAINER_H
