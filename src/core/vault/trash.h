#pragma once

// Trash backends for Vault::trash() (private to src/core/vault).

#include <QString>

namespace oc::vaultfs {

// Freedesktop trash through KIO::trash (what Dolphin uses; puts files from removable drives into
// <mount>/.Trash-<uid>), run synchronously; falls back to QFile::moveToTrash. Needs a
// QCoreApplication. The job's nested event loop excludes user input.
bool trashToSystem(const QString& absPath, QString* error);

// Obsidian's "move to .trash": <root>/.trash/<name>, then "<base> 1.<ext>", "<base> 2.<ext>", ... on
// collisions (folders: "<name> 1", ...). *trashedPath receives the destination.
bool trashToLocalFolder(const QString& root, const QString& absPath, bool isFolder, QString* error,
                        QString* trashedPath = nullptr);

} // namespace oc::vaultfs
