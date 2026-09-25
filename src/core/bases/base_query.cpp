#include "core/bases/base_query.h"

#include <QCollator>
#include <QRegularExpression>

#include <cmath>

namespace oc::bases {

// ---------------------------------------------------------------------------------------------
// QueryContext
// ---------------------------------------------------------------------------------------------
std::shared_ptr<QueryContext> QueryContext::create(const DataSource* ds, const BaseFile& base, const BaseView* view, const QString& thisPath)
{
    auto ctx = std::make_shared<QueryContext>();
    ctx->ds = ds;
    ctx->filter = Filter::both(base.filters, view ? view->filters : std::nullopt);
    ctx->formulas = base.formulas;
    ctx->thisPath = thisPath;
    return ctx;
}

std::shared_ptr<Entry> QueryContext::local()
{
    m_localUsed = true;
    if (thisPath.isEmpty())
        return nullptr;
    if (!m_local)
        m_local = std::make_shared<Entry>(std::weak_ptr<QueryContext>(shared_from_this()), thisPath);
    return m_local;
}

FormulaPtr QueryContext::formula(const QString& name) const
{
    for (const auto& [n, f] : formulas)
        if (n == name)
            return f;
    for (const auto& [n, f] : formulas)
        if (n.compare(name, Qt::CaseInsensitive) == 0)
            return f;
    return nullptr;
}

QStringList QueryContext::formulaNames() const
{
    QStringList out;
    for (const auto& [n, f] : formulas)
        out << n;
    return out;
}

// ---------------------------------------------------------------------------------------------
// Entry
// ---------------------------------------------------------------------------------------------
Entry::Entry(std::shared_ptr<QueryContext> ctx, const QString& path)
    : m_ctx(ctx)
    , m_weakCtx(ctx)
    , m_path(path)
{
}

Entry::Entry(std::weak_ptr<QueryContext> ctx, const QString& path)
    : m_weakCtx(std::move(ctx))
    , m_path(path)
{
}

std::shared_ptr<QueryContext> Entry::context() const { return m_ctx ? m_ctx : m_weakCtx.lock(); }

const DataSource* Entry::dataSource() const
{
    const std::shared_ptr<QueryContext> c = context();
    return c ? c->ds : nullptr;
}

const md::CachedMetadata* Entry::metadata() const
{
    const DataSource* ds = dataSource();
    return ds ? ds->metadata(m_path) : nullptr;
}

Value Entry::note()
{
    if (!m_note) {
        const md::CachedMetadata* m = metadata();
        m_note = Value::frontmatterObject(dataSource(), m_path, m && m->frontmatter ? *m->frontmatter : QList<md::FrontmatterValue>());
    }
    return *m_note;
}

Value Entry::file()
{
    if (!m_file)
        m_file = Value::file(dataSource(), m_path);
    return *m_file;
}

Value Entry::getByIdentifier(const QString& id)
{
    const QString lower = id.toLower();
    if (lower == u"this") {
        const std::shared_ptr<QueryContext> c = context();
        const std::shared_ptr<Entry> local = c ? c->local() : nullptr;
        return local ? Value::thisFile(dataSource(), local->path(), local) : Value();
    }
    if (lower == u"note")
        return note();
    if (lower == u"file")
        return file();
    if (lower == u"formula")
        return Value::formulas(shared_from_this(), formulaNames());
    return note().objectAccess(id).value_or(Value());
}

QStringList Entry::keys()
{
    QStringList k = propertyKeys();
    for (const char* s : {"this", "note", "file", "formula"})
        if (!k.contains(QLatin1String(s)))
            k << QLatin1String(s);
    return k;
}

QStringList Entry::formulaNames()
{
    const std::shared_ptr<QueryContext> c = context();
    return c ? c->formulaNames() : QStringList();
}

std::optional<Value> Entry::formulaValue(const QString& name)
{
    const std::shared_ptr<QueryContext> c = context();
    if (!c)
        return std::nullopt;
    // Obsidian's IA(): the formula's own spelling
    QString key = name;
    bool found = false;
    for (const auto& [n, f] : c->formulas)
        if (n == name) {
            found = true;
            break;
        }
    if (!found) {
        for (const auto& [n, f] : c->formulas)
            if (n.compare(name, Qt::CaseInsensitive) == 0) {
                key = n;
                found = true;
                break;
            }
    }
    if (m_evaluating.contains(key))
        throw EvalError{QStringLiteral("Infinite loop detected in formula")};
    if (auto it = m_formulaResults.constFind(key); it != m_formulaResults.cend())
        return *it;
    if (!found)
        return std::nullopt;
    const FormulaPtr f = c->formula(key);
    m_evaluating.insert(key);
    const Value v = f->value(*this);
    m_evaluating.remove(key);
    m_formulaResults.insert(key, v);
    return v;
}

Value Entry::value(const QString& propertyId)
{
    if (auto it = m_values.constFind(propertyId); it != m_values.cend())
        return *it;
    const PropertyRef ref = parsePropertyId(propertyId);
    Value v;
    if (ref.type == u"note")
        v = note().objectAccess(ref.name).value_or(Value());
    else if (ref.type == u"file")
        v = file().objectAccess(ref.name).value_or(Value());
    else
        v = formulaValue(ref.name).value_or(Value());
    m_values.insert(propertyId, v);
    return v;
}

Value Entry::valueOrError(const QString& propertyId)
{
    try {
        return value(propertyId);
    } catch (const EvalError& e) {
        return Value::error(e.message);
    }
}

QStringList Entry::propertyKeys() const
{
    QStringList out;
    if (const md::CachedMetadata* m = metadata(); m && m->frontmatter)
        for (const md::FrontmatterValue& v : *m->frontmatter)
            if (!out.contains(v.key))
                out << v.key;
    return out;
}

QVariant Entry::rawProperty(const QString& key) const
{
    const md::CachedMetadata* m = metadata();
    if (!m || !m->frontmatter)
        return {};
    for (const md::FrontmatterValue& v : *m->frontmatter)
        if (v.key == key)
            return v.value;
    for (const md::FrontmatterValue& v : *m->frontmatter)
        if (v.key.compare(key, Qt::CaseInsensitive) == 0)
            return v.value;
    return {};
}

bool Entry::hasRawProperty(const QString& key) const
{
    const md::CachedMetadata* m = metadata();
    if (!m || !m->frontmatter)
        return false;
    for (const md::FrontmatterValue& v : *m->frontmatter)
        if (v.key.compare(key, Qt::CaseInsensitive) == 0)
            return true;
    return false;
}

void Entry::clearCache()
{
    m_note.reset();
    m_file.reset();
    m_formulaResults.clear();
    m_values.clear();
}

// ---------------------------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------------------------
bool evaluateFile(const std::shared_ptr<QueryContext>& ctx, const EntryPtr& entry, QString* error)
{
    if (!ctx || !ctx->filter)
        return true;
    try {
        return ctx->filter->test(*entry);
    } catch (const EvalError& e) {
        if (error)
            *error = QStringLiteral("Failed to evaluate a filter: ") + e.message;
        return false;
    }
}

int naturalCompare(const QString& a, const QString& b)
{
    thread_local const QCollator collator = [] {
        QCollator c;
        c.setNumericMode(true);
        c.setCaseSensitivity(Qt::CaseInsensitive);
        return c;
    }();
    return collator.compare(a, b);
}

namespace {

// The display text used to sort links / URLs.
QString sortText(const Value& v)
{
    if ((v.type() == ValueType::Link || v.type() == ValueType::Url) && v.display())
        return v.display()->toString();
    return v.toString();
}

// Obsidian's applySort() step for one key: `*final` when the result must not be multiplied by the
// direction (nulls sort last either way).
double compareForSort(const Value& a, const Value& b, bool* final)
{
    *final = false;
    if (a.isNull() && b.isNull())
        return 0;
    if (a.type() == ValueType::Boolean || b.type() == ValueType::Boolean)
        return (a.isTruthy() ? 1 : -1) - (b.isTruthy() ? 1 : -1);
    if (a.isNull()) {
        *final = true;
        return 1;
    }
    if (b.isNull()) {
        *final = true;
        return -1;
    }
    if (a.type() == ValueType::Number && b.type() == ValueType::Number)
        return a.number() - b.number();
    if (a.is(ValueClass::Date) && b.is(ValueClass::Date))
        return a.dateMs() - b.dateMs();
    if (a.type() == ValueType::Duration && b.type() == ValueType::Duration)
        return a.durationMs() - b.durationMs();
    return naturalCompare(sortText(a), sortText(b));
}

QStringList searchTokens(const QString& q)
{
    QStringList out;
    for (const QString& t : q.toLower().split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts))
        out << t;
    return out;
}

} // namespace

