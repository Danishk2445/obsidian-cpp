#pragma once

#include <QString>

namespace oc {

class CommandRegistry;
class MainWindow;
class MarkdownView;
class SuggestModal;
class TFile;

// Note composer (core plugin "note-composer", on by default):
//   note-composer:merge-file       "Merge current file with another file..."  (active markdown file)
//   note-composer:split-file       "Extract current selection..."              (editor with a selection)
//   note-composer:extract-heading  "Extract this heading..."                   (cursor on a heading line)
// plus "Merge entire file with..." in file menus and the two extract items in the editor menu.
void registerNoteComposerCommands(CommandRegistry& registry, MainWindow* window);
void installNoteComposerMenus(); // file-menu / editor-menu providers (once per process)

// The pickers (Obsidian's file suggest over notes): Enter merges/moves to the bottom, Shift+Enter to the
// top, Mod+Enter creates a note named like the query. Merging asks first ("askBeforeMerging") and then
// opens the destination; extracting leaves a link/embed/nothing in place of the text.
SuggestModal* openMergeModal(MainWindow* window, TFile* current);
SuggestModal* openExtractModal(MainWindow* window, MarkdownView* view, const QString& defaultName = {});
// Selects the heading's section under the cursor and opens the extract picker named after it.
bool extractHeading(MainWindow* window, MarkdownView* view);

} // namespace oc
