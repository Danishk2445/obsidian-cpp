#pragma once

#include "text/changeset.h"
#include "text/selection.h"

#include <QString>

#include <optional>

namespace oc {

// A document update: changes (in the coordinates of the document before the transaction) plus the
// resulting selection for the originating view. userEvent follows CodeMirror conventions:
// "input.type", "input.type.compose", "input.paste", "input.drop", "delete.backward", "delete.forward",
// "delete.selection", "move.line", "undo", "redo", "external", "rename.links", "format", ...
struct Transaction {
    ChangeSet changes;
    std::optional<Selection> selection;
    QString userEvent;
    bool addToHistory = true;
    bool scrollIntoView = true;

    bool docChanged() const { return !changes.empty(); }
    bool isUserEvent(QStringView prefix) const
    {
        return userEvent == prefix || (userEvent.startsWith(prefix) && userEvent.size() > prefix.size() && userEvent[prefix.size()] == u'.');
    }
};

} // namespace oc
