#include "app/dicontainer.h"

#include "infrastructure/eventbus.h"
#include "infrastructure/databasemanager.h"
#include "infrastructure/networkclient.h"
#include "service/settingsservice.h"
#include "service/themeservice.h"
#include "service/playerservice.h"
#include "service/agentservice.h"
#include "service/conversationservice.h"
#include "viewmodel/playerviewmodel.h"
#include "viewmodel/chatviewmodel.h"

#include <QStandardPaths>
#include <QDir>

DIContainer::DIContainer() = default;
DIContainer::~DIContainer() = default;

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
    m_agentService    = std::make_unique<AgentService>(m_network.get(),
                                                       m_settingsService.get());
    m_convService     = std::make_unique<ConversationService>(m_db);

    // ViewModels
    m_playerVM = std::make_unique<PlayerViewModel>(m_playerService.get(),
                                                   m_eventBus);
    m_chatVM   = std::make_unique<ChatViewModel>(m_agentService.get(),
                                                 m_convService.get(),
                                                 m_eventBus);
}
