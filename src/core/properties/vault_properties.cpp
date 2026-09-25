#include "core/properties/vault_properties.h"

#include "core/files/file_manager.h"
#include "core/metadata/metadata_cache.h"
#include "core/properties/frontmatter_editor.h"
#include "core/properties/property_types.h"
#include "core/vault_session.h"
#include "markdown/metadata.h"

namespace oc::props {

namespace {

// Notes whose cached frontmatter has a key accepted by `match`.
QStringList notesWith(MetadataCache* mc, const std::function<bool(const QString&)>& match)
{
    QStringList out;
    for (const QString& path : mc->markdownFiles()) {
        const md::CachedMetadata* meta = mc->fileCache(path);
        if (!meta || !meta->frontmatter)
            continue;
        for (const md::FrontmatterValue& v : *meta->frontmatter) {
            if (match(v.key)) {
                out << path;
                break;
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

bool propertyExists(VaultSession* session, const QString& key)
{
    PropertyTypeManager* types = PropertyTypeManager::of(session);
    return types && types->allProperties().contains(key.toLower());
}

int renameProperty(VaultSession* session, const QString& oldKey, const QString& newKey, QString* error)
{
    MetadataCache* mc = session ? session->metadata() : nullptr;
    FileManager* fm = session ? session->fileManager() : nullptr;
    if (!mc || !fm)
        return -1;
    if (oldKey.isEmpty() || newKey.isEmpty() || oldKey == newKey)
        return 0;
    if (PropertyTypeManager* types = PropertyTypeManager::of(session)) {
        const QString type = types->assignedType(oldKey);
        if (!type.isEmpty()) {
            if (!PropertyTypeManager::isReserved(oldKey))
                types->unsetType(oldKey);
            types->setType(newKey, type);
        }
    }
    int changed = 0;
    for (const QString& path : notesWith(mc, [&](const QString& k) { return k == oldKey; })) {
        bool touched = false;
        QString err;
        const bool ok = fm->editFile(path, [&](const Text& text) -> QList<ChangeSpec> {
            FrontmatterEditor ed(text);
            if (!ed.isEditable() || !ed.has(oldKey))
                return {};
            if (ed.has(newKey)) {
                ed.merge(newKey, ed.value(oldKey));
                ed.remove(oldKey);
            } else {
                ed.rename(oldKey, newKey);
            }
            const QList<ChangeSpec> specs = ed.changes();
            touched = !specs.isEmpty();
            return specs;
        }, &err);
        if (!ok && error && error->isEmpty())
            *error = err;
        if (ok && touched)
            ++changed;
    }
    return changed;
}

int deleteProperty(VaultSession* session, const QString& key, QString* error)
{
    MetadataCache* mc = session ? session->metadata() : nullptr;
    FileManager* fm = session ? session->fileManager() : nullptr;
    if (!mc || !fm)
        return -1;
    const QString lower = key.toLower();
    int changed = 0;
    for (const QString& path : notesWith(mc, [&](const QString& k) { return k.toLower() == lower; })) {
        bool touched = false;
        QString err;
        const bool ok = fm->editFile(path, [&](const Text& text) -> QList<ChangeSpec> {
            FrontmatterEditor ed(text);
            if (!ed.isEditable())
                return {};
            QString actual;
            for (const Property& p : ed.properties()) {
                if (p.key == key) {
                    actual = p.key;
                    break;
                }
                if (actual.isEmpty() && p.key.toLower() == lower)
                    actual = p.key;
            }
            if (actual.isEmpty() || !ed.remove(actual))
                return {};
            const QList<ChangeSpec> specs = ed.changes();
            touched = !specs.isEmpty();
            return specs;
        }, &err);
        if (!ok && error && error->isEmpty())
            *error = err;
        if (ok && touched)
            ++changed;
    }
    if (PropertyTypeManager* types = PropertyTypeManager::of(session); types && !PropertyTypeManager::isReserved(key))
        types->unsetType(key);
    return changed;
}

} // namespace oc::props
