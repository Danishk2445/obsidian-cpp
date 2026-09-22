#pragma once

#include <QJsonObject>
#include <QString>

namespace oc {

// Small helpers for the clone's own JSON config files (never vault files).
namespace JsonFile {

// Returns an empty object if the file is missing or unparsable (and reports it via *error when given).
QJsonObject read(const QString& path, QString* error = nullptr);

// Writes atomically (QSaveFile) with 2-space indentation like Obsidian's own config files.
// Skips the write when the bytes on disk would not change. Creates parent dirs.
bool write(const QString& path, const QJsonObject& obj, QString* error = nullptr);

} // namespace JsonFile
} // namespace oc
