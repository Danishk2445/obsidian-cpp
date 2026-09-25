#include "editor/markdown_commands.h"

#include "core/documents/document.h"
#include "editor/editor_view.h"

#include <QRegularExpression>

namespace oc::mdcmd {

Settings& settings()
{
    static Settings s;
    return s;
}

namespace {

struct LinePrefix {
    QString quote;      // "> > " (incl. leading whitespace before '>')
    QString indent;     // whitespace before the list marker (after quotes)
    QString marker;     // "-", "*", "+", "3." or "3)"
    QString space;      // whitespace after the marker
    QString task;       // "[ ] " incl. trailing space, or ""
    QChar taskChar;
    int contentStart = 0; // column where content begins
    bool isList() const { return !marker.isEmpty(); }
    bool ordered() const { return !marker.isEmpty() && marker[0].isDigit(); }
};

LinePrefix parsePrefix(QStringView line)
{
    static const QRegularExpression quoteRe(QStringLiteral("^((?:[ \\t]{0,3}>[ \\t]?)*)"));
    static const QRegularExpression listRe(QStringLiteral("^([ \\t]*)([-*+]|\\d{1,9}[.)])([ \\t]+|$)(\\[(.)\\][ \\t]+|\\[(.)\\]$)?"));
    LinePrefix p;
    const QString s = line.toString();
    auto qm = quoteRe.match(s);
    p.quote = qm.captured(1);
    const QString rest = s.mid(p.quote.size());
    auto lm = listRe.match(rest);
    if (lm.hasMatch()) {
        p.indent = lm.captured(1);
        p.marker = lm.captured(2);
        p.space = lm.captured(3);
        p.task = lm.captured(4);
        if (!p.task.isEmpty())
            p.taskChar = !lm.captured(5).isEmpty() ? lm.captured(5)[0] : lm.captured(6)[0];
        p.contentStart = int(p.quote.size() + lm.capturedLength(0));
    } else {
        p.contentStart = int(p.quote.size());
    }
    return p;
}

QString nextMarker(const LinePrefix& p)
{
    if (!p.ordered())
        return p.marker;
    const QChar delim = p.marker.back();
    const int n = p.marker.left(p.marker.size() - 1).toInt();
    return QString::number(n + 1) + delim;
}

QString indentUnit() { return settings().useTab ? QStringLiteral("\t") : QString(settings().tabSize, QLatin1Char(' ')); }

bool inCodeBlock(const Document& doc, int line)
{
    const md::LineRole r = doc.syntax().role(line);
    return r == md::LineRole::FenceBody || r == md::LineRole::FenceOpen || r == md::LineRole::IndentedCode || r == md::LineRole::MathBody
           || r == md::LineRole::FrontmatterBody;
}

} // namespace

bool newlineAndContinueMarkup(EditorView& view)
{
    Document* doc = view.document();
    if (!doc || !settings().smartIndentList || view.isReadOnly())
        return false;
    const Text& t = doc->text();
    const Selection sel = view.selection();
    QList<ChangeSpec> specs;
    QList<qsizetype> cursorAfter; // positions in the NEW document (computed after mapping)
    struct Pending {
        qsizetype from, to;
        QString insert;
        qsizetype caretOffsetInInsert; // caret = from(mapped) + this
    };
    QList<Pending> pend;
    bool any = false;
    for (const SelRange& r : sel.ranges()) {
        const int line = t.lineAt(r.head);
        const QStringView lv = t.lineView(line);
        const qsizetype ls = t.lineStart(line);
        const int col = int(r.head - ls);
        const LinePrefix p = parsePrefix(lv);
        if (inCodeBlock(*doc, line) || (!p.isList() && p.quote.isEmpty()) || col < p.contentStart) {
            pend.push_back({r.from(), r.to(), QStringLiteral("\n"), 1});
            continue;
        }
        any = true;
        const bool emptyItem = lv.mid(p.contentStart).trimmed().isEmpty() && r.empty();
        if (emptyItem) {
            // Empty item: outdent one level, or remove the list/quote markup.
            if (p.isList() && !p.indent.isEmpty()) {
                QString indent = p.indent;
                if (indent.startsWith(QLatin1Char('\t')))
                    indent.remove(0, 1);
                else
                    indent.remove(0, std::min<qsizetype>(indent.size(), settings().tabSize));
                const QString newPrefix = p.quote + indent + p.marker + p.space + (p.task.isEmpty() ? QString() : QStringLiteral("[ ] "));
                pend.push_back({ls, ls + lv.size(), newPrefix, newPrefix.size()});
            } else if (p.isList()) {
                pend.push_back({ls, ls + lv.size(), p.quote, p.quote.size()});
            } else {
                // empty quote line -> leave the quote
                QString q = p.quote;
                const qsizetype gt = q.lastIndexOf(QLatin1Char('>'));
                q = gt >= 0 ? q.left(gt) : QString();
                pend.push_back({ls, ls + lv.size(), q.trimmed().isEmpty() ? QString() : q, q.trimmed().isEmpty() ? 0 : q.size()});
            }
            continue;
        }
        QString cont = QStringLiteral("\n") + p.quote;
        if (p.isList()) {
            cont += p.indent + nextMarker(p) + (p.space.isEmpty() ? QStringLiteral(" ") : p.space);
            if (!p.task.isEmpty())
                cont += QStringLiteral("[ ] ");
        }
        pend.push_back({r.from(), r.to(), cont, cont.size()});
    }
    if (!any)
        return false;
    for (const Pending& pp : pend)
        specs.push_back({pp.from, pp.to, pp.insert});
    Transaction tr;
    tr.changes = ChangeSet::of(specs, t.length());
    QList<SelRange> after;
    for (const Pending& pp : pend)
        after.push_back(SelRange::cursor(tr.changes.mapPos(pp.from, -1) + pp.caretOffsetInInsert));
    tr.selection = Selection::create(after, sel.mainIndex());
    tr.userEvent = QStringLiteral("input.type");
    view.dispatch(tr);
    return true;
}

bool indentListItem(EditorView& view, bool more)
{
    Document* doc = view.document();
    if (!doc || view.isReadOnly())
        return false;
    const Text& t = doc->text();
    const Selection sel = view.selection();
    QList<int> lines;
    for (const SelRange& r : sel.ranges()) {
        int la = t.lineAt(r.from()), lb = t.lineAt(r.to());
        if (lb > la && r.to() == t.lineStart(lb))
            --lb;
        for (int l = la; l <= lb; ++l)
            if (lines.isEmpty() || lines.last() < l)
                lines.push_back(l);
    }
    for (int l : lines)
        if (!parsePrefix(t.lineView(l)).isList())
            return false;
    QList<ChangeSpec> specs;
    for (int l : lines) {
        const LinePrefix p = parsePrefix(t.lineView(l));
        const qsizetype at = t.lineStart(l) + p.quote.size();
        if (more) {
            specs.push_back({at, at, indentUnit()});
        } else if (!p.indent.isEmpty()) {
            const int n = p.indent.startsWith(QLatin1Char('\t')) ? 1 : int(std::min<qsizetype>(p.indent.size(), settings().tabSize));
            specs.push_back({at, at + n, QString()});
        }
    }
    if (specs.isEmpty())
        return true;
    Transaction tr;
    tr.changes = ChangeSet::of(specs, t.length());
    tr.selection = sel.map(tr.changes.desc(), 1);
    tr.userEvent = more ? QStringLiteral("input.indent") : QStringLiteral("delete.dedent");
    view.dispatch(tr);
    return true;
}

bool deleteMarkupBackward(EditorView& view)
{
    Document* doc = view.document();
    if (!doc || view.isReadOnly() || view.selection().size() != 1 || !view.selection().main().empty())
        return false;
    const Text& t = doc->text();
    const qsizetype pos = view.cursorPosition();
    const int line = t.lineAt(pos);
    if (inCodeBlock(*doc, line))
        return false;
    const qsizetype ls = t.lineStart(line);
    const LinePrefix p = parsePrefix(t.lineView(line));
    if (!p.isList() || pos - ls != p.contentStart)
        return false;
    // Remove the list marker (and checkbox), keep indentation and quotes.
    const qsizetype from = ls + p.quote.size() + p.indent.size();
    Transaction tr;
    tr.changes = ChangeSet::of({{from, ls + p.contentStart, QString()}}, t.length());
    tr.selection = Selection::cursor(from);
    tr.userEvent = QStringLiteral("delete.backward");
    view.dispatch(tr);
    return true;
}

bool autoPairInput(EditorView& view, const QString& text)
{
    Document* doc = view.document();
    if (!doc || text.size() != 1 || view.isReadOnly())
        return false;
    const QChar c = text[0];
    const Text& t = doc->text();
    const Selection sel = view.selection();
    static const QString openers = QStringLiteral("([{\"'");
    static const QString closers = QStringLiteral(")]}\"'");
    static const QString mdPairs = QStringLiteral("*_~=`$");
    const bool bracket = settings().autoPairBrackets && openers.contains(c);
    const bool mdSym = settings().autoPairMarkdown && mdPairs.contains(c);

    // Wrap non-empty selections.
    if (!sel.allEmpty() && (bracket || mdSym)) {
        const QChar close = bracket ? closers[openers.indexOf(c)] : c;
        QList<ChangeSpec> specs;
        for (const SelRange& r : sel.ranges()) {
            specs.push_back({r.from(), r.from(), QString(c)});
            specs.push_back({r.to(), r.to(), QString(close)});
        }
        Transaction tr;
        tr.changes = ChangeSet::of(specs, t.length());
        QList<SelRange> after;
        for (const SelRange& r : sel.ranges())
            after.push_back(SelRange(tr.changes.mapPos(r.from(), 1), tr.changes.mapPos(r.to(), -1)));
        tr.selection = Selection::create(after, sel.mainIndex());
        tr.userEvent = QStringLiteral("input.type");
        view.dispatch(tr);
        return true;
    }
    if (sel.size() != 1 || !sel.main().empty())
        return false;
    const qsizetype pos = sel.main().head;
    const QChar next = t.charAt(pos);
    const QChar prev = pos > 0 ? t.charAt(pos - 1) : QChar();
    // Type over an auto-inserted closer.
    if (settings().autoPairBrackets && closers.contains(c) && next == c && (c != u'"' && c != u'\'' ? true : true)) {
        const QString o = QString(openers[closers.indexOf(c)]);
        if (c == u')' || c == u']' || c == u'}' || prev == c || (openers.contains(c) && next == c)) {
            view.setCursorPosition(pos + 1);
            return true;
        }
    }
    if (!bracket)
        return false;
    if (c == u'"' || c == u'\'') {
        if (prev.isLetterOrNumber() || prev == c)
            return false; // apostrophes in words, doubled quotes
    }
    if (!next.isNull() && next != u'\n' && !next.isSpace() && !closers.contains(next))
        return false;
    const QChar close = closers[openers.indexOf(c)];
    Transaction tr;
    tr.changes = ChangeSet::of({{pos, pos, QString(c) + close}}, t.length());
    tr.selection = Selection::cursor(pos + 1);
    tr.userEvent = QStringLiteral("input.type");
    view.dispatch(tr);
    return true;
}

// ---------------------------------------------------------------------------------------------
// toggles
// ---------------------------------------------------------------------------------------------
void toggleWrap(EditorView& view, const QString& open, const QString& close)
{
    Document* doc = view.document();
    if (!doc || view.isReadOnly())
        return;
    const Text& t = doc->text();
    const Selection sel = view.selection();
    QList<ChangeSpec> specs;
    QList<std::pair<qsizetype, qsizetype>> newSel; // in old coordinates + deltas applied later via mapping
    for (const SelRange& r : sel.ranges()) {
        const qsizetype a = r.from(), b = r.to();
        const QString inside = t.slice(a, b);
        const qsizetype ol = open.size(), cl = close.size();
        if (b - a >= ol + cl && inside.startsWith(open) && inside.endsWith(close)) {
            specs.push_back({a, a + ol, QString()});
            specs.push_back({b - cl, b, QString()});
        } else if (a >= ol && t.slice(a - ol, a) == open && t.slice(b, b + cl) == close) {
            specs.push_back({a - ol, a, QString()});
            specs.push_back({b, b + cl, QString()});
        } else {
            specs.push_back({a, a, open});
            specs.push_back({b, b, close});
        }
    }
    Transaction tr;
    tr.changes = ChangeSet::of(specs, t.length());
    QList<SelRange> after;
    for (const SelRange& r : sel.ranges()) {
        if (r.empty()) {
            const qsizetype p = tr.changes.mapPos(r.from(), 1);
            after.push_back(SelRange::cursor(p));
        } else {
            after.push_back(SelRange(tr.changes.mapPos(r.from(), 1), tr.changes.mapPos(r.to(), -1)));
        }
    }
    tr.selection = Selection::create(after, sel.mainIndex());
    tr.userEvent = QStringLiteral("format");
    view.dispatch(tr);
}

void toggleBold(EditorView& v) { toggleWrap(v, QStringLiteral("**"), QStringLiteral("**")); }
void toggleItalics(EditorView& v) { toggleWrap(v, QStringLiteral("*"), QStringLiteral("*")); }
void toggleStrikethrough(EditorView& v) { toggleWrap(v, QStringLiteral("~~"), QStringLiteral("~~")); }
void toggleHighlight(EditorView& v) { toggleWrap(v, QStringLiteral("=="), QStringLiteral("==")); }
void toggleCode(EditorView& v) { toggleWrap(v, QStringLiteral("`"), QStringLiteral("`")); }
void toggleInlineMath(EditorView& v) { toggleWrap(v, QStringLiteral("$"), QStringLiteral("$")); }

void toggleComment(EditorView& view)
{
    Document* doc = view.document();
    if (!doc)
        return;
    const SelRange r = view.selection().main();
    const Text& t = doc->text();
    if (t.lineAt(r.from()) != t.lineAt(r.to()) && !r.empty())
        toggleWrap(view, QStringLiteral("%%\n"), QStringLiteral("\n%%"));
    else
        toggleWrap(view, QStringLiteral("%%"), QStringLiteral("%%"));
}

namespace {
// Apply a per-line transformation to all lines touched by the selection.
void transformLines(EditorView& view, const std::function<QString(const QString& line, int index, bool allMatch)>& fn,
                    const std::function<bool(const QString& line)>& matches)
{
    Document* doc = view.document();
    if (!doc || view.isReadOnly())
        return;
    const Text& t = doc->text();
    const Selection sel = view.selection();
    QList<int> lines;
    for (const SelRange& r : sel.ranges()) {
        int la = t.lineAt(r.from()), lb = t.lineAt(r.to());
        if (lb > la && r.to() == t.lineStart(lb))
            --lb;
        for (int l = la; l <= lb; ++l)
            if (lines.isEmpty() || lines.last() < l)
                lines.push_back(l);
    }
    bool all = true;
    for (int l : lines)
        all &= matches(t.line(l));
    QList<ChangeSpec> specs;
    for (int i = 0; i < lines.size(); ++i) {
        const int l = lines[i];
        const QString before = t.line(l);
        const QString after = fn(before, i, all);
        if (after != before)
            specs.push_back({t.lineStart(l), t.lineEnd(l), after});
    }
    if (specs.isEmpty())
        return;
    Transaction tr;
    tr.changes = ChangeSet::of(specs, t.length());
    tr.selection = sel.map(tr.changes.desc(), 1);
    tr.userEvent = QStringLiteral("format");
    view.dispatch(tr);
}
} // namespace

void toggleBlockquote(EditorView& view)
{
    static const QRegularExpression re(QStringLiteral("^[ \\t]{0,3}> ?"));
    transformLines(
        view,
        [](const QString& line, int, bool all) -> QString { return all ? QString(line).remove(re) : QStringLiteral("> ") + line; },
        [](const QString& line) { return re.match(line).hasMatch(); });
}

void toggleBulletList(EditorView& view)
{
    static const QRegularExpression bullet(QStringLiteral("^([ \\t]*)[-*+] (\\[.\\] )?"));
    static const QRegularExpression ordered(QStringLiteral("^([ \\t]*)\\d{1,9}[.)] "));
    transformLines(
        view,
        [](const QString& line, int, bool all) -> QString {
            if (all)
                return QString(line).replace(bullet, QStringLiteral("\\1"));
            QString l = line;
            l.replace(ordered, QStringLiteral("\\1"));
            const qsizetype ws = l.indexOf(QRegularExpression(QStringLiteral("\\S")));
            if (ws < 0)
                return l + QStringLiteral("- ");
            return l.left(ws) + QStringLiteral("- ") + l.mid(ws);
        },
        [](const QString& line) { return bullet.match(line).hasMatch(); });
}

void toggleNumberedList(EditorView& view)
{
    static const QRegularExpression ordered(QStringLiteral("^([ \\t]*)\\d{1,9}[.)] "));
    static const QRegularExpression bullet(QStringLiteral("^([ \\t]*)[-*+] "));
    transformLines(
        view,
        [](const QString& line, int index, bool all) -> QString {
            if (all)
                return QString(line).replace(ordered, QStringLiteral("\\1"));
            QString l = line;
            l.replace(bullet, QStringLiteral("\\1"));
            const qsizetype ws = std::max<qsizetype>(0, l.indexOf(QRegularExpression(QStringLiteral("\\S|$"))));
            return l.left(ws) + QString::number(index + 1) + QStringLiteral(". ") + l.mid(ws);
        },
        [](const QString& line) { return ordered.match(line).hasMatch(); });
}

void toggleChecklist(EditorView& view, bool cycle)
{
    // Obsidian's toggleCheckList: one state for all touched lines, decided by the "least done" line —
    // any non-list line (cycling only) -> plain bullets; any non-task -> "[ ] "; any "[ ]" -> "[x] ";
    // otherwise (all done) -> "[ ] " again, or back to plain bullets when cycling. Only the prefix changes.
    Document* doc = view.document();
    if (!doc || view.isReadOnly())
        return;
    static const QRegularExpression re(QStringLiteral("^([>\\s]*)(([*+-] |(\\d+)([.)] ))(?:\\[(.)\\] )?)?"));
    const Text& t = doc->text();
    const Selection sel = view.selection();
    QList<int> lines;
    for (const SelRange& r : sel.ranges()) {
        const int la = t.lineAt(r.from()), lb = t.lineAt(r.to());
        for (int l = la; l <= lb; ++l)
            if (lines.isEmpty() || lines.last() < l)
                lines.push_back(l);
    }
    QList<QRegularExpressionMatch> matches;
    int state = 3;
    for (int l : lines) {
        const QRegularExpressionMatch m = re.match(t.line(l));
        const bool hasTask = m.capturedLength(6) > 0;
        if (state > 2 && hasTask && m.captured(6) == u" ")
            state = 2;
        if (state > 1 && !hasTask)
            state = 1;
        if (cycle && state > 0 && m.capturedLength(3) == 0)
            state = 0;
        matches.push_back(m);
    }
    QList<ChangeSpec> specs;
    for (qsizetype i = 0; i < lines.size(); ++i) {
        const QRegularExpressionMatch& m = matches[i];
        const QString marker = m.capturedLength(3) ? m.captured(3) : QStringLiteral("- ");
        const QString base = m.captured(1) + marker;
        QString text = base;
        if (state == 1 && m.capturedLength(6) == 0)
            text = base + QStringLiteral("[ ] ");
        else if (state == 2)
            text = base + QStringLiteral("[x] ");
        else if (state == 3 && !cycle)
            text = base + QStringLiteral("[ ] ");
        if (text != m.captured(0))
            specs.push_back({t.lineStart(lines[i]), t.lineStart(lines[i]) + m.capturedLength(0), text});
    }
    if (specs.isEmpty())
        return;
    Transaction tr;
    tr.changes = ChangeSet::of(specs, t.length());
    tr.selection = sel.map(tr.changes.desc(), 1);
    tr.userEvent = QStringLiteral("format");
    view.dispatch(tr);
}

int nextFootnoteNumber(const Text& text)
{
    // Obsidian's: one more than the largest numeric footnote definition id.
    static const QRegularExpression def(QStringLiteral("^[ \\t>]*\\[\\^([^\\]\\s]+)\\]:"));
    int max = 0;
    for (int l = 0; l < text.lineCount(); ++l) {
        const QStringView line = text.lineView(l);
        if (!line.contains(QStringLiteral("[^")))
            continue;
        const QRegularExpressionMatch m = def.matchView(line);
        bool ok = false;
        const int n = m.hasMatch() ? m.captured(1).toInt(&ok) : 0;
        static const QRegularExpression digits(QStringLiteral("^\\d+$"));
        if (ok && digits.match(m.captured(1)).hasMatch())
            max = std::max(max, n);
    }
    return max + 1;
}

QString insertFootnote(EditorView& view)
{
    Document* doc = view.document();
    if (!doc || view.isReadOnly())
        return {};
    const Text& t = doc->text();
    const QString ref = QStringLiteral("[^") + QString::number(nextFootnoteNumber(t)) + QLatin1Char(']');
    const SelRange r = view.selection().main();
    // blank line before the definition: count the newlines ending the document after the selection
    int trailing = 0;
    for (qsizetype p = t.length(); p > r.to() && trailing < 2 && t.slice(p - 1, p) == u"\n"; --p)
        ++trailing;
    const QString def = QString(2 - trailing, QLatin1Char('\n')) + ref + QStringLiteral(": \n");
    Transaction tr;
    tr.changes = ChangeSet::of({{r.from(), r.from(), ref}, {t.length(), t.length(), def}}, t.length());
    tr.selection = Selection::cursor(r.from() + ref.size());
    tr.userEvent = QStringLiteral("input");
    view.dispatch(tr);
    return ref;
}

void insertTag(EditorView& view)
{
    Document* doc = view.document();
    if (!doc || view.isReadOnly())
        return;
    const qsizetype from = view.selection().main().from();
    Transaction tr;
    tr.changes = ChangeSet::of({{from, from, QStringLiteral("#")}}, doc->text().length());
    tr.selection = Selection::cursor(from + 1);
    tr.userEvent = QStringLiteral("input.autocomplete"); // opens the tag suggest
    view.dispatch(tr);
}

void setHeading(EditorView& view, int level)
{
    static const QRegularExpression re(QStringLiteral("^#{1,6}[ \\t]+"));
    transformLines(
        view,
        [level](const QString& line, int, bool) -> QString {
            QString l = QString(line).remove(re);
            return level <= 0 ? l : QString(level, QLatin1Char('#')) + QLatin1Char(' ') + l;
        },
        [](const QString&) { return false; });
}

void insertLink(EditorView& view)
{
    Document* doc = view.document();
    if (!doc || view.isReadOnly())
        return;
    const SelRange r = view.selection().main();
    const QString selected = doc->text().slice(r.from(), r.to());
    static const QRegularExpression url(QStringLiteral("^[a-zA-Z][a-zA-Z0-9+.-]*://\\S+$"));
    QString ins;
    qsizetype caret;
    if (url.match(selected).hasMatch()) {
        ins = QStringLiteral("[](") + selected + QLatin1Char(')');
        caret = r.from() + 1;
    } else {
        ins = QLatin1Char('[') + selected + QStringLiteral("]()");
        caret = r.from() + selected.size() + 3;
    }
    Transaction tr;
    tr.changes = ChangeSet::of({{r.from(), r.to(), ins}}, doc->text().length());
    tr.selection = Selection::cursor(caret);
    tr.userEvent = QStringLiteral("format");
    view.dispatch(tr);
}

void insertWikilink(EditorView& view, bool embed)
{
    Document* doc = view.document();
    if (!doc || view.isReadOnly())
        return;
    const SelRange r = view.selection().main();
    const QString selected = doc->text().slice(r.from(), r.to());
    const QString open = embed ? QStringLiteral("![[") : QStringLiteral("[[");
    Transaction tr;
    tr.changes = ChangeSet::of({{r.from(), r.to(), open + selected + QStringLiteral("]]")}}, doc->text().length());
    tr.selection = Selection::cursor(r.from() + open.size() + selected.size());
    tr.userEvent = QStringLiteral("input.type");
    view.dispatch(tr);
}

namespace {
void insertBlockTemplate(EditorView& view, const QString& before, const QString& after)
{
    Document* doc = view.document();
    if (!doc || view.isReadOnly())
        return;
    const Text& t = doc->text();
    const SelRange r = view.selection().main();
    const int line = t.lineAt(r.from());
    const bool atLineStart = r.from() == t.lineStart(line) && t.lineLength(line) == 0;
    const QString lead = atLineStart ? QString() : QStringLiteral("\n");
    const QString selected = t.slice(r.from(), r.to());
    const QString ins = lead + before + selected + after;
    Transaction tr;
    tr.changes = ChangeSet::of({{r.from(), r.to(), ins}}, t.length());
    tr.selection = Selection::cursor(r.from() + lead.size() + before.size() + selected.size());
    tr.userEvent = QStringLiteral("input");
    view.dispatch(tr);
}
} // namespace

void insertHorizontalRule(EditorView& view) { insertBlockTemplate(view, QStringLiteral("---\n"), QString()); }
void insertCodeBlock(EditorView& view) { insertBlockTemplate(view, QStringLiteral("```\n"), QStringLiteral("\n```")); }
void insertMathBlock(EditorView& view) { insertBlockTemplate(view, QStringLiteral("$$\n"), QStringLiteral("\n$$")); }
void insertTable(EditorView& view)
{
    insertBlockTemplate(view, QString(), QStringLiteral("|  |  |\n| --- | --- |\n|  |  |"));
}
void insertCallout(EditorView& view, const QString& type) { insertBlockTemplate(view, QStringLiteral("> [!") + type + QStringLiteral("]\n> "), QString()); }

void clearFormatting(EditorView& view)
{
    Document* doc = view.document();
    if (!doc || view.isReadOnly())
        return;
    const SelRange r = view.selection().main();
    if (r.empty())
        return;
    QString s = doc->text().slice(r.from(), r.to());
    static const QRegularExpression re(QStringLiteral("(\\*\\*|__|~~|==|\\*|_|`)"));
    s.remove(re);
    Transaction tr;
    tr.changes = ChangeSet::of({{r.from(), r.to(), s}}, doc->text().length());
    tr.selection = Selection::single(r.from(), r.from() + s.size());
    tr.userEvent = QStringLiteral("format");
    view.dispatch(tr);
}

} // namespace oc::mdcmd
