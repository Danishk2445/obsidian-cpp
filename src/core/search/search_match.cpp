#include "core/search/search_match.h"

#include "core/search/search_internal.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <vector>

namespace oc::search {
namespace {

constexpr int kCancelInterval = 1024; // lines / regions / occurrences between two cancellation checks

// The part of the document a (sub)query is evaluated on: the whole document, or a region of the content.
struct View {
    int from = 0;
    int to = 0;
    bool whole = true; // the file name, tags without a position and properties only count in the whole document
};

struct Hits {
    QList<MatchRange> file;    // path coordinates
    QList<MatchRange> content; // content offsets, unsorted
    QList<PropertyMatch> props;

    void append(Hits&& o)
    {
        file.append(std::move(o.file));
        content.append(std::move(o.content));
        props.append(std::move(o.props));
    }
};

// A negation-free subquery whose content terms are plain text: if it matches inside a region, it also matches the
// enclosing view (used to reject documents before iterating lines). Regexes are excluded (anchors/lookarounds).
bool isMonotone(const Node& n)
{
    switch (n.kind) {
    case NodeKind::Not:
        return false;
    case NodeKind::Regex:
        return n.scope != Scope::Content && n.scope != Scope::Default;
    case NodeKind::And:
    case NodeKind::Or:
    case NodeKind::Scope:
    case NodeKind::Case:
    case NodeKind::Region:
        for (const NodePtr& c : n.children)
            if (c && !isMonotone(*c))
                return false;
        return true;
    case NodeKind::Text:
    case NodeKind::Phrase:
    case NodeKind::Empty:
    case NodeKind::Property:
    case NodeKind::Compare:
    case NodeKind::IsNull:
    case NodeKind::IsTrue:
    case NodeKind::IsFalse:
        return true;
    }
    return false;
}

QList<MatchRange> mergeRanges(QList<MatchRange> ranges)
{
    std::sort(ranges.begin(), ranges.end(), [](const MatchRange& a, const MatchRange& b) {
        return a.from != b.from ? a.from < b.from : a.to < b.to;
    });
    QList<MatchRange> out;
    out.reserve(ranges.size());
    for (const MatchRange& r : ranges) {
        if (!out.isEmpty() && r.from < out.back().to) // overlapping (touching hits stay separate matches)
            out.back().to = std::max(out.back().to, r.to);
        else
            out.push_back(r);
    }
    return out;
}

std::optional<double> numericValue(const QVariant& v)
{
    switch (v.typeId()) {
    case QMetaType::Double:
    case QMetaType::Float:
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
        return v.toDouble();
    case QMetaType::QString: {
        bool ok = false;
        const double d = v.toString().trimmed().toDouble(&ok);
        return ok ? std::optional<double>(d) : std::nullopt;
    }
    default:
        return std::nullopt;
    }
}

bool isListValue(const QVariant& v) { return v.typeId() == QMetaType::QVariantList || v.typeId() == QMetaType::QStringList; }

class Evaluator {
public:
    Evaluator(const SearchDocument& doc, const std::atomic<bool>* cancel)
        : m_doc(doc)
        , m_content(doc.hasContent ? QStringView(doc.content) : QStringView())
        , m_pathNoExt(doc.pathNoExtension())
        , m_name(doc.name())
        , m_nameOffset(int(doc.nameOffset()))
        , m_cancel(cancel)
    {
    }

    View wholeView() const { return {0, int(m_content.size()), true}; }
    bool canceled() const { return m_canceled; }
    bool checkCancel()
    {
        if (!m_canceled && m_cancel && m_cancel->load(std::memory_order_relaxed))
            m_canceled = true;
        return m_canceled;
    }

