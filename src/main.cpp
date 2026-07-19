#include <QApplication>
#include <QIcon>
#include <QSurfaceFormat>

#include "app/application.h"
#include "model/playertypes.h"
#include "model/videoinfo.h"
#include "model/videoframe.h"
#include "model/chatmessage.h"
#include "model/conversation.h"

int main(int argc, char* argv[])
{
    QApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

    QSurfaceFormat fmt;
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setDepthBufferSize(0);
    fmt.setStencilBufferSize(0);
    fmt.setSamples(0);
    fmt.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    QSurfaceFormat::setDefaultFormat(fmt);

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("FrameMind"));
    QApplication::setOrganizationName(QStringLiteral("FrameMind"));

    // 设置应用程序图标（任务栏图标）
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/app_dark.ico")));

    // 注册跨线程信号需要的自定义类型（SDK 线程 → 主线程 QueuedConnection）
    qRegisterMetaType<PlayerState>("PlayerState");
    qRegisterMetaType<VideoInfo>("VideoInfo");
    qRegisterMetaType<VideoFrame>("VideoFrame");
    qRegisterMetaType<ChatMessage>("ChatMessage");
    qRegisterMetaType<Conversation>("Conversation");

    Application application;
    application.start();

    return app.exec();
}
