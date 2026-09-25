#include "core/plugins/note_composer.h"

#include "core/config/config_store.h"
#include "core/config/vault_config.h"
#include "core/documents/document.h"
#include "core/documents/document_manager.h"
#include "core/files/file_manager.h"
#include "core/metadata/link_updater.h"
#include "core/metadata/metadata_cache.h"
#include "core/metadata/subpath.h"
#include "core/plugins/note_plugins.h"
#include "core/vault/vault.h"
#include "core/vault_session.h"
#include "markdown/metadata.h"
#include "text/codec.h"

#include <QFileInfo>
#include <QRegularExpression>

namespace oc::plugins {

using namespace Qt::StringLiterals;

namespace {

QString linkpathOf(const QString& link)
{
    const qsizetype hash = link.indexOf(u'#');
    return hash < 0 ? link : link.left(hash);
}

QString subpathOf(const QString& link)
{
    const qsizetype hash = link.indexOf(u'#');
    return hash < 0 ? QString() : link.mid(hash);
}

bool isWikilink(const md::LinkCache& l)
{
    return l.original.startsWith(u"[["_s) || l.original.startsWith(u"![["_s);
}

QList<const md::LinkCache*> bodyLinks(const md::CachedMetadata& meta)
{
    QList<const md::LinkCache*> out;
    for (const md::LinkCache& l : meta.links)
        out << &l;
    for (const md::LinkCache& l : meta.embeds)
        out << &l;
    return out;
}

QString basenameOf(const QString& path)
{
    return QFileInfo(path).completeBaseName();
}

// The note composer template's text, or nullopt (with a notice) when it is set but missing.
std::optional<QString> templateText(VaultSession* s, const NoteComposerOptions& o)
{
    if (o.templatePath.isEmpty())
        return std::nullopt;
    QString path = o.templatePath;
    while (path.startsWith(u'/'))
        path.remove(0, 1);
    if (QFileInfo(path).suffix().isEmpty())
        path += u".md"_s;
    Vault* v = s->vault();
    if (!v || !v->file(path)) {
        if (FileManager* fm = s->fileManager())
            emit fm->notice(QObject::tr("Failed to fetch template file: “%1” not found.").arg(path));
        return std::nullopt;
    }
    if (DocumentManager* dm = s->documents())
        if (Document* d = dm->find(path))
            return d->text().toString();
    const std::optional<QByteArray> bytes = v->read(path, nullptr, nullptr);
    return bytes ? std::optional(decodeFile(*bytes).text.toString()) : std::nullopt;
}

QString composed(VaultSession* s, const QString& content, const QString& fromTitle, const QString& newTitle)
{
    const NoteComposerOptions o = NoteComposerOptions::fromJson(s->config() ? s->config()->object(u"note-composer"_s) : QJsonObject());
    const std::optional<QString> t = templateText(s, o);
    return t ? applyComposerTemplate(*t, content, fromTitle, newTitle) : content;
}

LinkResolver::LinkFormat linkFormat(VaultSession* s)
{
    return s->vaultConfig() ? s->vaultConfig()->newLinkFormat() : LinkResolver::LinkFormat::Shortest;
}

Text currentText(VaultSession* s, const QString& path)
{
    if (DocumentManager* dm = s->documents())
        if (Document* d = dm->find(path))
            return d->text();
    const std::optional<QByteArray> bytes = s->vault() ? s->vault()->read(path, nullptr, nullptr) : std::nullopt;
    return bytes ? decodeFile(*bytes).text : Text();
}

} // namespace

NoteComposerOptions NoteComposerOptions::fromJson(const QJsonObject& o)
{
    NoteComposerOptions r;
    r.askBeforeMerging = o.value(u"askBeforeMerging"_s).toBool(true);
    const QString rep = o.value(u"replacementText"_s).toString();
    if (rep == u"embed" || rep == u"none" || rep == u"link")
        r.replacementText = rep;
    r.templatePath = o.value(u"template"_s).toString().trimmed();
    return r;
}

std::optional<QString> headingTextOf(const QString& line)
{
    static const QRegularExpression re(u"^#{1,6} (.*)"_s, QRegularExpression::MultilineOption);
    const QRegularExpressionMatch m = re.match(line);
    return m.hasMatch() ? std::optional(m.captured(1)) : std::nullopt;
}

std::optional<HeadingRange> headingRangeAt(const md::CachedMetadata& meta, const QString& text, int line)
{
    const md::HeadingCache* current = nullptr;
    const md::HeadingCache* next = nullptr;
    for (const md::HeadingCache& h : meta.headings) {
        if (current && h.level <= current->level) {
            next = &h;
            break;
        }
        if (!current && h.position.start.line == line)
            current = &h;
    }
    if (!current)
        return std::nullopt;
    HeadingRange r;
    r.heading = current->heading.trimmed();
    r.start = current->position.start.offset;
    if (next) {
        r.end = next->position.start.offset;
        while (r.end > r.start + 1 && text.at(r.end - 1) == u'\n' && text.at(r.end - 2) == u'\n')
            --r.end;
    } else {
        r.end = text.size();
    }
    return r;
}

QString applyComposerTemplate(QString templateText, const QString& content, const QString& fromTitle, const QString& newTitle,
                              const QDateTime& now)
{
    if (!templateText.contains(u"{{content}}"_s))
        templateText += u"\n\n{{content}}"_s;
    const QHash<QString, QString> vars{{u"fromTitle"_s, fromTitle}, {u"newTitle"_s, newTitle}, {u"content"_s, content}};
    return applyTemplate(templateText, vars, QString(), QString(), now);
}

std::optional<qsizetype> subpathOffset(const md::CachedMetadata& meta, const Text& text, const QString& subpath)
{
    const std::optional<SubpathRange> r = resolveSubpath(meta, subpath);
    if (!r || r->startLine < 0 || r->startLine >= text.lineCount())
        return std::nullopt;
    return text.lineStart(r->startLine); // the heading line / the block's (list item's) first line
}

QString rewriteLinksForDestination(const md::CachedMetadata& meta, const Text& text, const QString& sourcePath,
                                   const QString& destPath, qsizetype from, qsizetype to, const LinkResolver& resolver,
                                   LinkResolver::LinkFormat format)
{
    QString content = text.slice(from, to);
    QList<ChangeSpec> edits;
    for (const md::LinkCache* l : bodyLinks(meta)) {
        const qsizetype a = l->position.start.offset, b = l->position.end.offset;
        if (a < from || b > to)
            continue;
        const QString subpath = subpathOf(l->link);
        const QString path = linkpathOf(l->link);
        const bool wiki = isWikilink(*l);
        bool self = path.isEmpty();
        QString newLink;
        if (!path.isEmpty()) {
            const QString target = resolver.resolve(path, sourcePath, l->markdown);
            if (target.isEmpty())
                continue; // unresolved: left as written
            if (target == sourcePath)
                self = true;
            else
                newLink = resolver.linkText(target, destPath, format, wiki) + subpath;
        }
        if (self) {
            if (subpath.isEmpty())
                continue;
            const std::optional<qsizetype> at = subpathOffset(meta, text, subpath);
            if (!at)
                continue;
            newLink = *at >= from && *at <= to ? subpath : resolver.linkText(sourcePath, destPath, format, wiki) + subpath;
        }
        if (newLink != l->link)
            edits.push_back({a - from, b - from, LinkUpdater::rewrite(*l, newLink)});
    }
    std::sort(edits.begin(), edits.end(), [](const ChangeSpec& x, const ChangeSpec& y) { return x.from > y.from; });
    for (const ChangeSpec& e : std::as_const(edits))
        content.replace(e.from, e.to - e.from, e.insert);
    return content;
}

QList<ChangeSpec> pathlessLinkUpdatesForSource(const md::CachedMetadata& meta, const Text& text, const QString& sourcePath,
                                               const QString& destPath, qsizetype from, qsizetype to,
                                               const LinkResolver& resolver, LinkResolver::LinkFormat format)
{
    QList<ChangeSpec> out;
    for (const md::LinkCache* l : bodyLinks(meta)) {
        const qsizetype a = l->position.start.offset, b = l->position.end.offset;
        if (a >= from && b <= to)
            continue; // moves along
        const QString path = linkpathOf(l->link);
        if (!path.isEmpty() && resolver.resolve(path, sourcePath, l->markdown) != sourcePath)
            continue;
        const QString subpath = subpathOf(l->link);
        if (subpath.isEmpty())
            continue;
        const std::optional<qsizetype> at = subpathOffset(meta, text, subpath);
        if (!at || *at < from || *at > to)
            continue;
        const QString newLink = resolver.linkText(destPath, sourcePath, format, isWikilink(*l)) + subpath;
        if (newLink != l->link)
            out.push_back({a, b, LinkUpdater::rewrite(*l, newLink)});
    }
    return out;
}

bool mergeNotes(VaultSession* s, TFile* from, TFile* into, bool prepend, QString* error)
{
    FileManager* fm = s ? s->fileManager() : nullptr;
    if (!fm || !from || !into || from == into)
        return false;
    const QString fromPath = from->path(), intoPath = into->path();
    const QString content = composed(s, currentText(s, fromPath).toString(), basenameOf(fromPath), basenameOf(intoPath));
    if (!fm->insertIntoFile(into, content, prepend, error))
        return false;
    return fm->runLinkUpdate(
        [&] {
            Vault* v = s->vault();
            TFile* f = v ? v->file(fromPath) : nullptr;
            return f && fm->trashFile(f, error);
        },
        [fromPath, intoPath](const LinkUpdater::Reference& r) { return r.resolvedPaths.value(0) == fromPath ? intoPath : QString(); },
        {fromPath});
}

bool extractToNote(VaultSession* s, TFile* source, qsizetype from, qsizetype to, TFile* dest, bool prepend, QString* error)
{
    FileManager* fm = s ? s->fileManager() : nullptr;
    MetadataCache* mc = s ? s->metadata() : nullptr;
    if (!fm || !mc || !source || !dest || source == dest || to < from)
        return false;
    const QString sourcePath = source->path(), destPath = dest->path();
    // The source's metadata must describe its current text.
    if (DocumentManager* dm = s->documents())
        dm->saveAll();
    mc->waitUntilClean();
    const Text text = currentText(s, sourcePath);
    if (to > text.length())
        return false;
    const md::CachedMetadata* cached = mc->fileCache(sourcePath);
    const md::CachedMetadata meta = cached ? *cached : md::CachedMetadata();
    const LinkResolver::LinkFormat format = linkFormat(s);
    const QString moved = rewriteLinksForDestination(meta, text, sourcePath, destPath, from, to, mc->resolver(), format);
    const QString content = composed(s, moved, basenameOf(sourcePath), basenameOf(destPath));
    QList<ChangeSpec> sourceEdits = pathlessLinkUpdatesForSource(meta, text, sourcePath, destPath, from, to, mc->resolver(), format);
    const NoteComposerOptions o = NoteComposerOptions::fromJson(s->config() ? s->config()->object(u"note-composer"_s) : QJsonObject());
    const QString link = fm->generateMarkdownLink(destPath, sourcePath);
    const QString replacement = o.replacementText == u"embed" ? u"!"_s + link : o.replacementText == u"none" ? QString() : link;
    sourceEdits.push_back({from, to, replacement});
    const QString expected = text.slice(from, to);

    if (!fm->insertIntoFile(dest, content, prepend, error))
        return false;
    return fm->runLinkUpdate(
        [&] {
            return fm->editFile(
                sourcePath,
                [&](const Text& now) -> QList<ChangeSpec> {
                    if (now.length() != text.length() || now.slice(from, to) != expected)
                        return {}; // changed meanwhile: leave it
                    return sourceEdits;
                },
                error);
        },
        [&](const LinkUpdater::Reference& r) -> QString {
            if (r.resolvedPaths.value(0) != sourcePath)
                return {};
            const QString subpath = subpathOf(r.link.link);
            if (subpath.isEmpty())
                return {};
            const std::optional<qsizetype> at = subpathOffset(meta, text, subpath);
            return at && *at >= from && *at <= to ? destPath : QString();
        },
        {sourcePath});
}

} // namespace oc::plugins
