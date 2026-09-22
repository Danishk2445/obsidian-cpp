#pragma once

// Which names/paths the vault ignores, and vault-relative path helpers (private to src/core/vault).

#include <QString>
#include <QStringView>

namespace oc::vaultfs {

// True for dot entries (".obsidian", ".git", ".trash", ".#lock", our ".<name>.oc~*" temps, vim's
// ".x.swp") and temp files of atomic savers/editors: "*.tmp.<digits>.<hex>", "*.swp", "*~", "4913".
bool isIgnoredName(QStringView name);

// "name.tmp.<digits>.<hex>" (regex ^.+\.tmp\.\d+\.[0-9a-fA-F]+$).
bool isAtomicTempName(QStringView name);

// True if any '/'-separated component of `path` is ignored.
bool isIgnoredPath(QStringView path);

// Normalizes a user-supplied vault path: collapses repeated '/', strips leading/trailing '/'.
// Returns false for "." / ".." components or an empty result when `allowRoot` is false.
bool normalizePath(const QString& in, QString* out, bool allowRoot = false);

inline QString joinPath(const QString& dir, const QString& name)
{
    return dir.isEmpty() ? name : QString(dir + u'/' + name);
}

inline QString parentPath(const QString& path)
{
    const qsizetype i = path.lastIndexOf(u'/');
    return i < 0 ? QString() : path.left(i);
}

inline QString fileNameOf(const QString& path)
{
    const qsizetype i = path.lastIndexOf(u'/');
    return i < 0 ? path : path.mid(i + 1);
}

// "a/b" is inside "a" (and inside "" = root); a path is not inside itself.
inline bool isStrictlyInside(const QString& path, const QString& folder)
{
    if (folder.isEmpty())
        return !path.isEmpty();
    return path.size() > folder.size() && path.startsWith(folder) && path.at(folder.size()) == u'/';
}

inline int pathDepth(const QString& path)
{
    return path.isEmpty() ? 0 : int(path.count(u'/')) + 1;
}

// Obsidian's basename/extension split: the extension is what follows the LAST dot, unless the dot is
// the first or the last character.
void splitName(const QString& name, QString* basename, QString* extension);

} // namespace oc::vaultfs