    // Does `n` match on `v`? With `out`, the highlights of its positive terms are appended (only on success).
    bool eval(const Node& n, const View& v, Hits* out)
    {
        if (m_canceled)
            return false;
        switch (n.kind) {
        case NodeKind::And: {
            if (!out) {
                for (const NodePtr& c : n.children)
                    if (!eval(*c, v, nullptr))
                        return false;
                return true;
            }
            Hits local;
            for (const NodePtr& c : n.children)
                if (!eval(*c, v, &local))
                    return false;
            out->append(std::move(local));
            return true;
        }
        case NodeKind::Or: {
            bool any = false;
            for (const NodePtr& c : n.children) {
                if (eval(*c, v, out)) {
                    any = true;
                    if (!out)
                        return true;
                }
            }
            return any;
        }
        case NodeKind::Not:
            return !eval(*n.children.value(0), v, nullptr) && !m_canceled;
        case NodeKind::Scope:
        case NodeKind::Case:
            return eval(*n.children.value(0), v, out);
        case NodeKind::Region:
            return evalRegion(n, v, out);
        case NodeKind::Property:
            return evalProperty(n, out);
        case NodeKind::Text:
        case NodeKind::Phrase:
        case NodeKind::Regex:
        case NodeKind::Empty:
            return evalLeaf(n, v, out);
        case NodeKind::Compare:
        case NodeKind::IsNull:
        case NodeKind::IsTrue:
        case NodeKind::IsFalse:
            return false; // property value tests; evaluated by evalValue()
        }
        return false;
    }

private:
    bool evalLeaf(const Node& n, const View& v, Hits* out)
    {
        switch (n.scope) {
        case Scope::Default: {
            bool matched = false;
            if (v.whole) {
                matched = matchText(n, m_pathNoExt, 0, out ? &out->file : nullptr);
                if (matched && !out)
                    return true;
            }
            if (m_doc.hasContent && matchText(n, m_content.mid(v.from, v.to - v.from), v.from, out ? &out->content : nullptr))
                matched = true;
            return matched;
        }
        case Scope::FileName:
            return matchText(n, m_name, m_nameOffset, out ? &out->file : nullptr);
        case Scope::Path:
            return matchText(n, QStringView(m_doc.path), 0, out ? &out->file : nullptr);
        case Scope::Content:
            return m_doc.hasContent && matchText(n, m_content.mid(v.from, v.to - v.from), v.from, out ? &out->content : nullptr);
        case Scope::Tag:
            return matchTags(n, v, out);
        case Scope::PropertyKey:
        case Scope::PropertyValue:
            return false; // evaluated by evalProperty()
        }
        return false;
    }

    // Text/Phrase/Regex/Empty (and the textual fallback of IsTrue/IsFalse) on `s`; ranges are offset by `offset`.
    bool matchText(const Node& n, QStringView s, int offset, QList<MatchRange>* ranges)
    {
        switch (n.kind) {
        case NodeKind::Empty:
            return true;
        case NodeKind::Text:
        case NodeKind::Phrase:
        case NodeKind::IsTrue:
        case NodeKind::IsFalse: {
            const qsizetype len = n.text.size();
            if (len == 0)
                return true;
            qsizetype pos = n.matcher.indexIn(s, 0);
            if (pos < 0)
                return false;
            if (!ranges)
                return true;
            int count = 0;
            while (pos >= 0) {
                ranges->push_back({int(pos) + offset, int(pos + len) + offset});
                if (++count % kCancelInterval == 0 && checkCancel())
                    return false;
                pos = n.matcher.indexIn(s, pos + len);
            }
            return true;
        }
        case NodeKind::Regex: {
            if (!n.regex.isValid())
                return false;
            if (!ranges)
                return n.regex.matchView(s).hasMatch();
            bool any = false;
            int count = 0;
            QRegularExpressionMatchIterator it = n.regex.globalMatchView(s);
            while (it.hasNext()) {
                const QRegularExpressionMatch m = it.next();
                any = true;
                if (m.capturedLength(0) > 0)
                    ranges->push_back({int(m.capturedStart(0)) + offset, int(m.capturedEnd(0)) + offset});
                if (++count % kCancelInterval == 0 && checkCancel())
                    return false;
            }
            return any;
        }
        default:
            return false;
        }
    }

    int lineStartOffset(int line)
    {
        if (m_lineStarts.empty()) {
            m_lineStarts.push_back(0);
            for (qsizetype i = 0; i < m_content.size(); ++i)
                if (m_content[i] == u'\n')
                    m_lineStarts.push_back(int(i + 1));
        }
        return line >= 0 && size_t(line) < m_lineStarts.size() ? m_lineStarts[size_t(line)] : -1;
    }

    // Tag name without '#', case-folded (cached per document).
    QStringView foldedTag(qsizetype i)
    {
        if (m_foldedTags.isEmpty() && !m_doc.tags.isEmpty()) {
            m_foldedTags.reserve(m_doc.tags.size());
            for (const SearchTag& t : m_doc.tags)
                m_foldedTags.push_back((t.tag.startsWith(u'#') ? t.tag.mid(1) : t.tag).toCaseFolded());
        }
        return m_foldedTags[i];
    }

