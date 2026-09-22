#include "app/application.h"
#include "app/cli.h"

#include <QApplication>
#include <QStyleFactory>

int main(int argc, char** argv)
{
    QApplication::setApplicationName(QStringLiteral("obsidian-cpp"));
    QApplication::setApplicationDisplayName(QStringLiteral("Obsidian C++"));
    QApplication::setApplicationVersion(QStringLiteral(OC_VERSION));
    QApplication::setDesktopFileName(QStringLiteral("obsidian-cpp"));
    QApplication app(argc, argv);
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    oc::CliOptions opts = oc::parseCli(QApplication::arguments());
    oc::Application controller(std::move(opts));
    if (controller.start() != 0)
        return 1;
    return QApplication::exec();
}
