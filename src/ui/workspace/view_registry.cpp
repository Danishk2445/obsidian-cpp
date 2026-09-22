#include "ui/workspace/view_registry.h"

namespace oc {

void ViewRegistry::registerView(const QString& type, Factory factory)
{
    if (type.isEmpty() || !factory)
        return;
    m_factories.insert(type, std::move(factory));
}

void ViewRegistry::unregisterView(const QString& type)
{
    m_factories.remove(type);
    // Extensions pointing at a type that no longer exists are dropped too (Obsidian does the same
    // when a plugin that registered extensions unloads).
    for (auto it = m_extensions.begin(); it != m_extensions.end();) {
        if (it.value() == type)
            it = m_extensions.erase(it);
        else
            ++it;
    }
}

bool ViewRegistry::hasView(const QString& type) const { return m_factories.contains(type); }

View* ViewRegistry::create(const QString& type, WorkspaceLeaf* leaf) const
{
    auto it = m_factories.constFind(type);
    if (it == m_factories.cend())
        return nullptr;
    return (*it)(leaf);
}

void ViewRegistry::registerExtensions(const QStringList& extensions, const QString& type)
{
    for (const QString& e : extensions) {
        QString ext = e.toLower();
        if (ext.startsWith(QLatin1Char('.')))
            ext.remove(0, 1);
        if (!ext.isEmpty())
            m_extensions.insert(ext, type);
    }
}

QString ViewRegistry::typeForExtension(const QString& extension) const
{
    return m_extensions.value(extension.toLower());
}

bool ViewRegistry::isExtensionRegistered(const QString& extension) const
{
    return m_extensions.contains(extension.toLower());
}

} // namespace oc