    bool tagMatches(const Node& n, qsizetype i)
    {
        const QString& raw = m_doc.tags[i].tag;
        switch (n.kind) {
        case NodeKind::Empty:
            return true;
        case NodeKind::Text:
        case NodeKind::Phrase: {
            const QString& want = n.tagName;
            if (want.isEmpty())
                return true;
            const QStringView have = n.caseSensitive ? (raw.startsWith(u'#') ? QStringView(raw).mid(1) : QStringView(raw)) : foldedTag(i);
            if (want.endsWith(u'/')) // "tag:#a/" : nested tags only
                return have.startsWith(want);
            return have == want || (have.size() > want.size() && have.startsWith(want) && have[want.size()] == u'/');
        }
        case NodeKind::Regex: {
            if (!n.regex.isValid())
                return false;
            const QString full = raw.startsWith(u'#') ? raw : QString(u'#' + raw); // regexes see "#tag"
            return n.regex.matchView(full).hasMatch();
        }
        default:
            return false;
        }
    }

    bool matchTags(const Node& n, const View& v, Hits* out)
    {
        bool any = false;
        for (qsizetype i = 0; i < m_doc.tags.size(); ++i) {
            const SearchTag& t = m_doc.tags[i];
            if (!v.whole) { // only tags written inside the region
                const int p = t.from >= 0 ? t.from : lineStartOffset(t.line);
                if (p < v.from || p >= v.to)
                    continue;
            }
            if (!tagMatches(n, i))
                continue;
            any = true;
            if (!out)
                return true;
            if (t.from >= 0 && t.to > t.from && t.to <= m_content.size())
                out->content.push_back({t.from, t.to});
        }
        return any;
    }

    // Paragraphs separated by blank lines: the block fallback for documents without block structure.
    const QList<TextRange>& blocks()
    {
        if (!m_doc.blocks.isEmpty())
            return m_doc.blocks;
        if (!m_paragraphsReady) {
            m_paragraphsReady = true;
            const int size = int(m_content.size());
            int start = -1;
            int end = -1;
            int pos = 0;
            while (pos < size) {
                const int lineEnd = detail::lineEndOf(m_content, pos);
                bool blank = true;
                for (int i = pos; i < lineEnd && blank; ++i)
                    blank = m_content[i].isSpace();
                if (!blank) {
                    if (start < 0)
                        start = pos;
                    end = lineEnd;
                } else if (start >= 0) {
                    m_paragraphs.push_back({start, end});
                    start = -1;
                }
                const qsizetype nl = m_content.indexOf(u'\n', lineEnd);
                if (nl < 0)
                    break;
                pos = int(nl + 1);
            }
            if (start >= 0)
                m_paragraphs.push_back({start, end});
        }
        return m_paragraphs;
    }

    bool evalRegion(const Node& n, const View& v, Hits* out)
    {
        if (!m_doc.hasContent || n.children.isEmpty() || !n.children[0])
            return false;
        const Node& child = *n.children[0];
        if (isMonotone(child) && !eval(child, v, nullptr))
            return false; // it cannot match in any region if it does not match the enclosing view
        bool any = false;
        int visited = 0;
        const auto visit = [&](int from, int to) { // false: stop iterating
            if (++visited % kCancelInterval == 0 && checkCancel())
                return false;
            const View rv{from, to, false};
            if (!out) {
                if (eval(child, rv, nullptr)) {
                    any = true;
                    return false;
                }
                return !m_canceled;
            }
            Hits local;
            if (eval(child, rv, &local)) {
                any = true;
                out->append(std::move(local));
            }
            return !m_canceled;
        };
        const int size = int(m_content.size());
        const auto clipVisit = [&](const TextRange& r) {
            const int from = std::max({r.from, v.from, 0});
            const int to = std::min({r.to, v.to, size});
            return from >= to || visit(from, to);
        };
        switch (n.region) {
        case Region::Line: {
            int pos = v.from;
            while (pos < v.to) {
                const qsizetype nl = m_content.indexOf(u'\n', pos);
                const int next = nl < 0 || nl >= v.to ? v.to : int(nl);
                int end = next;
                if (end > pos && m_content[end - 1] == u'\r')
                    --end;
                if (!visit(pos, end))
                    break;
                pos = next + 1;
            }
            break;
        }
        case Region::Block:
            for (const TextRange& b : blocks())
                if (!clipVisit(b))
                    break;
            break;
        case Region::Section:
            if (m_doc.sections.isEmpty()) {
                visit(v.from, v.to);
                break;
            }
            for (const TextRange& s : m_doc.sections)
                if (!clipVisit(s))
                    break;
            break;
        case Region::Task:
        case Region::TaskTodo:
        case Region::TaskDone:
            for (const SearchTask& t : m_doc.tasks) {
                const bool done = !t.status.isNull() && t.status != u' ';
                if ((n.region == Region::TaskTodo && done) || (n.region == Region::TaskDone && !done))
                    continue;
                if (!clipVisit(t.range))
                    break;
            }
            break;
        }
        return any && !m_canceled;
    }