int compareValues(const Value& a, const Value& b)
{
    bool final = false;
    const double c = compareForSort(a, b, &final);
    return c < 0 ? -1 : c > 0 ? 1 : 0;
}

QStringList allPropertiesOf(const BaseFile& base, const QList<EntryPtr>& results, const std::function<QString(const QString&)>& propertyName)
{
    QStringList out = fileProperties();
    QSet<QString> seen;
    for (const EntryPtr& e : results) {
        for (const QString& k : e->propertyKeys()) {
            const QString lower = k.toLower();
            if (seen.contains(lower))
                continue;
            seen.insert(lower);
            QString name = propertyName ? propertyName(lower) : QString();
            if (name.isEmpty())
                name = k;
            out << makePropertyId(QStringLiteral("note"), name);
        }
    }
    for (const auto& [n, f] : base.formulas)
        out << makePropertyId(QStringLiteral("formula"), n);
    return out;
}

ViewData buildViewData(const BaseFile& base, const BaseView& view, QList<EntryPtr> results, const QString& search,
                       const std::function<QString(const QString&)>& propertyName)
{
    ViewData d;
    const QStringList order = view.columns();
    // search: every token must appear in one of the view's columns
    const QStringList tokens = searchTokens(search);
    if (!tokens.isEmpty()) {
        QList<EntryPtr> kept;
        for (const EntryPtr& e : std::as_const(results)) {
            QStringList remaining = tokens;
            for (const QString& prop : order) {
                const Value v = e->valueOrError(prop);
                if (v.isNull() || v.isError())
                    continue;
                const QString text = v.toString().toLower();
                for (qsizetype i = remaining.size() - 1; i >= 0; --i)
                    if (text.contains(remaining[i]))
                        remaining.removeAt(i);
                if (remaining.isEmpty())
                    break;
            }
            if (remaining.isEmpty())
                kept << e;
        }
        results = kept;
    }
    std::sort(results.begin(), results.end(), [](const EntryPtr& a, const EntryPtr& b) { return naturalCompare(a->path(), b->path()) < 0; });
    d.allProperties = allPropertiesOf(base, results, propertyName);
    const QSet<QString> known(d.allProperties.cbegin(), d.allProperties.cend());

    // sort
    QList<SortSpec> specs;
    for (const SortSpec& s : view.sortSpecs())
        if (known.contains(s.property))
            specs << s;
    if (specs.isEmpty())
        specs << SortSpec{QStringLiteral("file.name"), QStringLiteral("ASC")};
    std::stable_sort(results.begin(), results.end(), [&](const EntryPtr& a, const EntryPtr& b) {
        for (const SortSpec& s : std::as_const(specs)) {
            Value va, vb;
            try {
                va = a->value(s.property);
                vb = b->value(s.property);
            } catch (const EvalError&) {
                return false;
            }
            bool final = false;
            double c = compareForSort(va, vb, &final);
            if (std::isnan(c))
                return false;
            if (c == 0)
                continue;
            if (!final && s.direction != u"ASC")
                c = -c;
            return c < 0;
        }
        return false;
    });
    d.matched = int(results.size());
    if (view.limit > 0 && results.size() > qsizetype(view.limit))
        results.resize(qsizetype(view.limit));
    d.rows = results;

    // columns
    for (const QString& id : order) {
        if (known.contains(id)) {
            d.columns << id;
            continue;
        }
        const PropertyRef ref = parsePropertyId(id);
        if (ref.type == u"note") {
            QString name = propertyName ? propertyName(ref.name.toLower()) : QString();
            if (name.isEmpty())
                name = ref.name;
            d.columns << makePropertyId(QStringLiteral("note"), name);
        }
    }

    // groups
    if (!view.groupBy) {
        d.groups << ViewGroup{Value(), false, d.rows};
        return d;
    }
    const QString prop = view.groupBy->property;
    QList<ViewGroup> groups;
    QHash<QString, QList<int>> byText; // toString() -> group indexes (fast path)
    for (const EntryPtr& e : std::as_const(d.rows)) {
        const Value key = e->valueOrError(prop);
        const Value k = key.isError() ? Value() : key;
        const QString text = k.toString();
        int found = -1;
        for (int gi : byText.value(text))
            if (Value::looseEquals(groups[gi].key, k)) {
                found = gi;
                break;
            }
        if (found < 0 && groups.size() <= 200) {
            for (int gi = 0; gi < groups.size(); ++gi)
                if (Value::looseEquals(groups[gi].key, k)) {
                    found = gi;
                    break;
                }
        }
        if (found >= 0) {
            groups[found].entries << e;
            continue;
        }
        ViewGroup g;
        g.key = k;
        g.hasKey = !k.isNull();
        g.entries << e;
        byText[text] << int(groups.size());
        groups << g;
    }
    const double dir = view.groupBy->direction == u"ASC" ? 1 : -1;
    std::stable_sort(groups.begin(), groups.end(), [&](const ViewGroup& a, const ViewGroup& b) {
        double c;
        if (a.hasKey && b.hasKey) {
            if (Value::equals(a.key, b.key))
                c = 0;
            else if (a.key.type() == ValueType::Number && b.key.type() == ValueType::Number)
                c = (a.key.number() - b.key.number()) * dir;
            else
                c = naturalCompare(a.key.toString(), b.key.toString()) * dir;
        } else {
            c = a.hasKey ? 1 : b.hasKey ? -1 : 0;
        }
        return c < 0;
    });
    for (qsizetype i = 0; i < groups.size(); ++i) {
        if (groups[i].key.isNull() && i < groups.size() - 1) {
            ViewGroup g = groups.takeAt(i);
            groups << g;
            break;
        }
    }
    d.groups = groups;
    return d;
}

