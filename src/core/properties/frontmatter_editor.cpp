#include "core/properties/frontmatter_editor.h"

#include "core/properties/property_yaml.h"
#include "markdown/metadata.h"
#include "text/diff.h"

#include <yaml-cpp/yaml.h>

#include <cmath>

namespace oc::props {

// One piece of the frontmatter body: an entry (a top-level key: its leading comment lines, the key
// line and the value's continuation lines) or trivia (blank lines, detached comments).
struct FrontmatterEditor::Item {
    bool entry = false;
    QStringList lines;
    QString key;
    QVariant value;
    // Shape of an entry, from scanning its lines.
    bool shaped = false;    // the fields below describe `lines`
    int keyLine = 0;        // index of the key line in `lines` (after the leading comments)
    int keyEnd = 0;         // end column of the key text on the key line
    int colon = -1;         // column of the ':' after the key
    int valueCol = -1;      // first column of a value on the key line (-1: none -> block value or null)
    int commentCol = -1;    // start of the blanks before a "# comment" ending the key line (-1: none)
    bool blockScalar = false;
    int childIndent = -1;   // indentation of the first content line below the key line
    QList<int> seqItems;    // block sequence: the "- item" lines (indexes into `lines`)
    bool seqBroken = false; // content below the key that is not a single-line "- item" at childIndent
    bool simpleSeq = false; // a block sequence of single-line scalar items matching `value`
};

namespace {

using ItemPtr = std::shared_ptr<FrontmatterEditor::Item>;

bool isBlankText(QStringView s)
{
    for (QChar c : s)
        if (c != u' ' && c != u'\t')
            return false;
    return true;
}

bool isDelimiter(QStringView s) { return s.startsWith(u"---") && isBlankText(s.mid(3)); }

int spacesAt(QStringView s)
{
    int i = 0;
    while (i < s.size() && s[i] == u' ')
        ++i;
    return i;
}

bool isCommentLine(QStringView s)
{
    int i = 0;
    while (i < s.size() && (s[i] == u' ' || s[i] == u'\t'))
        ++i;
    return i < s.size() && s[i] == u'#';
}

bool isWs(QChar c) { return c == u' ' || c == u'\t'; }

bool isNullValue(const QVariant& v) { return !v.isValid() || v.typeId() == QMetaType::Nullptr; }
bool isListValue(const QVariant& v) { return v.typeId() == QMetaType::QVariantList || v.typeId() == QMetaType::QStringList; }
bool isMapValue(const QVariant& v) { return v.typeId() == QMetaType::QVariantMap || v.typeId() == QMetaType::QVariantHash; }
bool isScalarValue(const QVariant& v) { return !isListValue(v) && !isMapValue(v); }

// A quoted or flow value that may continue on the following lines.
struct OpenValue {
    QChar quote;   // '"' or '\'' while inside a quoted scalar
    int depth = 0; // [ { nesting
    bool active() const { return !quote.isNull() || depth > 0; }
};

// After a value ended at `end`: blanks, then nothing or a comment. Returns end (and *comment), or -2.
int afterValue(QStringView s, int end, int* comment)
{
    int j = end;
    while (j < s.size() && isWs(s[j]))
        ++j;
    if (j == s.size())
        return end;
    if (s[j] == u'#' && j > end) {
        *comment = end;
        return end;
    }
    return -2;
}

bool quoteMayStart(QStringView s, int i)
{
    int j = i - 1;
    while (j >= 0 && isWs(s[j]))
        --j;
    return j < 0 || s[j] == u'[' || s[j] == u'{' || s[j] == u',' || s[j] == u':';
}

// Continue scanning a quoted/flow value at column `from`. Returns the column after the value when it
// ends on this line, -1 when it continues on the next one, -2 on junk after it.
int scanOpenValue(QStringView s, int from, OpenValue& st, int* comment)
{
    int i = from;
    while (i < s.size()) {
        const QChar c = s[i];
        if (!st.quote.isNull()) {
            if (st.quote == u'"') {
                if (c == u'\\') {
                    i += 2;
                    continue;
                }
                if (c == u'"') {
                    st.quote = QChar();
                    ++i;
                    if (st.depth == 0)
                        return afterValue(s, i, comment);
                    continue;
                }
            } else if (c == u'\'') {
                if (i + 1 < s.size() && s[i + 1] == u'\'') {
                    i += 2;
                    continue;
                }
                st.quote = QChar();
                ++i;
                if (st.depth == 0)
                    return afterValue(s, i, comment);
                continue;
            }
            ++i;
            continue;
        }
        if (c == u'#' && (i == 0 || isWs(s[i - 1])))
            return -1; // a comment inside a multi-line flow collection
        if (c == u'[' || c == u'{') {
            ++st.depth;
        } else if (c == u']' || c == u'}') {
            if (--st.depth <= 0) {
                st.depth = 0;
                return afterValue(s, i + 1, comment);
            }
        } else if ((c == u'"' || c == u'\'') && quoteMayStart(s, i)) {
            st.quote = c;
        }
        ++i;
    }
    return -1;
}

// Double-quoted YAML key text (without the quotes). False on escapes we don't decode.
bool decodeDoubleQuoted(QStringView s, int from, int* end, QString* out)
{
    for (int j = from; j < s.size(); ++j) {
        const QChar c = s[j];
        if (c == u'"') {
            *end = j + 1;
            return true;
        }
        if (c != u'\\') {
            *out += c;
            continue;
        }
        if (++j >= s.size())
            return false;
        auto hex = [&](int n) -> bool {
            if (j + n >= s.size())
                return false;
            bool ok = false;
            const uint code = s.mid(j + 1, n).toUInt(&ok, 16);
            if (!ok)
                return false;
            if (code > 0xffff) {
                const char32_t cp = code;
                *out += QString::fromUcs4(&cp, 1);
            } else {
                *out += QChar(char16_t(code));
            }
            j += n;
            return true;
        };
        switch (s[j].unicode()) {
        case u'0': *out += QChar(u'\0'); break;
        case u'a': *out += QChar(u'\a'); break;
        case u'b': *out += QChar(u'\b'); break;
        case u't': case u'\t': *out += QChar(u'\t'); break;
        case u'n': *out += QChar(u'\n'); break;
        case u'v': *out += QChar(u'\v'); break;
        case u'f': *out += QChar(u'\f'); break;
        case u'r': *out += QChar(u'\r'); break;
        case u'e': *out += QChar(char16_t(0x1b)); break;
        case u' ': *out += QChar(u' '); break;
        case u'"': *out += QChar(u'"'); break;
        case u'/': *out += QChar(u'/'); break;
        case u'\\': *out += QChar(u'\\'); break;
        case u'N': *out += QChar(char16_t(0x85)); break;
        case u'_': *out += QChar(char16_t(0xa0)); break;
        case u'L': *out += QChar(char16_t(0x2028)); break;
        case u'P': *out += QChar(char16_t(0x2029)); break;
        case u'x': if (!hex(2)) return false; break;
        case u'u': if (!hex(4)) return false; break;
        case u'U': if (!hex(8)) return false; break;
        default: return false;
        }
    }
    return false; // unterminated (multi-line keys are not mapped)
}

struct KeyLine {
    QString key;
    int keyEnd = 0;
    int colon = -1;
    int valueCol = -1;
    int commentCol = -1;
    bool blockScalar = false;
    OpenValue open;
};

// A top-level "key: value" line. False when it isn't one we can map (explicit/complex keys, anchors,
// aliases, tags, junk after a quoted value, ...).
bool parseKeyLine(QStringView s, KeyLine& k)
{
    if (s.isEmpty())
        return false;
    const QChar c0 = s[0];
    int i = 0;
    if (c0 == u'"') {
        if (!decodeDoubleQuoted(s, 1, &k.keyEnd, &k.key))
            return false;
        i = k.keyEnd;
    } else if (c0 == u'\'') {
        int j = 1;
        for (; j < s.size(); ++j) {
            if (s[j] == u'\'') {
                if (j + 1 < s.size() && s[j + 1] == u'\'') {
                    k.key += u'\'';
                    ++j;
                    continue;
                }
                break;
            }
            k.key += s[j];
        }
        if (j >= s.size())
            return false;
        k.keyEnd = i = j + 1;
    } else {
        static const QString indicators = QStringLiteral("?[]{}&*!|>%@`,");
        if (indicators.contains(c0) || (c0 == u'-' && (s.size() == 1 || isWs(s[1]))))
            return false;
        int colon = -1;
        for (int j = 0; j < s.size(); ++j) {
            if (s[j] == u':' && (j + 1 == s.size() || isWs(s[j + 1]))) {
                colon = j;
                break;
            }
            if (s[j] == u'#' && j > 0 && isWs(s[j - 1]))
                return false;
        }
        if (colon <= 0)
            return false;
        int end = colon;
        while (end > 0 && isWs(s[end - 1]))
            --end;
        if (end == 0)
            return false;
        k.key = s.left(end).toString();
        k.keyEnd = end;
        i = end;
    }
    while (i < s.size() && isWs(s[i]))
        ++i;
    if (i >= s.size() || s[i] != u':')
        return false;
    k.colon = i;
    if (i + 1 < s.size() && !isWs(s[i + 1]))
        return false;
    int v = i + 1;
    while (v < s.size() && isWs(s[v]))
        ++v;
    if (v == s.size())
        return true; // "key:" (block value or null)
    const QChar c = s[v];
    if (c == u'#') {
        k.commentCol = i + 1;
        return true;
    }
    k.valueCol = v;
    if (c == u'&' || c == u'*' || c == u'!' || c == u'%' || c == u'@' || c == u'`')
        return false;
    if (c == u'|' || c == u'>') {
        k.blockScalar = true;
        int j = v + 1;
        while (j < s.size() && (s[j].isDigit() || s[j] == u'+' || s[j] == u'-'))
            ++j;
        return afterValue(s, j, &k.commentCol) != -2;
    }
    if (c == u'"' || c == u'\'' || c == u'[' || c == u'{') {
        if (c == u'"' || c == u'\'')
            k.open.quote = c;
        else
            k.open.depth = 1;
        return scanOpenValue(s, v + 1, k.open, &k.commentCol) != -2;
    }
    // plain scalar: an inline comment starts at a '#' after a blank
    for (int j = v + 1; j < s.size(); ++j) {
        if (s[j] == u'#' && isWs(s[j - 1])) {
            int ws = j;
            while (ws > v && isWs(s[ws - 1]))
                --ws;
            k.commentCol = ws;
            break;
        }
    }
    return true;
}

// A content line below an entry's key line: note the child indentation and sequence items.
void noteContent(FrontmatterEditor::Item& e, int index)
{
    const QString& s = e.lines[index];
    const int ind = spacesAt(s);
    if (e.childIndent < 0)
        e.childIndent = ind;
    if (e.blockScalar || e.valueCol >= 0)
        return;
    const QStringView rest = QStringView(s).mid(ind);
    const bool item = ind == e.childIndent && rest.startsWith(u'-') && (rest.size() == 1 || isWs(rest[1]));
    if (!item) {
        e.seqBroken = true;
        return;
    }
    e.seqItems << index;
    // the item must be a single-line scalar: no block scalar, no open quote/flow, no nested "k: v"
    int v = 1;
    while (v < rest.size() && isWs(rest[v]))
        ++v;
    if (v >= rest.size())
        return;
    const QChar c = rest[v];
    if (c == u'|' || c == u'>' || c == u'&' || c == u'*' || c == u'!' || c == u'-' || c == u'?') {
        e.seqBroken = true;
        return;
    }
    if (c == u'"' || c == u'\'' || c == u'[' || c == u'{') {
        OpenValue st;
        if (c == u'"' || c == u'\'')
            st.quote = c;
        else
            st.depth = 1;
        int comment = -1;
        if (scanOpenValue(rest, v + 1, st, &comment) < 0)
            e.seqBroken = true;
        return;
    }
    for (int j = v; j < rest.size(); ++j) {
        if (rest[j] == u':' && (j + 1 == rest.size() || isWs(rest[j + 1]))) {
            e.seqBroken = true; // "- key: value" (a mapping item)
            return;
        }
        if (rest[j] == u'#' && isWs(rest[j - 1]))
            return;
    }
}

struct Scan {
    QList<ItemPtr> items;
    bool ok = true;
};

Scan scanLines(const QStringList& lines)
{
    Scan r;
    ItemPtr cur;
    QList<int> pending;
    OpenValue open;
    auto trivia = [&](const QList<int>& idx) {
        if (idx.isEmpty())
            return;
        auto t = std::make_shared<FrontmatterEditor::Item>();
        for (int i : idx)
            t->lines << lines[i];
        r.items << t;
    };
    auto absorb = [&] {
        for (int i : std::as_const(pending))
            cur->lines << lines[i];
        pending.clear();
    };
    for (int i = 0; i < lines.size(); ++i) {
        const QString& s = lines[i];
        if (cur && open.active()) {
            absorb();
            cur->lines << s;
            int comment = -1;
            if (scanOpenValue(s, 0, open, &comment) == -2) {
                r.ok = false;
                return r;
            }
            continue;
        }
        if (isBlankText(s)) {
            pending << i;
            continue;
        }
        if (isWs(s[0])) {
            if (!cur) {
                r.ok = false;
                return r;
            }
            if (!cur->blockScalar && isCommentLine(s)) {
                pending << i;
                continue;
            }
            absorb();
            cur->lines << s;
            noteContent(*cur, int(cur->lines.size()) - 1);
            continue;
        }
        if (s[0] == u'#') {
            pending << i;
            continue;
        }
        if (s[0] == u'-' && (s.size() == 1 || isWs(s[1]))) {
            // a compact block sequence under "key:"
            if (!cur || cur->valueCol >= 0 || cur->blockScalar) {
                r.ok = false;
                return r;
            }
            absorb();
            cur->lines << s;
            noteContent(*cur, int(cur->lines.size()) - 1);
            continue;
        }
        // A new key. The comment lines directly above it belong to it.
        QList<int> lead;
        while (!pending.isEmpty() && pending.last() == i - 1 - lead.size() && isCommentLine(lines[pending.last()]))
            lead.prepend(pending.takeLast());
        trivia(pending);
        pending.clear();
        KeyLine kl;
        if (!parseKeyLine(s, kl)) {
            r.ok = false;
            return r;
        }
        auto e = std::make_shared<FrontmatterEditor::Item>();
        e->entry = true;
        for (int li : std::as_const(lead))
            e->lines << lines[li];
        e->keyLine = int(lead.size());
        e->lines << s;
        e->key = kl.key;
        e->keyEnd = kl.keyEnd;
        e->colon = kl.colon;
        e->valueCol = kl.valueCol;
        e->commentCol = kl.commentCol;
        e->blockScalar = kl.blockScalar;
        e->shaped = true;
        open = kl.open;
        r.items << e;
        cur = e;
    }
    if (open.active()) {
        r.ok = false;
        return r;
    }
    trivia(pending);
    return r;
}

// Is the entry's value a block sequence of single-line scalars matching its lines one to one?
void checkSimpleSeq(FrontmatterEditor::Item& e)
{
    e.simpleSeq = false;
    if (!e.shaped || e.seqBroken || e.seqItems.isEmpty() || e.valueCol >= 0 || e.blockScalar || !isListValue(e.value))
        return;
    const QVariantList values = e.value.toList();
    if (values.size() != e.seqItems.size())
        return;
    for (const QVariant& v : values)
        if (!isScalarValue(v))
            return;
    e.simpleSeq = true;
}

// Re-derive an entry's shape from its lines (after an edit).
void reshape(FrontmatterEditor::Item& e)
{
    const Scan s = scanLines(e.lines);
    const ItemPtr only = s.ok && s.items.size() == 1 && s.items[0]->entry ? s.items[0] : nullptr;
    const QString key = e.key;
    const QVariant value = e.value;
    const QStringList lines = e.lines;
    if (only) {
        e = *only;
    } else {
        e.shaped = false;
        e.seqItems.clear();
    }
    e.entry = true;
    e.key = key;
    e.value = value;
    e.lines = lines;
    checkSimpleSeq(e);
}

ItemPtr makeEntry(const QString& key, const QVariant& value)
{
    auto e = std::make_shared<FrontmatterEditor::Item>();
    e->entry = true;
    e->key = key;
    e->value = value;
    e->lines = yamlEntry(key, value).split(u'\n');
    reshape(*e);
    return e;
}

enum class YamlRoot { Null, Map, Other, Error };

YamlRoot probeYaml(const QString& yaml, QString* error, int* errorLine)
{
    try {
        const YAML::Node root = YAML::Load(yaml.toStdString());
        if (!root.IsDefined() || root.IsNull())
            return YamlRoot::Null;
        return root.IsMap() ? YamlRoot::Map : YamlRoot::Other;
    } catch (const YAML::Exception& e) {
        *error = QString::fromStdString(e.msg);
        *errorLine = e.mark.line >= 0 ? e.mark.line : -1;
        return YamlRoot::Error;
    } catch (const std::exception& e) {
        *error = QString::fromUtf8(e.what());
        return YamlRoot::Error;
    }
}

bool isFalsy(const QVariant& v)
{
    if (isNullValue(v))
        return true;
    switch (v.typeId()) {
    case QMetaType::Bool:
        return !v.toBool();
    case QMetaType::QString:
        return v.toString().isEmpty();
    case QMetaType::Double:
    case QMetaType::Float:
    case QMetaType::Int:
    case QMetaType::LongLong:
    case QMetaType::UInt:
    case QMetaType::ULongLong: {
        const double d = v.toDouble();
        return d == 0 || std::isnan(d);
    }
    default:
        return false;
    }
}

} // namespace

FrontmatterSpan findFrontmatter(const Text& text)
{
    FrontmatterSpan s;
    if (text.lineCount() < 2 || !isDelimiter(text.lineView(0)))
        return s;
    for (int l = 1; l < text.lineCount(); ++l) {
        if (isDelimiter(text.lineView(l))) {
            s.exists = true;
            s.openLine = 0;
            s.closeLine = l;
            return s;
        }
    }
    return s;
}

QVariant mergePropertyValue(const QVariant& existing, const QVariant& incoming, bool* changed)
{
    auto result = [&](const QVariant& v) {
        if (changed)
            *changed = v != existing || existing.typeId() != v.typeId() || !existing.isValid();
        return v;
    };
    if (isFalsy(existing))
        return result(incoming);
    if (isListValue(existing) && isListValue(incoming)) {
        QVariantList out = existing.toList();
        for (const QVariant& v : incoming.toList())
            if (!out.contains(v))
                out << v;
        QVariantList unique;
        for (const QVariant& v : std::as_const(out))
            if (!unique.contains(v))
                unique << v;
        return result(unique);
    }
    if (isMapValue(existing) && isMapValue(incoming)) {
        QVariantMap out = existing.toMap();
        const QVariantMap in = incoming.toMap();
        for (auto it = in.cbegin(); it != in.cend(); ++it)
            out.insert(it.key(), mergePropertyValue(out.value(it.key()), it.value()));
        return result(out);
    }
    if (!isNullValue(incoming))
        return result(incoming);
    if (changed)
        *changed = false;
    return existing;
}

FrontmatterEditor::FrontmatterEditor(const Text& text)
    : m_text(text)
{
    m_span = findFrontmatter(text);
    if (!m_span.exists)
        return;
    for (int l = m_span.openLine + 1; l < m_span.closeLine; ++l)
        m_original << text.line(l);
    const QString yaml = m_original.join(u'\n');
    int errorLine = -1;
    const YamlRoot root = probeYaml(yaml, &m_error, &errorLine);
    auto wholeAsTrivia = [&] {
        auto t = std::make_shared<Item>();
        t->lines = m_original;
        m_items = {t};
    };
    if (root == YamlRoot::Error || root == YamlRoot::Other) {
        m_state = State::Invalid;
        if (m_error.isEmpty())
            m_error = QStringLiteral("Frontmatter must be a valid object!");
        m_errorLine = errorLine >= 0 ? m_span.openLine + 1 + errorLine : -1;
        wholeAsTrivia();
        return;
    }
    QList<md::FrontmatterValue> values;
    if (root == YamlRoot::Map) {
        QString err;
        values = md::parseFrontmatter(yaml, &err);
        if (!err.isEmpty()) {
            m_state = State::Invalid;
            m_error = err;
            wholeAsTrivia();
            return;
        }
        for (int i = 0; i < values.size(); ++i) {
            for (int j = 0; j < i; ++j) {
                if (values[i].key == values[j].key) {
                    m_state = State::Invalid;
                    m_error = QStringLiteral("Map keys must be unique");
                    wholeAsTrivia();
                    return;
                }
            }
        }
    }
    m_state = State::Valid;
    Scan scan = scanLines(m_original);
    if (scan.ok) {
        int n = 0;
        for (const ItemPtr& it : std::as_const(scan.items)) {
            if (!it->entry)
                continue;
            if (n >= values.size() || values[n].key != it->key) {
                scan.ok = false;
                break;
            }
            it->value = values[n++].value;
            checkSimpleSeq(*it);
        }
        if (n != values.size())
            scan.ok = false;
    }
    if (scan.ok) {
        m_items = scan.items;
        return;
    }
    // Not mappable: keep the lines as they are until an edit rewrites the block.
    m_mappable = false;
    wholeAsTrivia();
    for (const md::FrontmatterValue& v : std::as_const(values)) {
        auto e = std::make_shared<Item>();
        e->entry = true;
        e->key = v.key;
        e->value = v.value;
        m_unmapped << e;
    }
}

FrontmatterEditor::~FrontmatterEditor() = default;

bool FrontmatterEditor::ensureRewriteMode()
{
    if (m_state == State::Invalid)
        return false;
    if (m_mappable)
        return true;
    // Obsidian's behaviour: the whole block is serialized again.
    m_items.clear();
    for (const ItemPtr& e : std::as_const(m_unmapped))
        m_items << makeEntry(e->key, e->value);
    m_unmapped.clear();
    m_mappable = true;
    return true;
}

QList<Property> FrontmatterEditor::properties() const
{
    QList<Property> out;
    const QList<ItemPtr>& src = m_mappable ? m_items : m_unmapped;
    if (m_cleared)
        return out;
    for (const ItemPtr& it : src)
        if (it->entry)
            out.append({it->key, it->value});
    return out;
}

int FrontmatterEditor::entryIndex(const QString& key) const
{
    for (int i = 0; i < m_items.size(); ++i)
        if (m_items[i]->entry && m_items[i]->key == key)
            return i;
    return -1;
}

bool FrontmatterEditor::has(const QString& key) const
{
    for (const Property& p : properties())
        if (p.key == key)
            return true;
    return false;
}

QVariant FrontmatterEditor::value(const QString& key) const
{
    for (const Property& p : properties())
        if (p.key == key)
            return p.value;
    return {};
}

void FrontmatterEditor::setEntryValue(Item& e, const QVariant& value)
{
    if (!e.shaped) {
        e.value = value;
        e.lines = e.lines.mid(0, e.keyLine) + yamlEntry(e.key, value).split(u'\n');
        reshape(e);
        return;
    }
    const QString keyLine = e.lines[e.keyLine];
    const QString prefix = keyLine.left(e.colon + 1);
    const QString comment = e.commentCol >= 0 ? keyLine.mid(e.commentCol) : QString();

    // A block list that stays a list of one-line scalars: keep the lines of unchanged items.
    bool newItemsSingleLine = isListValue(value) && !value.toList().isEmpty();
    if (newItemsSingleLine) {
        for (const QVariant& v : value.toList()) {
            bool single = false;
            yamlInlineItem(v, &single);
            if (!single) {
                newItemsSingleLine = false;
                break;
            }
        }
    }
    if (e.simpleSeq && newItemsSingleLine) {
        const QVariantList oldValues = e.value.toList();
        const QVariantList newValues = value.toList();
        QHash<QString, int> ids;
        auto idOf = [&](const QVariant& v) {
            const QString canon = yamlInlineItem(v, nullptr);
            auto it = ids.constFind(canon);
            if (it != ids.cend())
                return *it;
            const int id = int(ids.size());
            ids.insert(canon, id);
            return id;
        };
        std::vector<int> a, b;
        for (const QVariant& v : oldValues)
            a.push_back(idOf(v));
        for (const QVariant& v : newValues)
            b.push_back(idOf(v));
        QHash<int, int> matched; // old item -> new item
        for (const auto& [i, j] : diffSequences(a, b))
            matched.insert(i, j);
        const QString indent(std::max(0, e.childIndent), u' ');
        auto fresh = [&](int j) { return indent + QStringLiteral("- ") + yamlInlineItem(newValues[j], nullptr); };
        QStringList out = e.lines.mid(0, e.keyLine + 1);
        int next = 0, oldItem = 0;
        for (int li = e.keyLine + 1; li < e.lines.size(); ++li) {
            if (!e.seqItems.contains(li)) {
                out << e.lines[li];
                continue;
            }
            const int oi = oldItem++;
            const auto m = matched.constFind(oi);
            if (m == matched.cend())
                continue; // removed
            while (next < *m)
                out << fresh(next++);
            out << e.lines[li];
            next = *m + 1;
        }
        while (next < newValues.size())
            out << fresh(next++);
        e.lines = out;
        e.value = value;
        reshape(e);
        return;
    }

    const QStringList parts = yamlPairValue(value).split(u'\n');
    const QString first = parts.first(); // "" or " value" / " |-" / " []"
    QStringList rest = parts.mid(1);
    QString line = prefix;
    if (!first.isEmpty()) {
        const bool blockHeader = first.startsWith(QLatin1String(" |"));
        QString sep = QStringLiteral(" ");
        if (e.valueCol >= 0 && !e.blockScalar && !blockHeader)
            sep = keyLine.mid(e.colon + 1, e.valueCol - e.colon - 1); // keep aligned values aligned
        line += sep + first.mid(1);
    }
    line += comment;
    // A block collection keeps the indentation its children had (e.g. compact "- item" lists).
    const bool newSeq = isListValue(value) && !value.toList().isEmpty();
    const bool newMap = isMapValue(value) && !value.toMap().isEmpty();
    const bool oldSeq = !e.seqItems.isEmpty() && !e.seqBroken;
    if (!rest.isEmpty() && e.valueCol < 0 && !e.blockScalar && e.childIndent >= 0 && e.childIndent != 2
        && ((newSeq && oldSeq) || (newMap && !oldSeq && e.childIndent > 0))) {
        const QString pad(e.childIndent, u' ');
        for (QString& l : rest)
            if (l.startsWith(QLatin1String("  ")))
                l = pad + l.mid(2);
    }
    e.lines = e.lines.mid(0, e.keyLine) + QStringList{line} + rest;
    e.value = value;
    reshape(e);
}

void FrontmatterEditor::insertEntry(const QString& key, const QVariant& value)
{
    ItemPtr e = makeEntry(key, value);
    int pos = 0;
    for (int i = int(m_items.size()) - 1; i >= 0; --i) {
        Item& it = *m_items[i];
        int lastNonBlank = -1;
        for (int j = 0; j < it.lines.size(); ++j)
            if (!isBlankText(it.lines[j]))
                lastNonBlank = j;
        if (lastNonBlank < 0)
            continue;
        if (!it.entry && lastNonBlank < it.lines.size() - 1) {
            auto tail = std::make_shared<Item>();
            tail->lines = it.lines.mid(lastNonBlank + 1);
            it.lines = it.lines.mid(0, lastNonBlank + 1);
            m_items.insert(i + 1, tail);
        }
        pos = i + 1;
        break;
    }
    m_items.insert(pos, e);
}

bool FrontmatterEditor::set(const QString& key, const QVariant& value)
{
    if (!ensureRewriteMode())
        return false;
    m_cleared = false;
    const int i = entryIndex(key);
    if (i < 0)
        insertEntry(key, value);
    else
        setEntryValue(*m_items[i], value);
    m_modified = true;
    return true;
}

bool FrontmatterEditor::add(const QString& key, const QVariant& value)
{
    if (m_state == State::Invalid || has(key))
        return false;
    return set(key, value);
}

bool FrontmatterEditor::rename(const QString& from, const QString& to)
{
    if (m_state == State::Invalid || !has(from) || to.isEmpty())
        return false;
    if (from == to)
        return true;
    if (has(to))
        return false;
    ensureRewriteMode();
    Item& e = *m_items[entryIndex(from)];
    if (e.shaped) {
        QString& line = e.lines[e.keyLine];
        line = yamlKey(to) + line.mid(e.keyEnd);
        e.key = to;
        reshape(e);
    } else {
        e.key = to;
        e.lines = e.lines.mid(0, e.keyLine) + yamlEntry(to, e.value).split(u'\n');
        reshape(e);
    }
    m_modified = true;
    return true;
}

bool FrontmatterEditor::remove(const QString& key)
{
    if (m_state == State::Invalid || !has(key))
        return false;
    ensureRewriteMode();
    m_items.removeAt(entryIndex(key));
    m_modified = true;
    bool any = false;
    for (const ItemPtr& it : std::as_const(m_items))
        any = any || it->entry;
    if (!any)
        clear(); // Obsidian writes no frontmatter for no properties
    return true;
}

bool FrontmatterEditor::reorder(const QStringList& keys)
{
    if (m_state == State::Invalid)
        return false;
    QStringList current;
    for (const Property& p : properties())
        current << p.key;
    QStringList a = current, b = keys;
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    if (a != b)
        return false;
    if (current == keys)
        return true;
    ensureRewriteMode();
    QList<ItemPtr> ordered;
    for (const QString& k : keys)
        ordered << m_items[entryIndex(k)];
    int n = 0;
    for (ItemPtr& it : m_items)
        if (it->entry)
            it = ordered[n++];
    m_modified = true;
    return true;
}

bool FrontmatterEditor::merge(const QString& key, const QVariant& value)
{
    if (m_state == State::Invalid)
        return false;
    bool changed = false;
    const QVariant merged = mergePropertyValue(has(key) ? this->value(key) : QVariant(), value, &changed);
    if (has(key) && !changed)
        return true;
    return set(key, merged);
}

void FrontmatterEditor::clear()
{
    if (!m_span.exists && m_items.isEmpty())
        return;
    m_items.clear();
    m_unmapped.clear();
    m_mappable = true;
    m_cleared = true;
    m_modified = true;
}

QList<ChangeSpec> FrontmatterEditor::changes() const
{
    if (!m_modified)
        return {};
    QStringList body;
    bool anyEntry = false;
    for (const ItemPtr& it : m_items) {
        body << it->lines;
        anyEntry = anyEntry || it->entry;
    }
    if (m_cleared && !anyEntry) {
        if (!m_span.exists)
            return {};
        const qsizetype to = m_span.closeLine + 1 < m_text.lineCount() ? m_text.lineStart(m_span.closeLine + 1) : m_text.length();
        return {ChangeSpec{0, to, QString()}};
    }
    if (!m_span.exists) {
        if (!anyEntry)
            return {};
        return {ChangeSpec{0, 0, QStringLiteral("---\n") + body.join(u'\n') + QStringLiteral("\n---\n")}};
    }
    // Line diff of the body: untouched lines keep their bytes and terminators.
    QHash<QString, int> ids;
    auto idOf = [&](const QString& s) {
        auto it = ids.constFind(s);
        if (it != ids.cend())
            return *it;
        const int id = int(ids.size());
        ids.insert(s, id);
        return id;
    };
    std::vector<int> a, b;
    for (const QString& l : m_original)
        a.push_back(idOf(l));
    for (const QString& l : std::as_const(body))
        b.push_back(idOf(l));
    auto pairs = diffSequences(a, b);
    pairs.emplace_back(int(a.size()), int(b.size()));
    const int base = m_span.openLine + 1;
    QList<ChangeSpec> out;
    int oi = 0, ni = 0;
    for (const auto& [pa, pb] : pairs) {
        if (pa > oi || pb > ni) {
            const QString inserted = body.mid(ni, pb - ni).join(u'\n');
            if (pa > oi && pb > ni)
                out.append({m_text.lineStart(base + oi), m_text.lineEnd(base + pa - 1), inserted});
            else if (pb > ni)
                out.append({m_text.lineStart(base + oi), m_text.lineStart(base + oi), inserted + u'\n'});
            else
                out.append({m_text.lineStart(base + oi), m_text.lineStart(base + pa), QString()});
        }
        oi = pa + 1;
        ni = pb + 1;
    }
    return out;
}

Text FrontmatterEditor::apply(Eol newEol) const
{
    const QList<ChangeSpec> specs = changes();
    if (specs.isEmpty())
        return m_text;
    return ChangeSet::of(specs, m_text.length()).apply(m_text, newEol);
}

} // namespace oc::props
