#include "app/application.h"

#include "app/dicontainer.h"
#include "view/mainwindow.h"

Application::Application() = default;
Application::~Application() = default;

void Application::start()
{
    m_container = std::make_unique<DIContainer>();
    m_container->initialize();

    m_mainWindow = std::make_unique<MainWindow>(m_container->playerVM(),
                                                m_container->chatVM(),
                                                m_container->settingsService(),
                                                m_container->agentService());
    m_mainWindow->show();
}
