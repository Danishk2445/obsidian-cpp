#pragma once

#include "editor/editor_view.h"
#include "ui/workspace/view.h"
#include "ui/workspace/workspace.h"

#include <QPointer>
#include <QTimer>

#include <functional>
#include <memory>

namespace oc {

class Document;
class EditorSearchBar;
class EditorSuggest;
class FoldStore;
class InlineTitle;
class ViewRegistry;
class VimController;

// Obsidian's MarkdownView: a note in Live Preview, Source mode or Reading view, sharing its Document
// with every other pane showing the same file. View state is Obsidian's
// {"file", "mode": "source"|"preview", "source": bool}; the ephemeral state carries the cursor and
// scroll position ({"cursor": {"from": {line, ch}, "to": ...}, "scroll": line}) and one-shot
// navigation requests ("subpath": "#Heading" / "#^block", "line", "match", "rename": "all"|"end",
// "focus").
class MarkdownView : public FileView {
    Q_OBJECT
public:
    explicit MarkdownView(WorkspaceLeaf* leaf, QWidget* parent = nullptr);
    ~MarkdownView() override;

    QString viewType() const override { return QStringLiteral("markdown"); }
    QString icon() const override { return QStringLiteral("file"); }
    bool canAcceptExtension(const QString& extension) const override;
    void loadFile(TFile* file) override;
    void onRename(TFile* file, const QString& oldPath) override;

    QJsonObject state() const override;
    void setState(const QJsonObject& state) override;
    QJsonObject ephemeralState() const override;
    void setEphemeralState(const QJsonObject& eState) override;
    QList<QWidget*> headerActions() override;
    void paneMenuEntries(QList<MenuEntry>& items, const QString& source) override;
    void onClose() override;
    void focusContent() override;

    EditorView* editor() const { return m_editor; }
    Document* document() const;
    EditorView::Mode mode() const { return m_editor->mode(); }
    void setMode(EditorView::Mode mode);
    void toggleReading();        // markdown:toggle-preview (Mod+E)
    void toggleSource();         // editor:toggle-source
    void showSearch(bool replace); // editor:open-search / editor:open-search-replace
    EditorSearchBar* searchBar() const { return m_searchBar; }
    InlineTitle* inlineTitle() const { return m_title; }
    EditorSuggest* editorSuggest() const { return m_suggest; }
    VimController* vim() const { return m_vim; } // while "vimMode" is on
    bool save();                 // editor:save-file
    void applySettings();        // re-read the vault config (readable line length, folding, ...)

    // What a click on a #tag does (Obsidian opens the global search with "tag:#tag"). Folds are
    // remembered in the session's FoldStore (a child object of the VaultSession).
    static void setTagHandler(std::function<void(const QString& tag, MarkdownView* from)> handler);
    // Obsidian's openLinkText: follow a link written in `sourcePath` (kind as EditorView::linkActivated;
    // URLs go to the desktop, unresolved notes are created, "#sub" alone targets the source note).
    static void openLinkText(Workspace* ws, const QString& target, int kind, const QString& sourcePath, bool newLeaf);
    static void openLinkText(Workspace* ws, const QString& target, int kind, const QString& sourcePath, Workspace::NewLeaf mode);

signals:
    void modeChanged(oc::EditorView::Mode mode);

private:
    void openLink(const QString& target, int kind, bool newLeaf);
    void addSpellingActions(QMenu* menu, qsizetype pos);
    void navigateToSubpath(const QString& subpath);
    void renameFromTitle(const QString& newName);
    FoldStore* foldStore() const;
    void saveFolds();
    void restoreFolds();
    void updateTitle();
    void setModeFromState(const QJsonObject& state);
    void setVimMode(bool on);

    EditorView* m_editor;
    EditorSearchBar* m_searchBar;
    InlineTitle* m_title;
    EditorSuggest* m_suggest;
    VimController* m_vim = nullptr;
    std::shared_ptr<Document> m_doc;
    QTimer m_foldSave;
    QMetaObject::Connection m_configConn;
    QMetaObject::Connection m_typingConn;
    QMetaObject::Connection m_resolvedConn;
    bool m_restoringFolds = false;
    bool m_previewAttached = false;
    bool m_spellcheck = false; // vault config "spellcheck" (applySettings)
    bool m_typing = false;     // the last change was typing at m_typingPos (spellcheck skips that word)
    qsizetype m_typingPos = -1;
};

// Registers the "markdown" view type for .md files.
void registerMarkdownView(ViewRegistry& registry);

} // namespace oc
