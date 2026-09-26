#pragma once

// Obsidian's embedRegistry: file extensions -> embed providers (PDF, audio, video, notes...). The
// editor only knows the interface; the media module (ui/views/media) and the note embeds
// (editor/embeds/note_embed.h, "md") register the providers.
//
// An embed ("![[doc.pdf#page=3]]", "![](a.mp3)", "![[Note#Heading]]") is laid out as an EmbedWidget
// (an inline widget of the provider's size that paints a static rendering) and, in an EditorView,
// the provider's live QWidget is hosted over it while it is near the viewport (see hosted_embeds.h).

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSizeF>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

class QPainter;
class QRectF;
class QWidget;

namespace oc {

class InlineWidget;
class StyleSheet;
class Theme;

// One embed of a vault file, as written in a note.
struct EmbedRequest {
    enum class Display { Inline, Popover };

    QString absPath;   // the resolved file on disk
    QString linktext;  // the link as written, target + subpath ("doc.pdf#page=3"), for opening it
    QString subpath;   // "#page=3&height=400" ("" = none)
    QString extension; // lowercase, without the dot
    int width = -1;    // "|300" / "|300x200" of the link (-1 = none)
    int height = -1;
    Display display = Display::Inline;
    // (additive) Code blocks rendered by a provider (see EmbedRegistry::registerCodeBlockLanguage):
    // the fence's language and content; absPath is empty.
    QString language;
    QString source;

    // Identity of the embedded content (file + subpath + display) for pooling hosted widgets.
    QString key() const;
    // Obsidian parses PDF subpaths as URL search params: "#page=3&height=400".
    bool hasParam(const QString& name) const;
    QString param(const QString& name) const;
    int intParam(const QString& name, int fallback = -1) const; // integer prefix like parseInt()

    static EmbedRequest fromLink(const QString& absPath, const QString& target, const QString& subpath);
};

// What a live embed widget may ask of the view that shows it.
struct EmbedContext {
    EmbedRequest request;
    QPointer<QWidget> view;             // the EditorView hosting the embed (null in popovers)
    bool livePreview = false;           // the note is editable: offer the "edit" (reveal source) action
    std::function<void()> revealSource; // put the cursor into the embed's markdown (Live Preview)
};

class EmbedProvider {
public:
    virtual ~EmbedProvider() = default;
    // Size of the embed when `maxWidth` px are available (the note's text column).
    virtual QSizeF size(const EmbedRequest& request, qreal maxWidth) const = 0;
    // Static rendering: under the hosted widget, in nested renderers (callouts) and PDF export.
    virtual void paint(QPainter* p, const QRectF& rect, const EmbedRequest& request) const = 0;
    // The live widget. It is parented and positioned by the host.
    virtual QWidget* createWidget(const EmbedContext& context, QWidget* parent) const = 0;
    // Part of the layout cache key beyond the request (e.g. the file's mtime or probed size).
    virtual quint64 signature(const EmbedRequest& request) const { Q_UNUSED(request); return 0; }
};

class EmbedRegistry : public QObject {
    Q_OBJECT
public:
    static EmbedRegistry& instance();

    void registerExtensions(const QStringList& extensions, std::shared_ptr<EmbedProvider> provider); // lowercase
    void unregisterExtensions(const QStringList& extensions);
    std::shared_ptr<EmbedProvider> provider(const QString& extension) const;
    bool isExtensionRegistered(const QString& extension) const { return m_providers.contains(extension.toLower()); }
    bool isEmpty() const { return m_providers.isEmpty(); }

    // The rendering of `absPath` changed (file modified, a video's dimensions became known...):
    // views showing embeds of it re-decorate those lines. Code block embeds have an empty absPath.
    void notifyChanged(const QString& absPath);

    // (additive) Fenced code blocks a provider renders (Obsidian's registerCodeBlockPostProcessor, e.g.
    // Bases' ```base): while the cursor is outside the fence, Live Preview and Reading show the
    // provider's embed (EmbedRequest::language / source) in place of the code.
    void registerCodeBlockLanguage(const QString& language, std::shared_ptr<EmbedProvider> provider); // lowercase
    void unregisterCodeBlockLanguage(const QString& language);
    std::shared_ptr<EmbedProvider> codeBlockProvider(const QString& language) const;
    bool isCodeBlockLanguageRegistered(const QString& language) const { return m_codeBlocks.contains(language.toLower()); }
    bool isEmptyOfCodeBlocks() const { return m_codeBlocks.isEmpty(); }

    // Opening an embedded file from its embed (Obsidian's link context menu): mode "" = the current
    // tab, "tab" = a new tab, "split" = to the right. `from` is the embed's widget (its ancestors tell
    // the workspace). The UI installs the opener.
    using Opener = std::function<void(QWidget* from, const EmbedRequest& request, const QString& mode)>;
    void setOpener(Opener opener) { m_opener = std::move(opener); }
    void open(QWidget* from, const EmbedRequest& request, const QString& mode) const
    {
        if (m_opener)
            m_opener(from, request, mode);
    }

signals:
    void changed(const QString& absPath);

private:
    EmbedRegistry() = default;
    QHash<QString, std::shared_ptr<EmbedProvider>> m_providers;
    QHash<QString, std::shared_ptr<EmbedProvider>> m_codeBlocks;
    Opener m_opener;
};

// The inline widget for an embed of `target` + `subpath` whose file resolves to `absPath` (the
// decorator's resolveAttachment), or nullptr when no provider handles its extension.
std::shared_ptr<InlineWidget> makeEmbedWidget(const QString& absPath, const QString& target, const QString& subpath, int width,
                                              int height, qreal maxWidth);

// The inline widget for an embed as written in a note ("![[target#subpath|WxH]]", "![](target#subpath)"),
// or nullptr when no provider handles it: a target whose own extension is registered ("doc.pdf")
// goes to that provider; otherwise the target is resolved (`resolve`: link path -> absolute path,
// "" when unresolved) and the file's extension decides, so notes ("![[Note#Heading]]") reach the
// "md" provider. An unresolved note ("![[New note]]": no extension, or ".md") goes to the "md"
// provider with an empty absPath (Obsidian's "not created yet" embed); image targets are left to
// the caller.
std::shared_ptr<InlineWidget> makeLinkEmbedWidget(const std::function<QString(const QString& linkpath)>& resolve, const QString& target,
                                                  const QString& subpath, int width, int height, qreal maxWidth);

// (additive) The inline widget for a fenced code block of a registered language (`source` = the fence's
// content), or nullptr when no provider renders the language.
std::shared_ptr<InlineWidget> makeCodeBlockEmbedWidget(const QString& language, const QString& source, qreal maxWidth);

// While alive, embeds are laid out and painted for print (PDF export; Obsidian's @media print rules):
// providers drop their actions and max-heights and render with the export's theme / styles (light).
// Scopes nest; GUI thread only.
class EmbedPrintScope {
public:
    EmbedPrintScope(std::shared_ptr<const Theme> theme, std::shared_ptr<const StyleSheet> styles);
    ~EmbedPrintScope();
    EmbedPrintScope(const EmbedPrintScope&) = delete;
    EmbedPrintScope& operator=(const EmbedPrintScope&) = delete;

    static bool active();
    static std::shared_ptr<const Theme> theme();        // null outside a scope
    static std::shared_ptr<const StyleSheet> styles();  // null outside a scope

private:
    std::shared_ptr<const Theme> m_prevTheme;
    std::shared_ptr<const StyleSheet> m_prevStyles;
    bool m_prevActive = false;
};

} // namespace oc
