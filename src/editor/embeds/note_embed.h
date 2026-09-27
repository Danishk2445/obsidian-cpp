#pragma once

// Note embeds ("![[Note]]", "![[Note#Heading]]", "![[Note#Heading#Sub]]", "![[Note#^block]]",
// "![[Note#[^1]]]", "![](Note.md#Heading)"), rendered like Obsidian's .markdown-embed.inline-embed:
//  - a box with --embed-border-start (2px accent) on the left and --embed-padding inside (24px on
//    the left); the whole note shows its name as a bold .markdown-embed-title line (the inline title
//    and the properties stay hidden, the frontmatter isn't rendered); a heading embed shows the
//    heading and its section, a block embed the block (a list item with its children, un-indented),
//    a footnote embed the definition's text; a subpath that doesn't resolve shows the heading
//    "##### Unable to find “x” in Note";
//  - the content is the note rendered like the reading view, at most --embed-max-height (4000px)
//    tall: taller content scrolls inside the live widget; print (PDF export) shows all of it;
//  - an "Open link" icon (maximize-2) at the top right opens the note at the subpath;
//  - nested embeds render down to depth 5 (Obsidian's limit); deeper ones, and embeds of a note
//    section that is already being embedded above them (cycles), show the generic file box
//    ("Note.md", .file-embed.mod-generic);
//  - an embed of a note that doesn't exist shows “Note” is not created yet. Click to create.
// Text comes from the open Document when there is one (unsaved edits show), else from the file.
// Layouts (sizes included) are cached by content hash + width + nesting chain; hosts report note
// changes (noteChanged) and the embeds showing them re-lay out through EmbedRegistry::notifyChanged.
//
// This is the engine (size, static paint, hit testing, caches). The live widget, link opening,
// hover previews and the vault wiring are in the UI (ui/views/markdown/note_embed_view.h).

#include "editor/embeds/embed_registry.h"
#include "render/layout/doc_layout.h"
#include "render/layout/style_sheet.h"

#include <QHash>
#include <QObject>
#include <QSet>

#include <memory>
#include <optional>

namespace oc {

class Document;
class LivePreviewDecorator;

namespace md {
class StaticDocSource;
}

// What note embeds need from a vault: path mapping, open documents and link resolution. The UI
// registers one per open vault (NoteEmbeds::addHost); files outside every host are read from disk
// and the links in them are left unresolved.
class NoteEmbedHost {
public:
    virtual ~NoteEmbedHost() = default;
    virtual std::optional<QString> vaultPath(const QString& absPath) const = 0; // nullopt: not in this vault
    virtual QString absolutePath(const QString& vaultPath) const = 0;
    virtual const Document* openDocument(const QString& vaultPath) const = 0;   // loaded Document or null
    // Target (vault path) of a link path (no subpath) written in `sourcePath`, "" when unresolved.
    virtual QString resolveLink(const QString& linkpath, const QString& sourcePath) const = 0;
    // The vault's rendering options (app.json "showIndentGuide" / "strictLineBreaks"); embeds of its notes render
    // with them. When they change, the host calls NoteEmbeds::linksChanged() so cached layouts are rebuilt.
    virtual bool showIndentGuide() const { return true; }
    virtual bool strictLineBreaks() const { return false; }
};

// One embed rendered at one width: the box, the title, the laid-out content, the link icon.
// Coordinates are relative to the box's top-left. Shared (cached); GUI thread only.
class NoteEmbedLayout {
public:
    enum class Kind {
        Note,    // .markdown-embed
        Generic, // too deep / cycle: .file-embed.mod-generic ("Note.md")
        Missing, // unresolved: .file-embed.mod-empty ("“Note” is not created yet. Click to create.")
    };
    struct Hit {
        enum class Type { None, Content, Link, OpenIcon, Box };
        Type type = Type::None;
        QString linktext;   // Link: the link as written; OpenIcon: "<vault path><subpath>"; Box: the embed's link
        QString sourcePath; // vault path the link is resolved from ("" for OpenIcon / top-level boxes)
        int linkKind = 0;   // EditorView::linkActivated kinds: 0 wikilink, 1 embed, 2 markdown link, 3 url, 4 tag
        QRectF rect;        // the hit link / icon / box (layout coordinates)
    };

    ~NoteEmbedLayout();

    Kind kind() const { return m_kind; }
    const EmbedRequest& request() const { return m_request; }
    QString vaultPath() const { return m_vaultPath; }  // "" outside every host
    QString title() const { return m_title; }          // the .markdown-embed-title ("" for sections)
    QString markdown() const;                          // the rendered markdown (section / message)
    bool subpathFound() const { return m_found; }
    int depth() const { return m_depth; }              // 1 = embedded in a note
    QSizeF size() const { return m_size; }
    qreal contentTop() const { return m_contentTop; }  // below the title
    qreal viewportHeight() const { return m_viewportH; } // the visible content (<= --embed-max-height)
    qreal contentHeight() const;                       // all content (estimated beyond what's laid out)
    bool scrollable() const { return m_scrollable; }
    qreal maxScroll() const;
    QRectF contentRect() const;                        // the content viewport
    QRectF iconRect() const { return m_iconRect; }     // the "Open link" icon (empty in print)

