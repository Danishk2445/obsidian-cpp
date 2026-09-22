#pragma once

#include "editor/editor_view.h"
#include "ui/workspace/view.h"

#include <QPointer>
#include <QTimer>

#include <functional>
#include <memory>

namespace oc {

class Document;
class EditorSearchBar;
class FoldStore;
class InlineTitle;
class ViewRegistry;

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
    void populatePaneMenu(QMenu* menu) override;
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
    bool save();                 // editor:save-file
    void applySettings();        // re-read the vault config (readable line length, folding, ...)

    // What a click on a #tag does (Obsidian opens the global search with "tag:#tag"). Folds are
    // remembered in the session's FoldStore (a child object of the VaultSession).
    static void setTagHandler(std::function<void(const QString& tag, MarkdownView* from)> handler);

signals:
    void modeChanged(oc::EditorView::Mode mode);

private:
    void openLink(const QString& target, int kind, bool newLeaf);
    void navigateToSubpath(const QString& subpath);
    void renameFromTitle(const QString& newName);
    FoldStore* foldStore() const;
    void saveFolds();
    void restoreFolds();
    void updateTitle();
    void setModeFromState(const QJsonObject& state);

    EditorView* m_editor;
    EditorSearchBar* m_searchBar;
    InlineTitle* m_title;
    std::shared_ptr<Document> m_doc;
    QTimer m_foldSave;
    bool m_restoringFolds = false;
};

// Registers the "markdown" view type for .md files.
void registerMarkdownView(ViewRegistry& registry);

} // namespace oc
