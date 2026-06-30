#ifndef FRAMEMIND_APPLICATION_H
#define FRAMEMIND_APPLICATION_H

#include <memory>

class DIContainer;
class MainWindow;

/**
 * 应用生命周期管理：构建 DI 容器、创建并显示主窗口。
 */
class Application {
public:
    Application();
    ~Application();

    void start();   // 初始化 DI + 显示主窗口

private:
    std::unique_ptr<DIContainer> m_container;
    std::unique_ptr<MainWindow>  m_mainWindow;
};

#endif // FRAMEMIND_APPLICATION_H
