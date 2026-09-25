// oc-vault-stats: read-only summary of how the clone indexes a vault (for acceptance checks against
// Obsidian): files, links/embeds (wikilink vs markdown), resolved/unresolved link counts, tags,
// headings, and the indexing time. Never writes anything (no clone config, nothing in the vault).
//
//   oc-vault-stats <vault folder> [--unresolved]   (--unresolved lists unresolved targets with counts)
#include "core/metadata/metadata_cache.h"
#include "core/vault/vault.h"
#include "markdown/metadata.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QTextStream>

#include <cstdio>

using namespace oc;

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() < 2 || !QFileInfo(args[1]).isDir()) {
        std::fprintf(stderr, "usage: oc-vault-stats <vault folder> [--unresolved]\n");
        return 2;
    }
    QTextStream out(stdout);
    QElapsedTimer t;
    t.start();
    Vault vault(QFileInfo(args[1]).absoluteFilePath());
    MetadataCache meta;
    meta.setContentProvider([&vault](const QString& path) -> std::optional<QByteArray> {
        QFile f(vault.absolutePath(path));
        return f.open(QIODevice::ReadOnly) ? std::optional(f.readAll()) : std::nullopt;
    });
    vault.load();
    const qint64 scanMs = t.elapsed();
    QStringList paths;
    for (TFile* f : vault.files())
        paths << f->path();
    meta.indexAll(paths);
    while (!meta.isReady())
        QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 50);
    meta.waitUntilClean();
    const qint64 indexMs = t.elapsed() - scanMs;

    int notes = 0, wikiLinks = 0, wikiEmbeds = 0, mdLinks = 0, mdEmbeds = 0, tags = 0, headings = 0, frontmatter = 0;
    for (TFile* f : vault.markdownFiles()) {
        ++notes;
        const md::CachedMetadata* c = meta.fileCache(f->path());
        if (!c)
            continue;
        for (const md::LinkCache& l : c->links)
            ++(l.original.startsWith(QLatin1String("[[")) ? wikiLinks : mdLinks);
        for (const md::LinkCache& l : c->embeds)
            ++(l.original.startsWith(QLatin1String("![[")) ? wikiEmbeds : mdEmbeds);
        tags += int(c->tags.size());
        headings += int(c->headings.size());
        frontmatter += c->frontmatter ? 1 : 0;
    }
    int resolved = 0, unresolved = 0;
    QHash<QString, int> unresolvedTargets;
    for (const auto& targets : meta.resolvedLinks())
        for (int n : targets)
            resolved += n;
    for (const auto& targets : meta.unresolvedLinks())
        for (auto it = targets.cbegin(); it != targets.cend(); ++it) {
            unresolved += it.value();
            unresolvedTargets[it.key()] += it.value();
        }
    out << "files " << vault.fileCount() << "  notes " << notes << "  folders " << vault.folders().size() << '\n'
        << "wikilinks " << wikiLinks << "  wiki embeds " << wikiEmbeds << "  markdown links " << mdLinks << "  markdown embeds "
        << mdEmbeds << '\n'
        << "resolved link occurrences " << resolved << "  unresolved occurrences " << unresolved << "  unresolved targets "
        << unresolvedTargets.size() << '\n'
        << "tags " << tags << "  headings " << headings << "  notes with frontmatter " << frontmatter << '\n'
        << "scan " << scanMs << " ms  index " << indexMs << " ms\n";
    if (args.contains(QStringLiteral("--unresolved")))
        for (auto it = unresolvedTargets.cbegin(); it != unresolvedTargets.cend(); ++it)
            out << "  " << it.value() << "\t" << it.key() << '\n';
    return 0;
}
