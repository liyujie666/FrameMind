#include <QApplication>
#include <QIcon>

#include "app/application.h"
#include "model/playertypes.h"
#include "model/videoinfo.h"
#include "model/chatmessage.h"
#include "model/conversation.h"

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("FrameMind"));
    QApplication::setOrganizationName(QStringLiteral("FrameMind"));

    // 设置应用程序图标（任务栏图标）
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/app.ico")));

    // 注册跨线程信号需要的自定义类型（SDK 线程 → 主线程 QueuedConnection）
    qRegisterMetaType<PlayerState>("PlayerState");
    qRegisterMetaType<VideoInfo>("VideoInfo");
    qRegisterMetaType<ChatMessage>("ChatMessage");
    qRegisterMetaType<Conversation>("Conversation");

    Application application;
    application.start();

    return app.exec();
}