    // Measure the content visible at `scroll` (lazy layout of tall embeds).
    void ensureLaidOut(qreal scroll);
    // `origin` = the box's top-left; `visible` limits painting (layout coordinates, empty = all);
    // `icon`: paint the "Open link" icon (the live widget has a button there); `hover`: the mouse is
    // over a generic / "not created yet" box.
    void paint(QPainter* p, const QPointF& origin, qreal scroll, const QRectF& visible = {}, bool icon = true, bool hover = false);
    Hit hitTest(const QPointF& pos, qreal scroll = 0);
    // Does it show `absPath` (itself or in a nested embed)?
    bool dependsOn(const QString& absPath) const { return m_deps.contains(absPath); }

private:
    friend class NoteEmbeds;
    NoteEmbedLayout() = default;
    void paintBox(QPainter* p, const QRectF& box, bool hover) const;

    Kind m_kind = Kind::Note;
    EmbedRequest m_request;
    QString m_vaultPath;
    QString m_title;
    bool m_found = true;
    bool m_print = false;
    int m_depth = 1;
    QSizeF m_size;
    qreal m_contentLeft = 0, m_contentTop = 0, m_contentWidth = 0, m_viewportH = 0;
    bool m_scrollable = false;
    QRectF m_iconRect;
    QStringList m_childChain; // the nesting chain of embeds inside this one (ancestors + this)
    StyleSheetPtr m_styles;
    std::shared_ptr<const Theme> m_theme;
    std::unique_ptr<md::StaticDocSource> m_source;
    std::unique_ptr<LivePreviewDecorator> m_decorator;
    std::unique_ptr<DocLayout> m_layout;
    // validity
    QHash<QString, quint64> m_deps; // absPath -> content hash of every note rendered (this + nested)
    quint64 m_linkGeneration = 0;
    quint64 m_styleKey = 0;
    int m_options = -1; // the host's rendering options it was built with (NoteEmbeds::renderOptions)
    quint64 m_lastUse = 0;
};

class NoteEmbeds : public QObject {
    Q_OBJECT
public:
    static NoteEmbeds& instance();

    static constexpr int kMaxDepth = 5; // Obsidian renders embeds down to depth 5

    void addHost(std::shared_ptr<NoteEmbedHost> host);
    void removeHost(const NoteEmbedHost* host);
    NoteEmbedHost* hostFor(const QString& absPath, QString* vaultPath = nullptr) const;

    // A note's text changed (edited, saved, modified on disk, renamed, deleted): its snapshot is
    // dropped and every embed showing it (directly or nested) is re-laid out.
    void noteChanged(const QString& absPath);
    // Link resolution changed (files created / renamed / deleted, metadata resolved): cached layouts
    // are rebuilt on next use.
    void linksChanged();

    // The layout of `request` at `width` inside the current nesting chain (empty at the top level,
    // set while an embed lays out / paints / hit-tests its content).
    std::shared_ptr<NoteEmbedLayout> layout(const EmbedRequest& request, qreal width);
    // Content hash of the note's current text (0 when unreadable).
    quint64 contentHash(const QString& absPath);

    int cachedLayouts() const { return int(m_layouts.size()); } // tests
    int builds() const { return m_builds; }                     // tests: layouts built so far
    QList<std::shared_ptr<NoteEmbedLayout>> cachedLayoutsOf(const QString& absPath) const; // tests
    void clearCaches();

signals:
    // An embedded note changed (absPath) or links were re-resolved ("" = anything): live widgets repaint.
    void changed(const QString& absPath);

private:
    NoteEmbeds() = default;
    struct Snapshot;
    std::shared_ptr<Snapshot> snapshot(const QString& absPath);
    std::shared_ptr<NoteEmbedLayout> build(const EmbedRequest& request, qreal width, const QStringList& chain);
    bool valid(NoteEmbedLayout& l);
    int renderOptions(const QString& absPath) const; // the host's showIndentGuide / strictLineBreaks as bits
    void trim();
    StyleSheetPtr currentStyles(std::shared_ptr<const Theme>* theme);

    struct ChainScope;
    friend class NoteEmbedLayout;

    QList<std::shared_ptr<NoteEmbedHost>> m_hosts;
    QHash<QString, std::shared_ptr<Snapshot>> m_snapshots;
    QHash<QString, std::shared_ptr<NoteEmbedLayout>> m_layouts;
    QStringList m_chain;                       // current nesting chain (see layout())
    QList<QHash<QString, quint64>*> m_collect; // deps collected by the layouts being built
    quint64 m_linkGeneration = 1;
    quint64 m_clock = 0;
    int m_builds = 0;
    StyleSheetPtr m_styles;
    std::shared_ptr<const Theme> m_stylesTheme;
};

// The "md" embed provider: size / static paint from NoteEmbeds; the UI subclass adds the live widget.
class NoteEmbedProvider : public EmbedProvider {
public:
    QSizeF size(const EmbedRequest& request, qreal maxWidth) const override;
    void paint(QPainter* p, const QRectF& rect, const EmbedRequest& request) const override;
    QWidget* createWidget(const EmbedContext& context, QWidget* parent) const override;
    quint64 signature(const EmbedRequest& request) const override;
};

} // namespace oc
