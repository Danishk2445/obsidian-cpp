#pragma once

// Vault-wide property operations (Obsidian's fileManager.renameProperty / deleteProperty, used by the
// "All properties" view). Notes are edited through FileManager::editFile: an open note gets one
// undoable change that is saved right away, a closed one a compare-and-swap write; only notes whose
// frontmatter has the property are touched, and only the property's lines change.

#include <QString>

namespace oc {

class VaultSession;

namespace props {

// Renames `oldKey` (exact spelling) to `newKey` in every note; where a note already has `newKey`
// the values are merged (lists concatenated, otherwise the old key's value wins) and `oldKey` goes.
// The assigned type moves to the new name. Returns the number of notes changed, -1 when the session
// has no vault services.
int renameProperty(VaultSession* session, const QString& oldKey, const QString& newKey, QString* error = nullptr);

// Removes `key` (matched case-insensitively) from every note and unsets its assigned type. A note
// left without properties loses its frontmatter block, like in Obsidian.
int deleteProperty(VaultSession* session, const QString& key, QString* error = nullptr);

// Would renaming to `newKey` merge into a property the vault already has (Obsidian asks first)?
bool propertyExists(VaultSession* session, const QString& key);

} // namespace props
} // namespace oc
