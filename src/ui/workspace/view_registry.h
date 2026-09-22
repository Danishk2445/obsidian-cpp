#pragma once

// FROZEN INTERFACE (M1) — maps view types and file extensions to view factories.

#include <QHash>
#include <QString>
#include <QStringList>

#include <functional>

namespace oc {

class View;
class WorkspaceLeaf;

class ViewRegistry {
public:
    using Factory = std::function<View*(WorkspaceLeaf* leaf)>;

    void registerView(const QString& type, Factory factory);
    void unregisterView(const QString& type);
    bool hasView(const QString& type) const;
    View* create(const QString& type, WorkspaceLeaf* leaf) const; // nullptr if unknown

    void registerExtensions(const QStringList& extensions, const QString& type); // lowercase, no dot
    QString typeForExtension(const QString& extension) const;                    // "" if none
    bool isExtensionRegistered(const QString& extension) const;

private:
    QHash<QString, Factory> m_factories;
    QHash<QString, QString> m_extensions;
};

} // namespace oc
