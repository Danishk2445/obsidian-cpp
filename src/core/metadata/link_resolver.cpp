#include "core/metadata/link_resolver.h"

#include <QFileInfo>

#include <algorithm>

namespace oc {

namespace {
bool isMarkdown(const QString& path) { return path.endsWith(QLatin1String(".md"), Qt::CaseInsensitive); }
int segments(const QString& path) { return int(path.count(QLatin1Char('/'))); }
} // namespace

QString LinkResolver::nameKey(const QString& path)
{
    const QString name = path.mid(path.lastIndexOf(QLatin1Char('/')) + 1);
    if (isMarkdown(name))
        return name.left(name.size() - 3).toLower();
    return name.toLower();
}

QString LinkResolver::folderOf(const QString& path)
{
    const qsizetype slash = path.lastIndexOf(QLatin1Char('/'));
    return slash < 0 ? QString() : path.left(slash);
}

QString LinkResolver::normalize(const QString& path)
{
    QStringList out;
    for (const QString& seg : path.split(QLatin1Char('/'))) {
        if (seg.isEmpty() || seg == u".")
            continue;
        if (seg == u"..") {
            if (!out.isEmpty())
                out.removeLast();
            continue;
        }
        out << seg;
    }
    return out.join(QLatin1Char('/'));
}

void LinkResolver::setFiles(const QStringList& paths)
{
    m_byPath.clear();
    m_byName.clear();
    for (const QString& p : paths)
        addFile(p);
}

void LinkResolver::addFile(const QString& path)
{
    m_byPath.insert(path.toLower(), path);
    QStringList& l = m_byName[nameKey(path)];
    if (!l.contains(path))
        l.push_back(path);
}

void LinkResolver::removeFile(const QString& path)
{
    m_byPath.remove(path.toLower());
    auto it = m_byName.find(nameKey(path));
    if (it != m_byName.end()) {
        it->removeAll(path);
        if (it->isEmpty())
            m_byName.erase(it);
    }
}

QPair<QString, QString> LinkResolver::splitSubpath(const QString& link)
{
    const qsizetype hash = link.indexOf(QLatin1Char('#'));
    if (hash < 0)
        return {link.trimmed(), QString()};
    return {link.left(hash).trimmed(), link.mid(hash)};
}

QStringList LinkResolver::candidates(const QString& rawLinkpath, const QString& sourcePath) const
{
    QString linkpath = rawLinkpath.trimmed();
    while (linkpath.startsWith(QLatin1Char('/')))
        linkpath.remove(0, 1);
    if (linkpath.isEmpty())
        return {};
    const QString lower = linkpath.toLower();
    const bool hasExt = QFileInfo(linkpath).suffix().size() > 0 && !linkpath.endsWith(QLatin1Char('.'));
    // Name-based candidates whose path ends with the link path.
    const QString key = nameKey(hasExt && isMarkdown(linkpath) ? linkpath : (hasExt ? linkpath : linkpath + QStringLiteral(".md")));
    QStringList out;
    const QString suffixMd = QLatin1Char('/') + lower + QStringLiteral(".md");
    const QString suffix = QLatin1Char('/') + lower;
    for (const QString& p : m_byName.value(key)) {
        const QString pl = p.toLower();
        if (pl == lower || pl == lower + QStringLiteral(".md") || pl.endsWith(suffix) || pl.endsWith(suffixMd))
            out << p;
    }
    // Without an extension, a non-markdown file of that exact name also matches ("[[image.png]]" has an ext,
    // but "[[notes]]" can't match "notes.pdf").
    const QString srcFolder = folderOf(sourcePath).toLower();
    std::sort(out.begin(), out.end(), [&](const QString& a, const QString& b) {
        const bool sa = folderOf(a).toLower() == srcFolder, sb = folderOf(b).toLower() == srcFolder;
        if (sa != sb)
            return sa;
        const int ga = segments(a), gb = segments(b);
        if (ga != gb)
            return ga < gb;
        return a.localeAwareCompare(b) < 0;
    });
    return out;
}

QString LinkResolver::resolve(const QString& rawLinkpath, const QString& sourcePath, bool markdownLink) const
{
    QString linkpath = rawLinkpath.trimmed();
    if (linkpath.isEmpty())
        return sourcePath; // [[#Heading]] -> the source file itself
    const QString srcFolder = folderOf(sourcePath);
    auto exact = [&](const QString& p) -> QString {
        const QString l = p.toLower();
        if (auto it = m_byPath.constFind(l); it != m_byPath.cend())
            return *it;
        if (auto it = m_byPath.constFind(l + QStringLiteral(".md")); it != m_byPath.cend())
            return *it;
        return {};
    };
    // Relative paths ("./x", "../x") and markdown links try the source folder first.
    if (linkpath.startsWith(QLatin1String("./")) || linkpath.startsWith(QLatin1String("../")) || markdownLink) {
        const QString rel = normalize(srcFolder.isEmpty() ? linkpath : srcFolder + QLatin1Char('/') + linkpath);
        const QString hit = exact(rel);
        if (!hit.isEmpty())
            return hit;
        if (linkpath.startsWith(QLatin1Char('.')))
            return {};
    }
    // Absolute vault path.
    {
        QString abs = linkpath;
        while (abs.startsWith(QLatin1Char('/')))
            abs.remove(0, 1);
        const QString hit = exact(abs);
        if (!hit.isEmpty())
            return hit;
    }
    const QStringList c = candidates(linkpath, sourcePath);
    return c.isEmpty() ? QString() : c.first();
}

bool LinkResolver::isUniqueName(const QString& targetPath) const { return m_byName.value(nameKey(targetPath)).size() <= 1; }

QString LinkResolver::linkText(const QString& targetPath, const QString& sourcePath, LinkFormat format) const
{
    QString t = targetPath;
    if (isMarkdown(t))
        t.chop(3);
    switch (format) {
    case LinkFormat::Absolute:
        return t;
    case LinkFormat::Relative: {
        const QStringList from = folderOf(sourcePath).split(QLatin1Char('/'), Qt::SkipEmptyParts);
        const QStringList to = t.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        int common = 0;
        while (common < from.size() && common < to.size() - 1 && from[common] == to[common])
            ++common;
        QStringList parts;
        for (int i = common; i < from.size(); ++i)
            parts << QStringLiteral("..");
        for (int i = common; i < to.size(); ++i)
            parts << to[i];
        return parts.join(QLatin1Char('/'));
    }
    case LinkFormat::Shortest:
    default: {
        if (isUniqueName(targetPath)) {
            const QString name = t.mid(t.lastIndexOf(QLatin1Char('/')) + 1);
            return name;
        }
        return t; // ambiguous name: full path from the vault root
    }
    }
}

} // namespace oc