// ---------------------------------------------------------------------------------------------
// Summaries
// ---------------------------------------------------------------------------------------------
namespace {
class ValuesScope : public Scope {
public:
    ValuesScope(const DataSource* ds, const QList<Value>& values) : m_ds(ds), m_values(values) {}
    Value getByIdentifier(const QString& id) override { return id.toLower() == u"values" ? Value::list(m_values) : Value(); }
    QStringList keys() override { return {QStringLiteral("values")}; }
    const DataSource* dataSource() const override { return m_ds; }

private:
    const DataSource* m_ds;
    QList<Value> m_values;
};
} // namespace

std::optional<ValueType> firstValueType(const QList<Value>& values)
{
    for (const Value& v : values)
        if (!v.isNull())
            return v.type();
    return std::nullopt;
}

Value summaryValue(const BaseFile& base, const DataSource* ds, const QList<Value>& values, const QString& name)
{
    const FormulaPtr f = base.summaryFormula(name, firstValueType(values));
    if (!f)
        return Value();
    ValuesScope scope(ds, values);
    return f->value(scope);
}

// ---------------------------------------------------------------------------------------------
// New items
// ---------------------------------------------------------------------------------------------
namespace {

// Obsidian's kQ(): the property a filter's left-hand side is about.
std::optional<PropertyRef> lhsProperty(const ExprNode& e)
{
    using K = ExprNode::Kind;
    if (e.kind == K::ObjectAccess) {
        const ExprNode& o = *e.children[0];
        if (o.kind == K::Ident) {
            if (o.name == u"note" || o.name == u"file" || o.name == u"formula")
                return PropertyRef{o.name, e.name};
            return PropertyRef{QStringLiteral("note"), o.name + u'.' + e.name};
        }
    } else if (e.kind == K::ArrayAccess) {
        const ExprNode& a = *e.children[0];
        const ExprNode& i = *e.children[1];
        if (a.kind == K::Ident && (a.name == u"note" || a.name == u"file" || a.name == u"formula") && i.kind == K::Primitive)
            return PropertyRef{a.name, i.value.toString()};
    } else if (e.kind == K::Ident) {
        if (e.name == u"file")
            return PropertyRef{QStringLiteral("file"), QStringLiteral("file")};
        return PropertyRef{QStringLiteral("note"), e.name};
    } else if (e.kind == K::Call && e.name == u"list" && !e.subject && e.children.size() == 1) {
        return lhsProperty(*e.children[0]);
    }
    return std::nullopt;
}

class EmptyScope : public Scope {
public:
    explicit EmptyScope(const DataSource* ds) : m_ds(ds) {}
    Value getByIdentifier(const QString&) override { return Value(); }
    QStringList keys() override { return {}; }
    const DataSource* dataSource() const override { return m_ds; }

private:
    const DataSource* m_ds;
};

// Obsidian's EQ(): a value as frontmatter data.
QVariant toFrontmatter(const Value& v, const DataSource* ds, const QString& sourcePath)
{
    if (v.is(ValueClass::File))
        return QString(QStringLiteral("[[") + (ds ? ds->linkText(v.filePath(), sourcePath) : v.filePath()) + QStringLiteral("]]"));
    if (v.type() == ValueType::Link) {
        const QString target = v.resolve();
        const QString text = target.isEmpty() || !ds ? v.text() : ds->linkText(target, sourcePath);
        return QString(QStringLiteral("[[") + text + (v.display() ? u'|' + v.display()->toString() : QString()) + QStringLiteral("]]"));
    }
    if (v.type() == ValueType::Boolean)
        return v.boolValue();
    if (v.type() == ValueType::Number)
        return v.number();
    if (v.is(ValueClass::Primitive))
        return v.text();
    if (v.isNull())
        return QVariant();
    if (v.is(ValueClass::List)) {
        QVariantList l;
        for (const Value& i : v.items())
            l << toFrontmatter(i, ds, sourcePath);
        return l;
    }
    if (v.type() == ValueType::Object) {
        QVariantMap m;
        const ObjectData* o = v.objectData();
        for (int i = 0; i < o->keys.size(); ++i)
            m.insert(o->keys[i], toFrontmatter(o->at(i), ds, sourcePath));
        return m;
    }
    return v.toString();
}

void setDefault(QList<QPair<QString, QVariant>>& fm, const QString& key, const QVariant& value)
{
    for (auto& e : fm)
        if (e.first == key) {
            e.second = value;
            return;
        }
    fm.append({key, value});
}

bool hasDefault(const QList<QPair<QString, QVariant>>& fm, const QString& key)
{
    for (const auto& e : fm)
        if (e.first == key)
            return true;
    return false;
}

} // namespace