    // Sub-queries of a property key: text matching on the key string.
    bool evalKey(const Node& n, QStringView key, QList<MatchRange>* ranges)
    {
        switch (n.kind) {
        case NodeKind::And: {
            QList<MatchRange> local;
            for (const NodePtr& c : n.children)
                if (!evalKey(*c, key, ranges ? &local : nullptr))
                    return false;
            if (ranges)
                ranges->append(std::move(local));
            return true;
        }
        case NodeKind::Or: {
            bool any = false;
            for (const NodePtr& c : n.children) {
                if (evalKey(*c, key, ranges)) {
                    any = true;
                    if (!ranges)
                        return true;
                }
            }
            return any;
        }
        case NodeKind::Not:
            return !evalKey(*n.children.value(0), key, nullptr);
        case NodeKind::Scope:
        case NodeKind::Case:
            return evalKey(*n.children.value(0), key, ranges);
        case NodeKind::Text:
        case NodeKind::Phrase:
        case NodeKind::Regex:
        case NodeKind::Empty:
            return matchText(n, key, 0, ranges);
        default:
            return false;
        }
    }

    static bool compareValue(const Node& n, const QVariant& value)
    {
        if (!value.isValid() || value.typeId() == QMetaType::Bool || value.typeId() == QMetaType::QVariantMap)
            return false;
        const std::optional<double> number = numericValue(value);
        if (n.number) { // numeric comparison; a non-numeric value compares false (NaN in JavaScript)
            if (!number)
                return false;
            return n.compareOp == '>' ? *number > *n.number : *number < *n.number;
        }
        if (value.typeId() != QMetaType::QString)
            return false; // a number against a non-numeric operand
        const int c = QString::compare(value.toString(), n.text, n.caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive);
        return n.compareOp == '>' ? c > 0 : c < 0;
    }

    // Sub-queries of a property value, evaluated against one scalar value (list elements are tried one by one).
    bool evalValue(const Node& n, const QVariant& value, QList<MatchRange>* ranges)
    {
        switch (n.kind) {
        case NodeKind::And: {
            QList<MatchRange> local;
            for (const NodePtr& c : n.children)
                if (!evalValue(*c, value, ranges ? &local : nullptr))
                    return false;
            if (ranges)
                ranges->append(std::move(local));
            return true;
        }
        case NodeKind::Or: {
            bool any = false;
            for (const NodePtr& c : n.children) {
                if (evalValue(*c, value, ranges)) {
                    any = true;
                    if (!ranges)
                        return true;
                }
            }
            return any;
        }
        case NodeKind::Not:
            return !evalValue(*n.children.value(0), value, nullptr);
        case NodeKind::Scope:
        case NodeKind::Case:
            return evalValue(*n.children.value(0), value, ranges);
        case NodeKind::Empty:
            return true;
        case NodeKind::IsNull:
            return !value.isValid();
        case NodeKind::Compare:
            return compareValue(n, value);
        case NodeKind::IsTrue:
        case NodeKind::IsFalse:
            if (value.typeId() == QMetaType::Bool)
                return value.toBool() == (n.kind == NodeKind::IsTrue);
            [[fallthrough]]; // otherwise like the text "true"/"false"
        case NodeKind::Text:
        case NodeKind::Phrase:
        case NodeKind::Regex: {
            if (!value.isValid())
                return false;
            const QString text = propertyValueText(value);
            return matchText(n, text, 0, ranges);
        }
        case NodeKind::Region:
        case NodeKind::Property:
            return false;
        }
        return false;
    }

    bool evalProperty(const Node& n, Hits* out)
    {
        const Node* keyQuery = n.children.value(0).get();
        const Node* valueQuery = n.children.value(1).get();
        bool any = false;
        for (const SearchProperty& p : m_doc.properties) {
            QList<MatchRange> keyRanges;
            if (keyQuery && !evalKey(*keyQuery, p.key, out ? &keyRanges : nullptr))
                continue;
            if (!valueQuery) {
                any = true;
                if (!out)
                    return true;
                out->props.push_back({p.key, QString(), keyRanges, {}});
                continue;
            }
            const auto tryValue = [&](const QVariant& value) {
                QList<MatchRange> valueRanges;
                if (!evalValue(*valueQuery, value, out ? &valueRanges : nullptr))
                    return false;
                any = true;
                if (out)
                    out->props.push_back({p.key, propertyValueText(value), keyRanges, mergeRanges(std::move(valueRanges))});
                return true;
            };
            if (isListValue(p.value)) {
                for (const QVariant& element : p.value.toList())
                    if (tryValue(element) && !out)
                        return true;
            } else if (tryValue(p.value) && !out) {
                return true;
            }
        }
        return any;
    }

