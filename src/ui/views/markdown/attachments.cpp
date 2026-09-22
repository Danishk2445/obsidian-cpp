#include "ui/views/markdown/attachments.h"

#include "core/config/vault_config.h"
#include "core/files/file_manager.h"
#include "core/metadata/link_suggest.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "editor/editor_hooks.h"
#include "editor/editor_view.h"
#include "ui/views/markdown/markdown_view.h"

#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QMimeData>
#include <QRegularExpression>
#include <QTextDocument>
#include <QUrl>

namespace oc::attachments {

using namespace Qt::StringLiterals;

namespace {
QString normalizePath(QString p)
{
    p.replace(QRegularExpression(u"[\\\\/]+"_s), u"/"_s);
    while (p.startsWith(u'/'))
        p.remove(0, 1);
    while (p.endsWith(u'/'))
        p.chop(1);
    return p;
}

MarkdownView* viewOf(EditorView& e)
{
    for (QWidget* w = e.parentWidget(); w; w = w->parentWidget())
        if (auto* mv = qobject_cast<MarkdownView*>(w))
            return mv;
    return nullptr;
}

QString embedFor(VaultSession& s, TFile* file, const TFile* source)
{
    FileManager* fm = s.fileManager();
    const QString link = fm ? fm->generateMarkdownLink(file->path(), source ? source->path() : QString()) : u"[["_s + file->path() + u"]]"_s;
    return u"!"_s + link;
}

void insertText(EditorView& e, const QString& text, const QString& userEvent)
{
    e.replaceSelections(text, userEvent);
}

// Files carried by the data: vault files are embedded, outside files copied in first.
bool insertFiles(MarkdownView& view, const QMimeData& data, const QString& userEvent)
{
    VaultSession* s = view.session();
    Vault* vault = s ? s->vault() : nullptr;
    if (!vault)
        return false;
    QStringList embeds;
    const QString root = QDir(vault->absolutePath(QString())).absolutePath();
    for (const QUrl& url : data.urls()) {
        if (!url.isLocalFile())
            continue;
        const QString abs = QFileInfo(url.toLocalFile()).absoluteFilePath();
        if (QFileInfo(abs).isDir())
            continue; // Obsidian imports whole folders; not supported here
        TFile* file = nullptr;
        if (abs.startsWith(root + u'/'))
            file = vault->file(abs.mid(root.size() + 1));
        if (!file) {
            QFile f(abs);
            if (!f.open(QIODevice::ReadOnly))
                continue;
            const QFileInfo fi(abs);
            file = saveAttachment(*s, fi.completeBaseName(), fi.suffix(), f.readAll(), view.file());
        }
        if (file)
            embeds << embedFor(*s, file, view.file());
    }
    if (embeds.isEmpty())
        return false;
    insertText(*view.editor(), embeds.join(u"\n\n"_s), userEvent);
    return true;
}

bool insertImage(MarkdownView& view, const QImage& image, const QString& userEvent)
{
    VaultSession* s = view.session();
    if (!s || image.isNull())
        return false;
    QByteArray png;
    QBuffer buf(&png);
    buf.open(QIODevice::WriteOnly);
    if (!image.save(&buf, "PNG"))
        return false;
    const QString name = u"Pasted image "_s + QDateTime::currentDateTime().toString(u"yyyyMMddHHmmss"_s);
    TFile* file = saveAttachment(*s, name, u"png"_s, png, view.file());
    if (!file)
        return false;
    insertText(*view.editor(), embedFor(*s, file, view.file()), userEvent);
    return true;
}

// HTML to Markdown (Obsidian: turndown; here Qt's converter, trimmed).
QString htmlToMarkdown(const QString& html)
{
    QTextDocument doc;
    doc.setHtml(html);
    QString md = doc.toMarkdown(QTextDocument::MarkdownDialectGitHub);
    while (md.endsWith(u'\n'))
        md.chop(1);
    return md;
}
} // namespace

QString availablePathFor(VaultSession& session, const QString& nameIn, const QString& ext, const TFile* source)
{
    Vault* vault = session.vault();
    VaultConfig* cfg = session.vaultConfig();
    QString folder = cfg ? cfg->string(u"attachmentFolderPath"_s) : u"/"_s;
    const QString sourceFolder = source && source->parent() ? source->parent()->path() : QString();
    if (folder == u"." || folder == u"./")
        folder = sourceFolder;
    else if (folder.startsWith(u"./"))
        folder = (sourceFolder.isEmpty() ? QString() : sourceFolder + u'/') + folder.mid(2);
    folder = normalizePath(folder);
    QString name = headingSubpathText(normalizePath(nameIn)).left(250);
    if (name.isEmpty())
        name = u"Untitled"_s;
    if (!folder.isEmpty() && vault && !vault->folder(folder))
        vault->createFolder(folder);
    return vault ? vault->availablePath(folder, name, ext) : (folder.isEmpty() ? name : folder + u'/' + name) + u'.' + ext;
}

TFile* saveAttachment(VaultSession& session, const QString& name, const QString& ext, const QByteArray& bytes, const TFile* source,
                      QString* error)
{
    Vault* vault = session.vault();
    if (!vault)
        return nullptr;
    return vault->createFile(availablePathFor(session, name, ext, source), bytes, error);
}

bool isUrl(const QString& text)
{
    static const QRegularExpression re(u"^[A-Za-z][A-Za-z0-9+.-]*:\\S*$"_s);
    return !text.isEmpty() && !text.contains(u' ') && re.match(text).hasMatch();
}

bool handlePaste(MarkdownView& view, const QMimeData& data)
{
    EditorView* e = view.editor();
    VaultSession* s = view.session();
    if (!e || !e->document() || !s)
        return false;
    // 1. Markdown / HTML text
    QString converted;
    if (data.hasFormat(u"text/markdown"_s)) {
        converted = QString::fromUtf8(data.data(u"text/markdown"_s));
    } else if (data.hasHtml() && s->vaultConfig() && s->vaultConfig()->flag(u"autoConvertHtml"_s)) {
        const QString html = data.html();
        // a lone <img> from a browser copy: paste the image file instead
        static const QRegularExpression loneImg(u"^\\s*(<meta[^>]*>\\s*)?<img [^>]+>\\s*$"_s, QRegularExpression::CaseInsensitiveOption);
        // Only HTML with document structure is converted: code editors and terminals put styled
        // <div>/<span> HTML on the clipboard, whose plain text is what the user means.
        static const QRegularExpression structure(u"<(a|b|strong|em|i|h[1-6]|ul|ol|li|table|img|code|pre|blockquote|p)[\\s>]"_s,
                                                  QRegularExpression::CaseInsensitiveOption);
        if (!(data.hasImage() && loneImg.match(html).hasMatch()) && structure.match(html).hasMatch())
            converted = htmlToMarkdown(html);
    }
    // 2. A URL pasted over a single-line selection becomes a link
    const QString text = !converted.isEmpty() ? converted : data.text();
    const Selection& sel = e->selection();
    if (!text.isEmpty() && isUrl(text.trimmed()) && !text.trimmed().contains(u'\n')) {
        const Text& t = e->document()->text();
        bool anySelected = false, singleLines = true;
        for (const SelRange& r : sel.ranges()) {
            anySelected |= !r.empty();
            singleLines &= t.lineAt(r.from()) == t.lineAt(r.to());
        }
        if (anySelected && singleLines) {
            const QString url = text.trimmed();
            QList<ChangeSpec> changes;
            for (const SelRange& r : sel.ranges())
                changes.append({r.from(), r.to(), r.empty() ? url : u"["_s + t.slice(r.from(), r.to()) + u"]("_s + url + u")"_s});
            Transaction tr;
            tr.changes = ChangeSet::of(changes, t.length());
            tr.userEvent = u"input.paste"_s;
            e->dispatch(tr);
            return true;
        }
    }
    if (!converted.isEmpty()) {
        e->pasteText(converted, u"input.paste"_s);
        return true;
    }
    // 3. Files and images
    if (data.hasUrls() && insertFiles(view, data, u"input.paste"_s))
        return true;
    if (data.hasImage() && !data.hasText())
        return insertImage(view, qvariant_cast<QImage>(data.imageData()), u"input.paste"_s);
    return false; // plain text: the editor's default paste
}

bool handleDrop(MarkdownView& view, const QMimeData& data)
{
    if (data.hasUrls() && insertFiles(view, data, u"input.drop"_s))
        return true;
    if (data.hasImage() && !data.hasText())
        return insertImage(view, qvariant_cast<QImage>(data.imageData()), u"input.drop"_s);
    return false;
}

void installHooks()
{
    static const bool once = [] {
        EditorHooks::global().addPasteHandler([](EditorView& e, const QMimeData& d) {
            MarkdownView* mv = viewOf(e);
            return mv && handlePaste(*mv, d);
        });
        EditorHooks::global().addDropHandler([](EditorView& e, const QMimeData& d, qsizetype) {
            MarkdownView* mv = viewOf(e);
            return mv && handleDrop(*mv, d);
        });
        return true;
    }();
    Q_UNUSED(once);
}

} // namespace oc::attachments