NewItemDefaults newItemDefaults(const BaseFile& base, const BaseView* view, const DataSource* ds, const EntryPtr& entry,
                                const std::function<QString(const QString&)>& inferredType)
{
    NewItemDefaults out;
    const std::optional<Filter> filter = Filter::both(base.filters, view ? view->filters : std::nullopt);
    const QString sourcePath = entry ? entry->path() : QString();
    auto evalArgs = [&](const QList<ExprPtr>& args) -> QVariant {
        if (args.isEmpty())
            throw EvalError{QStringLiteral("Formula cannot be deserialized")};
        QVariantList values;
        for (const ExprPtr& a : args) {
            Value v;
            bool done = false;
            if (entry) {
                try {
                    v = evaluate(*a, *entry);
                    done = true;
                } catch (const EvalError&) {
                }
            }
            if (!done) {
                EmptyScope s(ds);
                try {
                    v = evaluate(*a, s);
                } catch (const EvalError&) {
                    v = Value();
                }
            }
            values << toFrontmatter(v, ds, sourcePath);
        }
        return values.size() > 1 ? QVariant(values) : values.first();
    };
    using K = ExprNode::Kind;
    if (filter) {
        for (const Filter::Rule& r : filter->rules()) {
            if (r.negated || !r.rule->formula)
                continue;
            const ExprNode& p = *r.rule->formula->root();
            try {
                if (p.kind == K::Not) {
                    const ExprNode& d = *p.children[0];
                    if (d.kind == K::Call && d.name == u"isEmpty" && d.subject) {
                        if (auto ref = lhsProperty(*d.subject); ref && ref->type == u"note" && !hasDefault(out.frontmatter, ref->name))
                            setDefault(out.frontmatter, ref->name, QString());
                    }
                } else if (p.kind == K::Compare) {
                    if (p.name != u">=" && p.name != u"<=" && p.name != u"==")
                        continue;
                    const std::optional<PropertyRef> ref = lhsProperty(*p.children[0]);
                    if (!ref)
                        continue;
                    const QVariant y = evalArgs({p.children[1]});
                    if (ref->type == u"note")
                        setDefault(out.frontmatter, ref->name, y);
                    else if (ref->type == u"file" && ref->name == u"folder" && p.name == u"==" && y.typeId() == QMetaType::QString)
                        out.folder = y.toString();
                } else if (p.kind == K::Call && p.subject) {
                    const std::optional<PropertyRef> ref = lhsProperty(*p.subject);
                    if (!ref)
                        continue;
                    QVariant y = evalArgs(p.children);
                    const bool isList = y.typeId() == QMetaType::QVariantList;
                    if (ref->type == u"note") {
                        const QString type = inferredType ? inferredType(ref->name) : QString();
                        if (p.name == u"startsWith" || p.name == u"endsWith" || p.name == u"contains" || p.name == u"containsAll") {
                            if (type == u"multitext" && !isList)
                                y = QVariantList{y};
                            setDefault(out.frontmatter, ref->name, y);
                        } else if (p.name == u"containsAny") {
                            const QVariant w = isList ? y.toList().value(0) : y;
                            setDefault(out.frontmatter, ref->name, type == u"multitext" ? QVariant(QVariantList{w}) : w);
                        }
                    } else if (ref->type == u"file") {
                        if (p.name == u"hasTag" && y.typeId() == QMetaType::QString) {
                            QVariantList tags;
                            for (const auto& e : std::as_const(out.frontmatter))
                                if (e.first == u"tags" && e.second.typeId() == QMetaType::QVariantList)
                                    tags = e.second.toList();
                            tags << y;
                            setDefault(out.frontmatter, QStringLiteral("tags"), tags);
                        } else if (p.name == u"inFolder" && y.typeId() == QMetaType::QString) {
                            out.folder = y.toString();
                        } else if (p.name == u"hasProperty" && y.typeId() == QMetaType::QString) {
                            setDefault(out.frontmatter, y.toString(), QVariant());
                        } else if (ref->name == u"tags") {
                            if (p.name == u"containsAny")
                                setDefault(out.frontmatter, QStringLiteral("tags"), QVariantList{isList ? y.toList().value(0) : y});
                            else if (p.name == u"contains" || p.name == u"containsAll")
                                setDefault(out.frontmatter, QStringLiteral("tags"), isList ? y : QVariant(QVariantList{y}));
                        }
                    }
                }
            } catch (const EvalError&) {
            }
        }
    }
    if (view) {
        for (const QString& id : view->columns()) {
            const PropertyRef ref = parsePropertyId(id);
            if (ref.type == u"note" && !hasDefault(out.frontmatter, ref.name))
                setDefault(out.frontmatter, ref.name, QVariant());
        }
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Export
// ---------------------------------------------------------------------------------------------
TableExport exportTable(const BaseFile& base, const ViewData& data)
{
    TableExport t;
    for (const QString& id : data.columns)
        t.headers << base.displayName(id);
    for (const EntryPtr& e : data.rows) {
        QStringList row;
        for (const QString& id : data.columns) {
            try {
                const Value v = e->value(id);
                row << (v.isNull() ? QString() : v.toString());
            } catch (const EvalError&) {
                row << QString();
            }
        }
        t.rows << row;
    }
    return t;
}

namespace {
QString toText(const TableExport& t, QChar sep)
{
    const QRegularExpression special(QStringLiteral("[\"\\n\\r") + QRegularExpression::escape(QString(sep)) + u']');
    auto cell = [&](const QString& c) {
        if (!special.match(c).hasMatch())
            return c;
        QString q = c;
        q.replace(u'"', QLatin1String("\"\""));
        return QString(u'"' + q + u'"');
    };
    QStringList lines;
    QStringList header;
    for (const QString& h : t.headers)
        header << cell(h);
    lines << header.join(sep);
    for (const QStringList& r : t.rows) {
        QStringList cells;
        for (const QString& c : r)
            cells << cell(c);
        lines << cells.join(sep);
    }
    return lines.join(u'\n');
}
} // namespace

QString TableExport::toTsv() const { return toText(*this, u'\t'); }
QString TableExport::toCsv() const { return toText(*this, u','); }

QString TableExport::toMarkdown() const
{
    auto esc = [](QString s) { return s.replace(u'|', QLatin1String("\\|")); };
    QStringList head;
    for (const QString& h : headers)
        head << esc(h);
    QList<QStringList> body;
    for (const QStringList& r : rows) {
        QStringList row;
        for (const QString& c : r)
            row << esc(c);
        body << row;
    }
    QList<qsizetype> widths;
    for (const QString& h : std::as_const(head))
        widths << h.size();
    for (const QStringList& r : std::as_const(body))
        for (qsizetype i = 0; i < r.size() && i < widths.size(); ++i)
            widths[i] = std::max(widths[i], r[i].size());
    for (qsizetype& w : widths)
        w += 2;
    QStringList lines;
    auto emitRow = [&](const QStringList& r) {
        QString line = QStringLiteral("|");
        for (qsizetype i = 0; i < r.size() && i < widths.size(); ++i) {
            const qsizetype f = widths[i] - r[i].size();
            const qsizetype v = f / 2;
            line += QString(v, u' ') + r[i] + QString(f - v, u' ') + u'|';
        }
        lines << line;
    };
    emitRow(head);
    QString sep = QStringLiteral("|");
    for (qsizetype w : std::as_const(widths))
        sep += u' ' + QString(w - 2, u'-') + QStringLiteral(" |");
    lines << sep;
    for (const QStringList& r : std::as_const(body))
        emitRow(r);
    return lines.join(u'\n');
}

} // namespace oc::bases
