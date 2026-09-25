#include "app/application.h"
#include "app/cli.h"

#include <QApplication>
#include <QIcon>
#include <QStyleFactory>

int main(int argc, char** argv)
{
    QApplication::setApplicationName(QStringLiteral("obsidian-cpp"));
    QApplication::setApplicationDisplayName(QStringLiteral("Obsidian C++"));
    QApplication::setApplicationVersion(QStringLiteral(OC_VERSION));
    QApplication::setDesktopFileName(QStringLiteral("obsidian-cpp"));
    QApplication app(argc, argv);
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/oc/app/obsidian-cpp.svg")));
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    oc::CliOptions opts = oc::parseCli(QApplication::arguments());
    oc::Application controller(std::move(opts));
    const int started = controller.start();
    if (started == oc::Application::HandedOff)
        return 0;
    if (started != 0)
        return 1;
    return QApplication::exec();
}
