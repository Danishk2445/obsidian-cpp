#include "core/config/clone_paths.h"

#include <QDir>
#include <QStandardPaths>

namespace oc {

namespace {
QString appDir(QStandardPaths::StandardLocation loc)
{
    return QStandardPaths::writableLocation(loc) + QStringLiteral("/obsidian-cpp");
}
} // namespace

QString ClonePaths::configRoot() { return appDir(QStandardPaths::GenericConfigLocation); }
QString ClonePaths::dataRoot() { return appDir(QStandardPaths::GenericDataLocation); }
QString ClonePaths::cacheRoot() { return appDir(QStandardPaths::GenericCacheLocation); }

QString ClonePaths::registryFile() { return configRoot() + QStringLiteral("/obsidian-cpp.json"); }
QString ClonePaths::vaultConfigDir(const QString& id) { return configRoot() + QStringLiteral("/vaults/") + id; }
QString ClonePaths::vaultDataDir(const QString& id) { return dataRoot() + QLatin1Char('/') + id; }
QString ClonePaths::vaultCacheDir(const QString& id) { return cacheRoot() + QLatin1Char('/') + id; }

QString ClonePaths::obsidianRegistryFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/obsidian/obsidian.json");
}

QString ClonePaths::ensureDir(const QString& path)
{
    QDir().mkpath(path);
    return path;
}

} // namespace oc
