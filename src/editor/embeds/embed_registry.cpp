#include "editor/embeds/embed_registry.h"

#include "editor/embeds/hosted_embeds.h"
#include "render/layout/style_sheet.h"

#include <QFileInfo>
#include <QUrl>

namespace oc {

namespace {
bool isImageExtension(const QString& ext)
{
    static const QStringList exts = {QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("gif"),
                                     QStringLiteral("bmp"), QStringLiteral("svg"), QStringLiteral("webp"), QStringLiteral("avif")};
    return exts.contains(ext);
}

struct PrintState {
    std::shared_ptr<const Theme> theme;
    std::shared_ptr<const StyleSheet> styles;
    bool active = false;
};

PrintState& printState()
{
    static PrintState s;
    return s;
}

// URLSearchParams over "#a=1&b=2" (a leading '#' or '?' is skipped).
QList<QPair<QString, QString>> searchParams(const QString& subpath)
{
    QList<QPair<QString, QString>> out;
    QStringView s = subpath;
    if (s.startsWith(u'#') || s.startsWith(u'?'))
        s = s.mid(1);
    for (QStringView part : s.split(u'&', Qt::SkipEmptyParts)) {
        const qsizetype eq = part.indexOf(u'=');
        auto decode = [](QStringView v) {
            QString t = v.toString();
            t.replace(u'+', u' ');
            return QUrl::fromPercentEncoding(t.toUtf8());
        };
        if (eq < 0)
            out.append({decode(part), QString()});
        else
            out.append({decode(part.left(eq)), decode(part.mid(eq + 1))});
    }
    return out;
}
} // namespace

// ---------------------------------------------------------------------------------------------
// EmbedRequest
// ---------------------------------------------------------------------------------------------
QString EmbedRequest::key() const
{
    return absPath + QLatin1Char('\n') + subpath + QLatin1Char('\n') + (display == Display::Popover ? u'p' : u'i');
}

bool EmbedRequest::hasParam(const QString& name) const
{
    for (const auto& [k, v] : searchParams(subpath))
        if (k == name)
            return true;
    return false;
}

QString EmbedRequest::param(const QString& name) const
{
    for (const auto& [k, v] : searchParams(subpath))
        if (k == name)
            return v;
    return {};
}

int EmbedRequest::intParam(const QString& name, int fallback) const
{
    // JavaScript parseInt(): optional sign and leading digits, the rest is ignored.
    const QString v = param(name).trimmed();
    qsizetype i = 0;
    if (i < v.size() && (v[i] == u'-' || v[i] == u'+'))
        ++i;
    const qsizetype digits = i;
    while (i < v.size() && v[i].isDigit())
        ++i;
    if (i == digits)
        return fallback;
    bool ok = false;
    const int n = v.left(i).toInt(&ok);
    return ok ? n : fallback;
}

EmbedRequest EmbedRequest::fromLink(const QString& absPath, const QString& target, const QString& subpath)
{
    EmbedRequest r;
    r.absPath = absPath;
    r.subpath = subpath;
    r.linktext = target + subpath;
    r.extension = QFileInfo(absPath).suffix().toLower();
    return r;
}

// ---------------------------------------------------------------------------------------------
// EmbedRegistry
// ---------------------------------------------------------------------------------------------
EmbedRegistry& EmbedRegistry::instance()
{
    static EmbedRegistry r;
    return r;
}

void EmbedRegistry::registerExtensions(const QStringList& extensions, std::shared_ptr<EmbedProvider> provider)
{
    for (const QString& e : extensions)
        m_providers.insert(e.toLower(), provider);
}

void EmbedRegistry::unregisterExtensions(const QStringList& extensions)
{
    for (const QString& e : extensions)
        m_providers.remove(e.toLower());
}

std::shared_ptr<EmbedProvider> EmbedRegistry::provider(const QString& extension) const { return m_providers.value(extension.toLower()); }

void EmbedRegistry::notifyChanged(const QString& absPath) { emit changed(absPath); }

std::shared_ptr<InlineWidget> makeEmbedWidget(const QString& absPath, const QString& target, const QString& subpath, int width,
                                              int height, qreal maxWidth)
{
    if (absPath.isEmpty())
        return nullptr;
    EmbedRequest r = EmbedRequest::fromLink(absPath, target, subpath);
    std::shared_ptr<EmbedProvider> p = EmbedRegistry::instance().provider(r.extension);
    if (!p)
        return nullptr;
    r.width = width;
    r.height = height;
    return std::make_shared<EmbedWidget>(std::move(r), std::move(p), maxWidth);
}

std::shared_ptr<InlineWidget> makeLinkEmbedWidget(const std::function<QString(const QString& linkpath)>& resolve, const QString& target,
                                                  const QString& subpath, int width, int height, qreal maxWidth)
{
    if (!resolve)
        return nullptr;
    const EmbedRegistry& registry = EmbedRegistry::instance();
    const qsizetype dot = target.lastIndexOf(QLatin1Char('.'));
    const QString ext = dot > target.lastIndexOf(QLatin1Char('/')) ? target.mid(dot + 1).toLower() : QString();
    if (!ext.isEmpty() && ext != u"md" && registry.isExtensionRegistered(ext))
        return makeEmbedWidget(resolve(target), target, subpath, width, height, maxWidth);
    // Notes: the link names no extension; the file it resolves to decides.
    const std::shared_ptr<EmbedProvider> notes = registry.provider(QStringLiteral("md"));
    if (!notes || isImageExtension(ext) || (target.isEmpty() && subpath.isEmpty()))
        return nullptr;
    const QString absPath = resolve(target);
    if (!absPath.isEmpty()) {
        if (QFileInfo(absPath).suffix().compare(u"md", Qt::CaseInsensitive) != 0)
            return nullptr;
        return makeEmbedWidget(absPath, target, subpath, width, height, maxWidth);
    }
    if (target.isEmpty() || (!ext.isEmpty() && ext != u"md"))
        return nullptr; // unresolved non-note: stays a link
    EmbedRequest r;
    r.linktext = target + subpath;
    r.subpath = subpath;
    r.extension = QStringLiteral("md");
    r.width = width;
    r.height = height;
    return std::make_shared<EmbedWidget>(std::move(r), notes, maxWidth);
}

// ---------------------------------------------------------------------------------------------
// EmbedPrintScope
// ---------------------------------------------------------------------------------------------
EmbedPrintScope::EmbedPrintScope(std::shared_ptr<const Theme> theme, std::shared_ptr<const StyleSheet> styles)
{
    PrintState& s = printState();
    m_prevTheme = std::exchange(s.theme, std::move(theme));
    m_prevStyles = std::exchange(s.styles, std::move(styles));
    m_prevActive = std::exchange(s.active, true);
}

EmbedPrintScope::~EmbedPrintScope()
{
    PrintState& s = printState();
    s.theme = std::move(m_prevTheme);
    s.styles = std::move(m_prevStyles);
    s.active = m_prevActive;
}

bool EmbedPrintScope::active() { return printState().active; }
std::shared_ptr<const Theme> EmbedPrintScope::theme() { return printState().theme; }
std::shared_ptr<const StyleSheet> EmbedPrintScope::styles() { return printState().styles; }

} // namespace oc
