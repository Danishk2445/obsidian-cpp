#pragma once

#include <QByteArray>
#include <QString>

class QMimeData;

namespace oc {

class MarkdownView;
class TFile;
class VaultSession;

// Obsidian's editor clipboard manager and attachment handling for markdown views.
namespace attachments {

// Where a new attachment goes (Obsidian's getAvailablePathForAttachments with "attachmentFolderPath"):
//   "/" (default) or ""  the vault root         "./" or "."  the note's folder
//   "./sub"              "sub" next to the note  "folder"     that vault folder (created when missing)
// The name is sanitized (":#|^\\ %% [[ ]]" -> spaces), capped at 250 chars, and made unique
// ("name 1.ext", ...). `source` may be null (then "./" means the root).
QString availablePathFor(VaultSession& session, const QString& name, const QString& ext, const TFile* source);

// Saves bytes as a new attachment next to `source`; returns the vault file (null on error).
TFile* saveAttachment(VaultSession& session, const QString& name, const QString& ext, const QByteArray& bytes,
                      const TFile* source, QString* error = nullptr);

// Obsidian's URL check for "paste a URL over a selection" (http(s), file, mailto, ... with no spaces).
bool isUrl(const QString& text);

// The paste pipeline (after built-in hooks such as Greentext Guard):
//   text/markdown as is; HTML converted to Markdown when "autoConvertHtml"; a single-line selection
//   plus a pasted URL becomes [selection](url); clipboard images become "Pasted image
//   YYYYMMDDHHmmss.png" attachments embedded with ![[...]]; file URLs inside the vault are embedded,
//   outside ones are copied in first. Returns false to let the editor paste plain text.
bool handlePaste(MarkdownView& view, const QMimeData& data);
// Drops: the same, minus the URL-over-selection rule.
bool handleDrop(MarkdownView& view, const QMimeData& data);

// Registers the paste/drop handlers with EditorHooks once (MarkdownView does this).
void installHooks();

} // namespace attachments
} // namespace oc
