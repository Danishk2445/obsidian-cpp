#pragma once

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QString>

#include <functional>
#include <optional>

namespace oc::plugins {

// Obsidian's template variable substitution (used by Templates, Daily notes and Unique note):
// {{name}} for every entry of `vars` (case-insensitive), then {{date}}, {{time}}, {{date:FMT}} and
// {{time:FMT}} (moment.js formats; defaults YYYY-MM-DD / HH:mm), all from the same instant.
QString applyTemplate(QString content, const QHash<QString, QString>& vars, const QString& dateFormat, const QString& timeFormat,
                      const QDateTime& now);

// A template's frontmatter (YAML between the leading --- lines, without them) and the rest.
struct TemplateParts {
    bool hasFrontmatter = false;
    QString frontmatter;
    QString body;
};
TemplateParts splitFrontmatter(const QString& content);
// Adds `templateYaml`'s top-level keys that the note's frontmatter doesn't have yet (or creates the
// frontmatter). Returns the note's new text (Obsidian's metadataEditor.insertProperties, simplified).
QString mergeFrontmatter(const QString& noteText, const QString& templateYaml);

// Daily notes ("daily-notes" config: format, folder, template).
struct DailyNotesOptions {
    QString format; // moment.js; "" = YYYY-MM-DD
    QString folder; // "" = the new-file location
    QString templatePath;
    static DailyNotesOptions fromJson(const QJsonObject& o);
    QString effectiveFormat() const;
};
QString dailyNoteName(const DailyNotesOptions& o, const QDateTime& when); // the formatted name (may contain '/')
// The date a note path stands for: relative to `folderPath` (the daily-notes folder, "" = root),
// without ".md", strictly parsed with the format. nullopt for other notes.
std::optional<QDateTime> dailyNoteDate(const QString& notePath, const QString& folderPath, const DailyNotesOptions& o);

// Unique note ids ("zk-prefixer": format default YYYYMMDDHHmm). `taken(prefix)` tells whether a file
// name starts with the candidate; on a clash the time is advanced by the smallest unit the format shows
// (Obsidian's quirk kept: the advanced id loses the suffix).
std::optional<QString> uniqueNoteName(const QString& format, const QString& suffix, const QDateTime& now,
                                      const std::function<bool(const QString&)>& taken);

// "path" -> "path.md" when it has no extension (template option values).
QString withMdExtension(const QString& path);

} // namespace oc::plugins
