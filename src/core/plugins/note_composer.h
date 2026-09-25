#pragma once

#include "core/metadata/link_resolver.h"
#include "text/changeset.h"
#include "text/text.h"

#include <QDateTime>
#include <QJsonObject>
#include <QString>

#include <optional>

namespace oc::md {
struct CachedMetadata;
}

namespace oc {
class VaultSession;
class TFile;
}

namespace oc::plugins {

// Note composer ("note-composer" in the clone config): {"askBeforeMerging": true, "replacementText":
// "link" | "embed" | "none", "template": "path/to/template.md"}.
struct NoteComposerOptions {
    bool askBeforeMerging = true;
    QString replacementText = QStringLiteral("link");
    QString templatePath;
    static NoteComposerOptions fromJson(const QJsonObject& o);
};

// The heading on line `line` and its section: from the heading to the next heading of the same or a
// higher level (blank lines before it trimmed down to one line break), or to the end of the note.
struct HeadingRange {
    QString heading;
    qsizetype start = 0;
    qsizetype end = 0;
};
std::optional<HeadingRange> headingRangeAt(const md::CachedMetadata& meta, const QString& text, int line);
// "# Title" -> "Title" (the first ATX heading marker at the start of the text), nullopt otherwise.
std::optional<QString> headingTextOf(const QString& line);

// The template ("note-composer" template option): {{content}} (appended after a blank line when the
// template lacks it), {{fromTitle}}, {{newTitle}}, {{date}}, {{time}}, {{date:FMT}}.
QString applyComposerTemplate(QString templateText, const QString& content, const QString& fromTitle, const QString& newTitle,
                              const QDateTime& now = QDateTime::currentDateTime());

// Where the subpath ("#Heading", "#^block", "#[^1]") of `meta`/`text` starts, as an offset.
std::optional<qsizetype> subpathOffset(const md::CachedMetadata& meta, const Text& text, const QString& subpath);

// Links inside text[from, to) of the source note rewritten for the note they move to: links to other
// notes are made relative to the destination; links into the source note ("#Heading", "[[Source#H]]")
// become "#Heading" when their target moves along, else "[[Source#H]]" from the destination.
QString rewriteLinksForDestination(const md::CachedMetadata& meta, const Text& text, const QString& sourcePath,
                                   const QString& destPath, qsizetype from, qsizetype to, const LinkResolver& resolver,
                                   LinkResolver::LinkFormat format);
// Links in the rest of the source note that point into the moved range: rewritten to point at the
// destination (e.g. "#Heading" -> "[[Dest#Heading]]").
QList<ChangeSpec> pathlessLinkUpdatesForSource(const md::CachedMetadata& meta, const Text& text, const QString& sourcePath,
                                               const QString& destPath, qsizetype from, qsizetype to,
                                               const LinkResolver& resolver, LinkResolver::LinkFormat format);

// The operations, as Obsidian's note composer runs them (links in other notes follow, see
// FileManager::runLinkUpdate). They return false with *error when a file couldn't be changed.
//
// Merge `from` into `into` (at the end, or the top when `prepend`), then delete `from` per the
// "Deleted files" setting; links to `from` now point at `into`.
bool mergeNotes(VaultSession* session, TFile* from, TFile* into, bool prepend, QString* error = nullptr);
// Move text[from, to) of `source` into `dest` (at its end or top), leaving a link, an embed or nothing
// ("replacementText"); links to headings/blocks that moved follow them.
bool extractToNote(VaultSession* session, TFile* source, qsizetype from, qsizetype to, TFile* dest, bool prepend,
                   QString* error = nullptr);

} // namespace oc::plugins