    const SearchDocument& m_doc;
    const QStringView m_content;
    const QStringView m_pathNoExt;
    const QStringView m_name;
    const int m_nameOffset;
    const std::atomic<bool>* m_cancel;
    bool m_canceled = false;
    bool m_paragraphsReady = false;
    QList<TextRange> m_paragraphs;
    QStringList m_foldedTags;
    std::vector<int> m_lineStarts;
};

bool admissible(const SearchQuery& query, const SearchDocument& doc)
{
    return !query.isEmpty() && (doc.hasContent || query.canMatchNonNotes());
}

} // namespace

QString propertyValueText(const QVariant& value)
{
    if (!value.isValid())
        return QString();
    switch (value.typeId()) {
    case QMetaType::Bool:
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    case QMetaType::Double:
    case QMetaType::Float: {
        const double d = value.toDouble();
        if (std::isnan(d))
            return QStringLiteral("NaN");
        if (std::isinf(d))
            return d > 0 ? QStringLiteral("Infinity") : QStringLiteral("-Infinity");
        if (d == std::trunc(d) && std::abs(d) < 1e21)
            return QString::number(d, 'f', 0); // JavaScript prints integral numbers without a fraction
        return QString::number(d, 'g', QLocale::FloatingPointShortest);
    }
    case QMetaType::QVariantList:
    case QMetaType::QStringList: {
        QStringList parts;
        for (const QVariant& e : value.toList())
            parts << propertyValueText(e);
        return parts.join(QLatin1StringView(", "));
    }
    case QMetaType::QVariantMap:
        return QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(value.toMap())).toJson(QJsonDocument::Compact));
    default:
        return value.toString();
    }
}

bool documentMatches(const SearchQuery& query, const SearchDocument& doc, const std::atomic<bool>* cancel)
{
    if (!admissible(query, doc))
        return false;
    Evaluator ev(doc, cancel);
    return ev.eval(*query.root(), ev.wholeView(), nullptr) && !ev.canceled();
}

std::optional<SearchResult> matchDocument(const SearchQuery& query, const SearchDocument& doc, const std::atomic<bool>* cancel)
{
    if (!admissible(query, doc))
        return std::nullopt;
    Evaluator ev(doc, cancel);
    const View whole = ev.wholeView();
    // Pass 1 stops at the first hit of every term, so non-matching documents are rejected cheaply.
    if (!ev.eval(*query.root(), whole, nullptr) || ev.canceled())
        return std::nullopt;
    // Pass 2 collects the highlights. Negated top-level terms were verified above and never highlight.
    Hits hits;
    const Node& root = *query.root();
    if (root.kind == NodeKind::And) {
        for (const NodePtr& c : root.children)
            if (c->kind != NodeKind::Not)
                ev.eval(*c, whole, &hits);
    } else if (root.kind != NodeKind::Not) {
        ev.eval(root, whole, &hits);
    }
    if (ev.canceled())
        return std::nullopt;

    SearchResult r;
    r.path = doc.path;
    r.fileMatches = mergeRanges(std::move(hits.file));
    const QList<MatchRange> content = mergeRanges(std::move(hits.content));
    r.contentMatches.reserve(content.size());
    const QChar* text = doc.content.constData();
    int line = 0;
    int pos = 0;
    for (const MatchRange& m : content) {
        for (; pos < m.from; ++pos) {
            if (text[pos] == u'\n' && ++line % kCancelInterval == 0 && ev.checkCancel())
                return std::nullopt;
        }
        r.contentMatches.push_back({m.from, m.to, line});
    }
    // A property term inside a region operator matches once per region: keep each (key, value) once.
    QSet<QString> seenProperties;
    for (PropertyMatch& p : hits.props) {
        if (!seenProperties.contains(p.key + QChar(0) + p.value)) {
            seenProperties.insert(p.key + QChar(0) + p.value);
            r.propertyMatches.push_back(std::move(p));
        }
    }
    return r;
}

} // namespace oc::search
