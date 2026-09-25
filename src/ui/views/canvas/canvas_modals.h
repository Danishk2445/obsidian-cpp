#pragma once

// The canvas plugin's prompts, in the window's overlay layer:
//  - file pickers ("Add note from vault" / "Add media from vault" / backgrounds / "Swap file..."),
//  - a text prompt ("Enter URL", "Convert to file..."),
//  - "Narrow to heading..." and "Jump to group" suggesters.

#include "core/canvas/canvas_model.h"

#include <QString>

#include <functional>

namespace oc {

class Modal;
class OverlayLayer;
class SuggestModal;
class VaultSession;

namespace canvasui {

enum class FileFilter {
    Notes,  // notes and canvases (Shift+Enter creates a note)
    Media,  // attachments: everything but notes
    Images, // images only (group backgrounds)
    Any,
};

// Obsidian's file suggest: fuzzy search over the vault's files of `filter` (recent files first when the
// query is empty); `chosen` gets the vault path. `sourcePath` places notes created with Shift+Enter.
SuggestModal* openFilePicker(VaultSession* session, OverlayLayer* layer, FileFilter filter, const QString& sourcePath,
                             std::function<void(const QString& path)> chosen);

// A one-line prompt with a Save button: `validate` returns an error message (shown, the prompt stays
// open) or "" to accept.
Modal* promptText(OverlayLayer* layer, const QString& title, const QString& initial, const QString& placeholder,
                  std::function<QString(const QString&)> validate, std::function<void(const QString&)> done);

// "Narrow to heading...": the note's headings ("Show entire file" first); `chosen` gets "#Heading" or "".
SuggestModal* openHeadingPicker(VaultSession* session, OverlayLayer* layer, const QString& notePath,
                                std::function<void(const QString& subpath)> chosen);

// "Jump to group": the canvas' groups by label.
SuggestModal* openGroupPicker(OverlayLayer* layer, const canvas::CanvasData& data, std::function<void(const QString& nodeId)> chosen);

} // namespace canvasui
} // namespace oc
