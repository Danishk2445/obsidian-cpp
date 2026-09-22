#include "editor/plugins/greentext_guard.h"

#include "core/documents/document.h"
#include "editor/editor_hooks.h"
#include "editor/editor_view.h"

#include <QFileInfo>
#include <QMimeData>
#include <QRegularExpression>

namespace oc {

GreentextGuard::Settings GreentextGuard::Settings::fromJson(const QJsonObject& o)
{
    Settings s;
    s.patterns = o.value(QStringLiteral("patterns")).toString(s.patterns);
    s.onPaste = o.value(QStringLiteral("onPaste")).toBool(s.onPaste);
    s.expandToBlock = o.value(QStringLiteral("expandToBlock")).toBool(s.expandToBlock);
    s.onType = o.value(QStringLiteral("onType")).toString(s.onType);
    return s;
}

QJsonObject GreentextGuard::Settings::toJson() const
{
    return {{QStringLiteral("patterns"), patterns},
            {QStringLiteral("onPaste"), onPaste},
            {QStringLiteral("expandToBlock"), expandToBlock},
            {QStringLiteral("onType"), onType}};
}

GreentextGuard& GreentextGuard::instance()
{
    static GreentextGuard g;
    return g;
}

void GreentextGuard::install()
{
    if (m_inputHook)
        return;
    m_inputHook = EditorHooks::global().addInputHandler(
        [this](EditorView& v, qsizetype from, qsizetype to, const QString& text) { return handleInput(v, from, to, text); });
    m_pasteHook = EditorHooks::global().addPasteHandler([this](EditorView& v, const QMimeData& md) {
        const bool files = md.hasImage() || (md.hasUrls() && !md.urls().isEmpty() && md.urls().first().isLocalFile());
        return md.hasText() && handlePaste(v, md.text(), files);
    });
}

void GreentextGuard::uninstall()
{
    EditorHooks::global().remove(m_inputHook);
    EditorHooks::global().remove(m_pasteHook);
    m_inputHook = m_pasteHook = 0;
}

bool GreentextGuard::fileMatches(const QString& path) const
{
    if (path.isEmpty())
        return false;
    const QString name = QFileInfo(path).completeBaseName().toLower();
    for (const QString& p : m_settings.patterns.split(QLatin1Char(','))) {
        const QString pat = p.trimmed().toLower();
        if (!pat.isEmpty() && name.contains(pat))
            return true;
    }
    return false;
}

QString GreentextGuard::fenceWrap(const QString& text)
{
    int longest = 0, run = 0;
    for (QChar c : text) {
        run = c == u'`' ? run + 1 : 0;
        longest = std::max(longest, run);
    }
    const QString ticks(std::max(3, longest + 1), QLatin1Char('`'));
    QString body = text;
    static const QRegularExpression trailingWs(QStringLiteral("\\s+$"));
    body.remove(trailingWs);
    return ticks + QLatin1Char('\n') + body + QLatin1Char('\n') + ticks;
}

namespace {
bool insideFence(const Document& doc, int line)
{
    // The plugin scans for ``` / ~~~ lines; the block tree gives the same answer (and is exact).
    const md::LineRole r = doc.syntax().role(line);
    return r == md::LineRole::FenceBody || r == md::LineRole::FenceClose;
}

bool isStopLine(QStringView line)
{
    static const QRegularExpression fence(QStringLiteral("^\\s*(`{3,}|~{3,})"));
    return line.trimmed().isEmpty() || fence.match(line.toString()).hasMatch();
}
} // namespace

bool GreentextGuard::wrapSelectionInFence(EditorView& view)
{
    const QString sel = view.selectedText();
    if (sel.isEmpty())
        return false;
    view.replaceSelections(fenceWrap(sel) + QLatin1Char('\n'), QStringLiteral("input.paste"));
    return true;
}

bool GreentextGuard::handlePaste(EditorView& view, const QString& text, bool hasFiles)
{
    Document* doc = view.document();
    if (!doc || !m_settings.onPaste || hasFiles || !fileMatches(doc->path()))
        return false;
    static const QRegularExpression gt(QStringLiteral("^[ \\t]*>"), QRegularExpression::MultilineOption);
    if (text.isEmpty() || !gt.match(text).hasMatch())
        return false;
    const Text& t = doc->text();
    const SelRange r = view.selection().main();
    const int fromLine = t.lineAt(r.from()), toLine = t.lineAt(r.to());
    if (insideFence(*doc, fromLine))
        return false;

    if (!m_settings.expandToBlock) {
        const QString before = t.slice(t.lineStart(fromLine), r.from());
        const QString prefix = before.trimmed().isEmpty() ? QString() : QStringLiteral("\n");
        view.replaceSelections(prefix + fenceWrap(text) + QLatin1Char('\n'), QStringLiteral("input.paste"));
        return true;
    }
    int start = fromLine;
    while (start > 0 && !isStopLine(t.lineView(start - 1)))
        --start;
    int end = toLine;
    while (end < t.lineCount() - 1 && !isStopLine(t.lineView(end + 1)))
        ++end;
    const qsizetype blockStart = t.lineStart(start), blockEnd = t.lineEnd(end);
    const QString head = t.slice(blockStart, r.from());
    const QString tail = t.slice(r.to(), blockEnd);
    QString pasted = text;
    pasted.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    const QString lead = !head.isEmpty() && !head.endsWith(QLatin1Char('\n')) ? QStringLiteral("\n") : QString();
    const QString trail = !tail.isEmpty() && !pasted.endsWith(QLatin1Char('\n')) ? QStringLiteral("\n") : QString();
    const QString fenced = fenceWrap(head + lead + pasted + trail + tail);
    Transaction tr;
    tr.changes = ChangeSet::of({{blockStart, blockEnd, fenced}}, t.length());
    // caret at the end of the closing fence line
    tr.selection = Selection::cursor(blockStart + fenced.size());
    tr.userEvent = QStringLiteral("input.paste");
    view.dispatch(tr);
    return true;
}

bool GreentextGuard::handleInput(EditorView& view, qsizetype from, qsizetype to, const QString& text)
{
    Document* doc = view.document();
    if (!doc || text != u">" || m_settings.onType == u"off" || !fileMatches(doc->path()))
        return false;
    const Text& t = doc->text();
    const int line = t.lineAt(from);
    const QString before = t.slice(t.lineStart(line), from);
    for (QChar c : before)
        if (c != u' ' && c != u'\t')
            return false; // only when '>' would start a blockquote
    if (insideFence(*doc, line))
        return false;
    const QString insert = m_settings.onType == u"escape" ? QStringLiteral("\\>") : QStringLiteral("`>`");
    Transaction tr;
    tr.changes = ChangeSet::of({{from, to, insert}}, t.length());
    tr.selection = Selection::cursor(from + 2); // like the plugin: anchor from + 2
    tr.userEvent = QStringLiteral("input.type");
    view.dispatch(tr);
    return true;
}

} // namespace oc
