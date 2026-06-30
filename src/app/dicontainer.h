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
class PlayerViewModel;
class ChatViewModel;

/**
 * 简易依赖注入容器。负责装配 Infrastructure / Service / ViewModel 的生命周期。
 * 不引入新的全局单例（EventBus / DatabaseManager 的 instance() 为既有例外）。
 */
class DIContainer {
public:
    DIContainer();
    ~DIContainer();

    void initialize();

    PlayerViewModel*  playerVM() const { return m_playerVM.get(); }
    ChatViewModel*    chatVM() const { return m_chatVM.get(); }
    PlayerService*    playerService() const { return m_playerService.get(); }
    AgentService*     agentService() const { return m_agentService.get(); }
    SettingsService*  settingsService() const { return m_settingsService.get(); }
    ThemeService*     themeService() const { return m_themeService.get(); }

private:
    EventBus*        m_eventBus = nullptr;        // 不持有所有权
    DatabaseManager* m_db = nullptr;              // 不持有所有权（单例）

    std::unique_ptr<NetworkClient>       m_network;
    std::unique_ptr<SettingsService>     m_settingsService;
    std::unique_ptr<ThemeService>         m_themeService;
    std::unique_ptr<PlayerService>       m_playerService;
    std::unique_ptr<AgentService>        m_agentService;
    std::unique_ptr<ConversationService> m_convService;

    std::unique_ptr<PlayerViewModel> m_playerVM;
    std::unique_ptr<ChatViewModel>   m_chatVM;
};

#endif // FRAMEMIND_DICONTAINER_H
