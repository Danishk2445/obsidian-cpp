#pragma once

#include <QString>
#include <QtCore/qnamespace.h>
#include <QStringList>

class QKeyEvent;

namespace oc::vim {

// Vim key notation, as CodeMirror-vim spells it: printable characters as themselves ("a", "A", ":"),
// everything else in angle brackets with C-/A-/M-/S- prefixes ("<Esc>", "<CR>", "<BS>", "<C-r>",
// "<C-S-A>", "<S-Space>", "<Left>", "<PageDown>", "<F2>").

// The vim key for a key event (Obsidian's vimKeyFromEvent); empty for modifier-only presses and
// keys that have no name. `langmap` (from ":set langmap") remaps plain characters unless
// `literal` (the key is an argument, like the character after "f").
QString keyFromEvent(const QKeyEvent* event);

// Splits a key sequence ("d2w", "ci(<Esc>", "<C-v>jI-<Esc>") into keys. "<" not starting a known
// "<...>" name is the character itself.
QStringList splitKeys(const QString& keys);

// Inverse helpers for synthesizing events (tests, :normal): the Qt key, modifiers and text a
// vim key corresponds to. Returns false for an unknown name.
bool keyToQt(const QString& key, int* qtKey, Qt::KeyboardModifiers* mods, QString* text);

} // namespace oc::vim
