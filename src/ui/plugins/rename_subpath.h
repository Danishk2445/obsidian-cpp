#pragma once

#include "core/metadata/link_updater.h"

#include <QString>

namespace oc {

class CommandRegistry;
class MainWindow;
class MarkdownView;
class TextPromptModal;

// Obsidian's "Rename this heading..." (editor:rename-heading, editor menu on a heading line) and
// "Rename this block ID..." (editor menu on a " ^block-id"): a prompt for the new text, then the
// heading line / block id changes and every link to "note#Old heading" / "note#^old-id" follows
// ("Updated N links in M files.").
namespace rename_subpath {

// Obsidian's heading normalisation for comparing subpaths: link-breaking characters -> spaces,
// whitespace runs -> one space, trimmed (TD); and for writing a new subpath (DD).
QString normalizeHeading(const QString& heading);
QString sanitizeSubpath(const QString& heading);
// The heading text of a Markdown line ("## Text" -> "Text"), or a null string.
QString headingTextOf(const QString& line);
QString validateBlockId(const QString& id); // "" = valid

// Link edits for all references to `filePath`'s heading `oldHeading` (renamed to `newHeading`), or
// to its block `oldId` (renamed to `newId`), per source file.
QHash<QString, QList<LinkUpdater::Edit>> headingLinkEdits(const QList<LinkUpdater::Reference>& refs, const QString& filePath,
                                                          const QString& oldHeading, const QString& newHeading);
QHash<QString, QList<LinkUpdater::Edit>> blockLinkEdits(const QList<LinkUpdater::Reference>& refs, const QString& filePath,
                                                        const QString& oldId, const QString& newId);

// Open the prompts for the heading on `line` / the block id at [from, to) (the "^id" text) of `view`.
TextPromptModal* promptHeadingRename(MainWindow* window, MarkdownView* view, int line);
TextPromptModal* promptBlockIdRename(MainWindow* window, MarkdownView* view, qsizetype from, qsizetype to);

// The block id token (" ^id" at the end of a line) whose "^id" contains `pos`: [from, to) of "^id".
bool blockIdAt(const MarkdownView* view, qsizetype pos, qsizetype* from, qsizetype* to);

void registerCommands(CommandRegistry& registry, MainWindow* window); // editor:rename-heading
void installEditorMenus();                                           // once per process

} // namespace rename_subpath
} // namespace oc
