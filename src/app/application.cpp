#include "app/application.h"

#include "app/dicontainer.h"
#include "service/themeservice.h"
#include "view/mainwindow.h"

Application::Application() = default;
Application::~Application() = default;

void Application::start()
{
    m_container = std::make_unique<DIContainer>();
    m_container->initialize();

    m_mainWindow = std::make_unique<MainWindow>(m_container->playerVM(),
                                                m_container->chatVM(),
                                                m_container->fileListVM(),
                                                m_container->settingsService(),
                                                m_container->agentService(),
                                                m_container->fileManagerService(),
                                                m_container->themeService());
    m_mainWindow->show();

    // Apply initial theme (会通过 ThemeService::themeChanged 触发各 View 刷新)
    m_container->themeService()->applyTheme();
}
