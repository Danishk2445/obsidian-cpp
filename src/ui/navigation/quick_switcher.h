#pragma once

#include "core/util/match_range.h"
#include "ui/navigation/navigation_util.h"
#include "ui/workspace/workspace.h"

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

namespace oc {

class OverlayLayer;
class SuggestModal;
class TFile;
class VaultSession;

// The switcher's options (Obsidian's switcher.json in the clone's per-vault config).
struct QuickSwitcherOptions {
    bool showExistingOnly = false; // hide link targets that don't exist yet
    bool showAttachments = true;   // images, and other files a view is registered for
    bool showAllFileTypes = false; // also files no view can open

    static QuickSwitcherOptions fromJson(const QJsonObject& json);
    QJsonObject toJson() const; // only non-default keys, like Obsidian's plugin data
};

// One quick switcher suggestion (Obsidian's "file" / "alias" / "unresolved" items and the null "create" item).
struct SwitcherItem {
    enum class Type { File, Alias, Unresolved, Create };
    Type type = Type::File;
    QString path;              // File, Alias: the file's vault path
    QString text;              // the title: display path (".md" dropped), alias, link text, or the typed query
    QList<MatchRange> matches; // fuzzy highlights in `text`
    double score = 0;
    bool downranked = false;   // an excluded file ("Excluded files" setting): muted, sorted lower
};

// What the switcher searches, captured from the session when it opens (tests fill it directly).
struct SwitcherSource {
    QStringList files;                   // every vault file
    QStringList recent;                  // Workspace::lastOpenFiles(), most recent first
    QHash<QString, QStringList> aliases; // file path -> frontmatter aliases
    QStringList unresolved;              // link texts of unresolved links, de-duplicated
    std::function<bool(const QString& extension)> canOpen; // a view is registered for the (lowercase) extension
    UserIgnoreFilters ignore;

    static SwitcherSource fromSession(VaultSession* session);
};

// Obsidian's QuickSwitcherModal.getSuggestions(). The query is trimmed. An empty query lists the recent files.
// Otherwise every shown file type is matched by its display path (FuzzyQuery::matchPath; prepareSimpleSearch
// semantics from 10000 files on, like Obsidian), aliases and, unless showExistingOnly, unresolved link texts are
// matched too; excluded files lose 10 points; results are sorted by score (stable: files by path, then aliases,
// then unresolved). A non-empty query without matches yields the single Create item.
// Shown file types: notes, canvases and bases always; images and files the app can open when showAttachments;
// anything with showAllFileTypes.
QList<SwitcherItem> quickSwitcherItems(const QString& query, const SwitcherSource& source,
                                       const QuickSwitcherOptions& options = {});

// Tab in the switcher: the input text that autocompletes `item` — its path up to the next '/' after the last
// match (a trailing '/' is added when that is already the query).
QString quickSwitcherCompletion(const SwitcherItem& item, const QString& query);

// Obsidian's workspace.openLinkText() as the switcher uses it: opens the note `linktext` resolves to from
// `sourcePath`, or creates it — in the folders named in the link text (created as needed, relative to the vault
// root), otherwise where "Default location for new notes" points — and opens it. A "#subpath" is applied after
// opening. Returns the opened file, or nullptr (with *error) when the name is invalid or the vault refused.
TFile* openOrCreateLinkText(VaultSession* session, const QString& linktext, const QString& sourcePath,
                            Workspace::NewLeaf mode, QString* error = nullptr);

// Opens the quick switcher (switcher:open, Mod+O) in `layer`: Obsidian's placeholder, instructions and keys
// (Enter opens, Mod+Enter in a new tab, Mod+Alt+Enter to the right, Shift+Enter creates, Tab autocompletes).
// Returns the already open switcher when there is one. `source` overrides what is searched (tests).
SuggestModal* openQuickSwitcher(VaultSession* session, OverlayLayer* layer, const SwitcherSource* source = nullptr);

} // namespace oc
