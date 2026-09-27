#include "core/bases/base_file.h"

#include <QHash>

namespace oc::bases {

namespace {

struct ModelError {
    QString message;
};

QString viewIncorrectType(const QString& key, const QString& type, const QString& view)
{
    return QStringLiteral("\"%1\" must be a %2 in view \"%3\"").arg(key, type, view);
}

// A scalar key/value as a string (JavaScript would coerce object keys; values are checked).
QString scalarString(const YamlNode& n) { return n.isString() ? n.toString() : scalarText(n); }

} // namespace

// ---------------------------------------------------------------------------------------------
// Property ids
// ---------------------------------------------------------------------------------------------
PropertyRef parsePropertyId(const QString& id)
{
    const qsizetype dot = id.indexOf(u'.');
    if (dot < 0)
        return {QStringLiteral("note"), id};
    const QString prefix = id.left(dot);
    if (prefix != u"note" && prefix != u"formula" && prefix != u"file")
        return {QStringLiteral("note"), id};
    return {prefix, id.mid(dot + 1)};
}

QString propertyIdFromWritten(const QString& e)
{
    if (e.startsWith(QLatin1String("note.")) || e.startsWith(QLatin1String("formula.")) || e.startsWith(QLatin1String("file.")))
        return e;
    if (e == u"file")
        return QStringLiteral("file.file");
    return QStringLiteral("note.") + e;
}

QString writtenPropertyId(const QString& e)
{
    if (e.size() > 5 && e.startsWith(QLatin1String("note."))) {
        const QString t = e.mid(5);
        const QString n = t.section(u'.', 0, 0);
        return n == u"file" || n == u"formula" || n == u"note" ? e : t;
    }
    return e == u"file.file" ? QStringLiteral("file") : e;
}

QString makePropertyId(const QString& type, const QString& name) { return type + u'.' + name; }

const QStringList& fileProperties()
{
    static const QStringList ids = {QStringLiteral("file.file"),  QStringLiteral("file.name"),  QStringLiteral("file.basename"),
                                    QStringLiteral("file.fullname"), QStringLiteral("file.path"), QStringLiteral("file.folder"),
                                    QStringLiteral("file.ext"),   QStringLiteral("file.ctime"), QStringLiteral("file.mtime"),
                                    QStringLiteral("file.size"),  QStringLiteral("file.links"), QStringLiteral("file.backlinks"),
                                    QStringLiteral("file.embeds"), QStringLiteral("file.tags")};
    return ids;
}

QString filePropertyLabel(const QString& id)
{
    static const QHash<QString, QString> labels = {
        {QStringLiteral("file.file"), QStringLiteral("file")},
        {QStringLiteral("file.name"), QStringLiteral("file name")},
        {QStringLiteral("file.basename"), QStringLiteral("file base name")},
        {QStringLiteral("file.fullname"), QStringLiteral("file full name")},
        {QStringLiteral("file.path"), QStringLiteral("file path")},
        {QStringLiteral("file.folder"), QStringLiteral("folder")},
        {QStringLiteral("file.ext"), QStringLiteral("file extension")},
        {QStringLiteral("file.ctime"), QStringLiteral("created time")},
        {QStringLiteral("file.mtime"), QStringLiteral("modified time")},
        {QStringLiteral("file.size"), QStringLiteral("file size")},
        {QStringLiteral("file.links"), QStringLiteral("file links")},
        {QStringLiteral("file.backlinks"), QStringLiteral("file backlinks")},
        {QStringLiteral("file.embeds"), QStringLiteral("file embeds")},
        {QStringLiteral("file.tags"), QStringLiteral("file tags")},
    };
    return labels.value(id);
}

// ---------------------------------------------------------------------------------------------
// Filter
// ---------------------------------------------------------------------------------------------
Filter Filter::expr(const QString& text)
{
    Filter f;
    f.kind = Kind::Expr;
    f.text = text;
    f.formula = std::make_shared<Formula>(text);
    return f;
}

Filter Filter::group(Kind kind, const QList<Filter>& children)
{
    Filter f;
    f.kind = kind;
    f.children = children;
    return f;
}

std::optional<Filter> Filter::parse(const YamlNode& node, QString* error)
{
    auto fail = [&](const QString& m) -> std::optional<Filter> {
        if (error)
            *error = m;
        return std::nullopt;
    };
    if (node.isString())
        return expr(node.toString());
    if (!node.isMap() && !node.isList())
        return fail(QStringLiteral("\"filters\" must be a string or an object"));
    if (!node.isMap() || node.entries().size() != 1)
        return fail(QStringLiteral("\"filters\" may only have one of an \"and\", \"or\", or \"not\" keys."));
    const YamlNode::Entry& e = node.entries().first();
    Kind kind;
    if (e.first == u"and")
        kind = Kind::And;
    else if (e.first == u"or")
        kind = Kind::Or;
    else if (e.first == u"not")
        kind = Kind::Not;
    else
        return fail(QStringLiteral("\"filters\" may only have one of an \"and\", \"or\", or \"not\" keys."));
    if (!e.second.isList())
        return fail(QStringLiteral("\"%1\" filter must be an array").arg(e.first));
    Filter g = group(kind);
    for (const YamlNode& item : e.second.items()) {
        std::optional<Filter> child = parse(item, error);
        if (!child)
            return std::nullopt;
        g.children << *child;
    }
    return g;
}

YamlNode Filter::serialize() const
{
    if (kind == Kind::Expr)
        return YamlNode::string(text);
    QList<YamlNode> items;
    for (const Filter& c : children)
        items << c.serialize();
    const QString key = kind == Kind::And ? QStringLiteral("and") : kind == Kind::Or ? QStringLiteral("or") : QStringLiteral("not");
    return YamlNode::map({{key, YamlNode::list(items)}});
}

bool Filter::test(Scope& scope) const
{
    switch (kind) {
    case Kind::Expr:
        return formula && formula->test(scope);
    case Kind::And:
        for (const Filter& c : children)
            if (!c.test(scope))
                return false;
        return true;
    case Kind::Or:
        for (const Filter& c : children)
            if (c.test(scope))
                return true;
        return false;
    case Kind::Not:
        for (const Filter& c : children)
            if (c.test(scope))
                return false;
        return true;
    }
    return false;
}

bool Filter::hasError() const
{
    if (kind == Kind::Expr)
        return formula && formula->isInvalid();
    for (const Filter& c : children)
        if (c.hasError())
            return true;
    return false;
}

std::optional<Filter> Filter::optimized() const
{
    if (kind == Kind::Expr)
        return *this;
    QList<Filter> kept;
    for (const Filter& c : children) {
        std::optional<Filter> o = c.optimized();
        if (!o)
            continue;
        if (kind != Kind::Not && o->kind == kind)
            kept += o->children;
        else
            kept << *o;
    }
    if (kept.isEmpty())
        return std::nullopt;
    if (kind == Kind::Not) {
        if (kept.size() == 1 && kept[0].kind == Kind::Not)
            return group(Kind::And, kept[0].children).optimized();
        return group(Kind::Not, kept);
    }
    if (kept.size() == 1)
        return kept[0];
    return group(kind, kept);
}

std::optional<Filter> Filter::both(const std::optional<Filter>& a, const std::optional<Filter>& b)
{
    if (a && !b)
        return a;
    if (b && !a)
        return b;
    if (!a && !b)
        return std::nullopt;
    return group(Kind::And, {*a, *b}).optimized();
}

bool Filter::operator==(const Filter& o) const
{
    return kind == o.kind && text == o.text && children == o.children;
}

QList<Filter::Rule> Filter::rules() const
{
    QList<Rule> out;
    std::function<void(const Filter&, bool)> walk = [&](const Filter& f, bool negated) {
        if (f.kind == Kind::Expr) {
            out << Rule{&f, negated};
            return;
        }
        const bool n = f.kind == Kind::Not;
        for (const Filter& c : f.children)
            walk(c, negated != n);
    };
    walk(*this, false);
    return out;
}

// ---------------------------------------------------------------------------------------------
// BaseView
// ---------------------------------------------------------------------------------------------
QStringList BaseView::columns() const { return order.value_or(QStringList{QStringLiteral("file.name")}); }

YamlNode BaseView::option(const QString& key) const
{
    for (const YamlNode::Entry& e : data)
        if (e.first == key)
            return e.second;
    return {};
}

void BaseView::setOption(const QString& key, const YamlNode& value)
{
    for (qsizetype i = 0; i < data.size(); ++i) {
        if (data[i].first != key)
            continue;
        if (value.isNull())
            data.removeAt(i);
        else
            data[i].second = value;
        return;
    }
    if (!value.isNull())
        data.append({key, value});
}

QString BaseView::summaryOf(const QString& propertyId) const
{
    if (!summaries)
        return {};
    for (const auto& [id, name] : *summaries)
        if (id == propertyId)
            return name;
    return {};
}

void BaseView::setSummary(const QString& propertyId, const QString& name)
{
    if (name.isEmpty()) {
        if (!summaries)
            return;
        for (qsizetype i = 0; i < summaries->size(); ++i)
            if ((*summaries)[i].first == propertyId) {
                summaries->removeAt(i);
                return;
            }
        return;
    }
    if (!summaries)
        summaries = QList<QPair<QString, QString>>();
    for (auto& e : *summaries)
        if (e.first == propertyId) {
            e.second = name;
            return;
        }
    summaries->append({propertyId, name});
}

void BaseView::setSortProperty(const QString& propertyId, const QString& dir)
{
    QString t = dir;
    QList<SortSpec> n = sortSpecs();
    qsizetype i = -1;
    for (qsizetype k = 0; k < n.size(); ++k)
        if (n[k].property == propertyId) {
            i = k;
            break;
        }
    if (i >= 0) {
        if (t == u"TOGGLE")
            t = n[i].direction == u"ASC" ? QStringLiteral("DESC") : QStringLiteral("NONE");
        n.removeAt(i);
    }
    if (t == u"TOGGLE")
        t = QStringLiteral("ASC");
    if (t == u"ASC" || t == u"DESC")
        n.prepend(SortSpec{propertyId, t});
    sort = n;
}

YamlNode BaseView::serialize() const
{
    YamlNode e = YamlNode::map();
    e.set(QStringLiteral("type"), YamlNode::string(type));
    e.set(QStringLiteral("name"), YamlNode::string(name));
    if (filters)
        e.set(QStringLiteral("filters"), filters->serialize());
    if (groupBy)
        e.set(QStringLiteral("groupBy"), YamlNode::map({{QStringLiteral("property"), YamlNode::string(writtenPropertyId(groupBy->property))},
                                                        {QStringLiteral("direction"), YamlNode::string(groupBy->direction)}}));
    if (order) {
        QList<YamlNode> items;
        for (const QString& id : *order)
            items << YamlNode::string(writtenPropertyId(id));
        e.set(QStringLiteral("order"), YamlNode::list(items));
    }
    if (sort) {
        QList<YamlNode> items;
        for (const SortSpec& s : *sort)
            items << YamlNode::map({{QStringLiteral("property"), YamlNode::string(writtenPropertyId(s.property))},
                                    {QStringLiteral("direction"), YamlNode::string(s.direction)}});
        e.set(QStringLiteral("sort"), YamlNode::list(items));
    }
    if (limit != 0)
        e.set(QStringLiteral("limit"), YamlNode::number(limit));
    if (summaries) {
        QList<YamlNode::Entry> entries;
        for (const auto& [id, n] : *summaries)
            entries.append({writtenPropertyId(id), YamlNode::string(n)});
        e.set(QStringLiteral("summaries"), YamlNode::map(entries));
    }
    static const QStringList own = {QStringLiteral("query"), QStringLiteral("type"), QStringLiteral("name"), QStringLiteral("data")};
    for (const YamlNode::Entry& d : data) {
        if (own.contains(d.first) || e.contains(d.first))
            continue;
        e.set(d.first, d.second);
    }
    return e;
}

// ---------------------------------------------------------------------------------------------
// PropertyConfig
// ---------------------------------------------------------------------------------------------
YamlNode PropertyConfig::serialize() const
{
    YamlNode e = YamlNode::map(unrecognized);
    if (!displayName.isEmpty())
        e.set(QStringLiteral("displayName"), YamlNode::string(displayName));
    return e.entries().isEmpty() ? YamlNode() : e;
}

// ---------------------------------------------------------------------------------------------
// BaseFile
// ---------------------------------------------------------------------------------------------
namespace {

BaseView parseView(const YamlNode& t, int index)
{
    if (!t.isMap())
        throw ModelError{QStringLiteral("\"view\" items must be a YAML object, however item %1 is not").arg(index)};
    const YamlNode nameNode = t.value(QStringLiteral("name"));
    if (!nameNode.isString() || nameNode.toString().isEmpty())
        throw ModelError{QStringLiteral("Missing or invalid \"name\" in view %1").arg(index)};
    const QString name = nameNode.toString();
    const YamlNode* typeNode = t.find(QStringLiteral("type"));
    if (!typeNode || typeNode->isNull() || (typeNode->isString() && typeNode->toString().isEmpty()))
        throw ModelError{QStringLiteral("\"type\" is required in view \"%1\"").arg(name)};
    if (!typeNode->isString())
        throw ModelError{viewIncorrectType(QStringLiteral("type"), QStringLiteral("string"), name)};
    BaseView v;
    v.type = typeNode->toString();
    v.name = name;
    for (const YamlNode::Entry& e : t.entries()) {
        const QString& key = e.first;
        const YamlNode& val = e.second;
        if (key == u"type" || key == u"name")
            continue;
        if (key == u"filters") {
            QString err;
            std::optional<Filter> f = Filter::parse(val, &err);
            if (!f)
                throw ModelError{QStringLiteral("Unable to parse filters in view \"%1\": %2").arg(name, err)};
            v.filters = f;
        } else if (key == u"groupBy") {
            if (!val.isMap() || !val.contains(QStringLiteral("property")) || !val.contains(QStringLiteral("direction")))
                throw ModelError{viewIncorrectType(QStringLiteral("groupBy"), QStringLiteral("object"), name)};
            const YamlNode p = val.value(QStringLiteral("property"));
            const YamlNode d = val.value(QStringLiteral("direction"));
            if (p.isString() && !p.toString().isEmpty() && d.isString() && (d.toString() == u"ASC" || d.toString() == u"DESC"))
                v.groupBy = SortSpec{propertyIdFromWritten(p.toString()), d.toString()};
        } else if (key == u"summaries") {
            if (!val.isMap() && !val.isList())
                throw ModelError{viewIncorrectType(QStringLiteral("summaries"), QStringLiteral("object"), name)};
            QList<QPair<QString, QString>> s;
            for (const YamlNode::Entry& se : val.entries()) {
                if (!se.second.isString())
                    throw ModelError{QStringLiteral("\"summaries\" values must all be of type string in view \"%1\"").arg(name)};
                s.append({propertyIdFromWritten(se.first), se.second.toString()});
            }
            v.summaries = s;
        } else if (key == u"order") {
            if (!val.isList())
                throw ModelError{viewIncorrectType(QStringLiteral("order"), QStringLiteral("array"), name)};
            QStringList order;
            for (const YamlNode& item : val.items())
                order << propertyIdFromWritten(scalarString(item));
            v.order = order;
        } else if (key == u"sort") {
            if (!val.isList())
                continue;
            QList<SortSpec> sort;
            for (const YamlNode& item : val.items()) {
                if (!item.isMap())
                    continue;
                YamlNode p = item.value(QStringLiteral("property"));
                if (item.contains(QStringLiteral("column")) && !item.value(QStringLiteral("column")).isNull())
                    p = item.value(QStringLiteral("column"));
                const YamlNode d = item.value(QStringLiteral("direction"));
                if (p.isString() && !p.toString().isEmpty() && d.isString() && (d.toString() == u"ASC" || d.toString() == u"DESC"))
                    sort << SortSpec{propertyIdFromWritten(p.toString()), d.toString()};
            }
            v.sort = sort;
        } else if (key == u"limit") {
            if (val.isNumber() && val.toNumber() > 0)
                v.limit = val.toNumber();
        } else {
            v.data.append({key, val});
        }
    }
    return v;
}

} // namespace

std::optional<BaseFile> BaseFile::fromString(const QString& text, QString* error)
{
    if (text.trimmed().isEmpty()) {
        BaseFile b;
        BaseView v;
        v.type = QStringLiteral("table");
        v.name = QStringLiteral("Table");
        b.views << v;
        return b;
    }
    QString yamlError;
    const std::optional<YamlNode> root = parseYaml(text, &yamlError);
    if (!root) {
        if (error)
            *error = yamlError;
        return std::nullopt;
    }
    return fromYaml(*root, error);
}

std::optional<BaseFile> BaseFile::fromYaml(const YamlNode& root, QString* error)
{
    try {
        if (!root.isMap() && !root.isNull())
            throw ModelError{QStringLiteral("Query is an invalid format. It should be a YAML object.")};
        BaseFile h;
        const YamlNode* views = root.find(QStringLiteral("views"));
        if (views && !views->isList())
            throw ModelError{QStringLiteral("Views must be an array")};
        if (views) {
            int index = 1;
            for (const YamlNode& v : views->items())
                h.views << parseView(v, index++);
        }
        if (h.views.isEmpty()) {
            BaseView v;
            v.type = QStringLiteral("table");
            v.name = QStringLiteral("Table");
            h.views << v;
        }
        if (const YamlNode* f = root.find(QStringLiteral("filters")); f && !f->isNull() && !(f->isString() && f->toString().isEmpty())
                                                                        && !(f->isBool() && !f->toBool())) {
            QString err;
            h.filters = Filter::parse(*f, &err);
            if (!h.filters)
                throw ModelError{err};
        }
        if (const YamlNode* p = root.find(QStringLiteral("properties")); p && !p->isNull()) {
            if (!p->isMap() && !p->isList())
                throw ModelError{QStringLiteral("Invalid properties configuration.")};
            for (const YamlNode::Entry& e : p->entries()) {
                if (e.second.isNull())
                    continue;
                if (!e.second.isMap() && !e.second.isList())
                    throw ModelError{QStringLiteral("Invalid properties configuration.")};
                PropertyConfig c;
                for (const YamlNode::Entry& ce : e.second.entries()) {
                    if (ce.first == u"displayName") {
                        if (ce.second.isString())
                            c.displayName = ce.second.toString();
                    } else {
                        c.unrecognized.append(ce);
                    }
                }
                h.properties.append({e.first, c});
            }
        }
        if (const YamlNode* d = root.find(QStringLiteral("display")); d && !d->isNull()) {
            if (!d->isMap() && !d->isList())
                throw ModelError{QStringLiteral("\"display\" must be a object")};
            for (const YamlNode::Entry& e : d->entries()) {
                if (!e.second.isString())
                    throw ModelError{QStringLiteral("Display values must be strings. Value for property %1 is not.").arg(e.first)};
                PropertyConfig* c = h.propertyConfig(propertyIdFromWritten(e.first));
                if (c->displayName.isEmpty())
                    c->displayName = e.second.toString();
            }
        }
        if (const YamlNode* f = root.find(QStringLiteral("formulas")); f && !f->isNull()) {
            if (!f->isMap() && !f->isList())
                throw ModelError{QStringLiteral("\"formulas\" must be a object")};
            for (const YamlNode::Entry& e : f->entries()) {
                if (!e.second.isString())
                    throw ModelError{QStringLiteral("Formula values must be strings. Value for property %1 is not.").arg(e.first)};
                h.formulas.append({e.first, std::make_shared<Formula>(e.second.toString())});
            }
        }
        if (const YamlNode* s = root.find(QStringLiteral("summaries")); s && !s->isNull()) {
            if (!s->isMap() && !s->isList())
                throw ModelError{QStringLiteral("\"summaries\" must be a object")};
            QList<QPair<QString, FormulaPtr>> list;
            for (const YamlNode::Entry& e : s->entries()) {
                if (!e.second.isString())
                    throw ModelError{QStringLiteral("Formula values must be strings. Value for property %1 is not.").arg(e.first)};
                list.append({e.first, std::make_shared<Formula>(e.second.toString())});
            }
            h.summaryFormulas = list;
        }
        if (const YamlNode* n = root.find(QStringLiteral("newItemFolder")); n && !n->isNull() && !(n->isString() && n->toString().isEmpty())) {
            if (!n->isString())
                throw ModelError{QStringLiteral("\"newItemFolder\" must be a string")};
            h.newItemFolder = n->toString();
        }
        if (const YamlNode* n = root.find(QStringLiteral("newItemTemplate")); n && !n->isNull() && !(n->isString() && n->toString().isEmpty())) {
            if (!n->isString())
                throw ModelError{QStringLiteral("\"newItemTemplate\" must be a string")};
            h.newItemTemplate = n->toString();
        }
        static const QStringList known = {QStringLiteral("views"),      QStringLiteral("filters"),       QStringLiteral("display"),
                                          QStringLiteral("properties"), QStringLiteral("formulas"),      QStringLiteral("newItemFolder"),
                                          QStringLiteral("newItemTemplate")};
        for (const YamlNode::Entry& e : root.entries())
            if (!known.contains(e.first))
                h.unrecognized.append(e);
        return h;
    } catch (const ModelError& e) {
        if (error)
            *error = e.message;
        return std::nullopt;
    }
}

YamlNode BaseFile::serialize() const
{
    YamlNode e = YamlNode::map(unrecognized);
    if (filters)
        e.set(QStringLiteral("filters"), filters->serialize());
    if (!formulas.isEmpty()) {
        QList<YamlNode::Entry> f;
        for (const auto& [name, formula] : formulas)
            f.append({name, YamlNode::string(formula->text())});
        e.set(QStringLiteral("formulas"), YamlNode::map(f));
    }
    if (summaryFormulas) {
        QList<YamlNode::Entry> s;
        for (const auto& [name, formula] : *summaryFormulas)
            s.append({name, YamlNode::string(formula->text())});
        e.set(QStringLiteral("summaries"), YamlNode::map(s));
    }
    QList<YamlNode::Entry> props;
    for (const auto& [key, config] : properties) {
        const YamlNode n = config.serialize();
        if (!n.isNull())
            props.append({key, n});
    }
    if (!props.isEmpty())
        e.set(QStringLiteral("properties"), YamlNode::map(props));
    QList<YamlNode> v;
    for (const BaseView& view : views)
        v << view.serialize();
    e.set(QStringLiteral("views"), YamlNode::list(v));
    if (!newItemFolder.isEmpty())
        e.set(QStringLiteral("newItemFolder"), YamlNode::string(newItemFolder));
    if (!newItemTemplate.isEmpty())
        e.set(QStringLiteral("newItemTemplate"), YamlNode::string(newItemTemplate));
    return e;
}

QString BaseFile::toString() const
{
    const YamlNode e = serialize();
    return e.entries().isEmpty() ? QString() : dumpYaml(e);
}

const BaseView* BaseFile::view(const QString& name) const
{
    if (name.isEmpty())
        return views.isEmpty() ? nullptr : &views.first();
    for (const BaseView& v : views)
        if (v.name == name)
            return &v;
    return nullptr;
}

BaseView* BaseFile::view(const QString& name) { return const_cast<BaseView*>(std::as_const(*this).view(name)); }

int BaseFile::viewIndex(const QString& name) const
{
    for (int i = 0; i < views.size(); ++i)
        if (views[i].name == name)
            return i;
    return name.isEmpty() && !views.isEmpty() ? 0 : -1;
}

QStringList BaseFile::viewNames() const
{
    QStringList out;
    for (const BaseView& v : views)
        out << v.name;
    return out;
}

QString BaseFile::uniqueViewName(const QString& base) const
{
    const QStringList names = viewNames();
    QString n = base;
    for (int i = 2; names.contains(n); ++i)
        n = base + u' ' + QString::number(i);
    return n;
}

FormulaPtr BaseFile::formula(const QString& name) const
{
    for (const auto& [n, f] : formulas)
        if (n == name)
            return f;
    for (const auto& [n, f] : formulas)
        if (n.compare(name, Qt::CaseInsensitive) == 0)
            return f;
    return nullptr;
}

QStringList BaseFile::formulaNames() const
{
    QStringList out;
    for (const auto& [n, f] : formulas)
        out << n;
    return out;
}

void BaseFile::setFormula(const QString& name, const QString& text)
{
    for (auto& [n, f] : formulas)
        if (n == name) {
            f = std::make_shared<Formula>(text);
            return;
        }
    formulas.append({name, std::make_shared<Formula>(text)});
}

void BaseFile::removeFormula(const QString& name)
{
    bool found = false;
    for (qsizetype i = 0; i < formulas.size(); ++i)
        if (formulas[i].first == name) {
            formulas.removeAt(i);
            found = true;
            break;
        }
    if (!found)
        return;
    const QString id = makePropertyId(QStringLiteral("formula"), name);
    for (qsizetype i = 0; i < properties.size(); ++i)
        if (propertyIdFromWritten(properties[i].first) == id) {
            properties.removeAt(i);
            break;
        }
    for (BaseView& v : views) {
        if (v.order)
            v.order->removeOne(id);
        if (v.sort) {
            QList<SortSpec> kept;
            for (const SortSpec& s : *v.sort)
                if (s.property != id)
                    kept << s;
            v.sort = kept;
        }
        v.setSummary(id, QString());
        if (v.groupBy && v.groupBy->property == id)
            v.groupBy.reset();
    }
}

PropertyConfig* BaseFile::propertyConfig(const QString& propertyId)
{
    for (auto& [key, c] : properties)
        if (key == propertyId)
            return &c;
    for (auto& [key, c] : properties)
        if (propertyIdFromWritten(key) == propertyId)
            return &c;
    properties.append({propertyId, PropertyConfig()});
    return &properties.last().second;
}

QString BaseFile::displayName(const QString& propertyId) const
{
    for (const auto& [key, c] : properties)
        if ((key == propertyId || propertyIdFromWritten(key) == propertyId) && !c.displayName.isEmpty())
            return c.displayName;
    const QString label = filePropertyLabel(propertyId);
    if (!label.isEmpty())
        return label;
    return parsePropertyId(propertyId).name;
}

void BaseFile::setDisplayName(const QString& propertyId, const QString& name) { propertyConfig(propertyId)->displayName = name; }

QStringList BaseFile::builtinSummaries(std::optional<ValueType> type)
{
    if (!type)
        return {};
    QStringList out = {QStringLiteral("Empty"), QStringLiteral("Filled"), QStringLiteral("Unique")};
    switch (*type) {
    case ValueType::Number:
        out << QStringLiteral("Average") << QStringLiteral("Min") << QStringLiteral("Max") << QStringLiteral("Sum") << QStringLiteral("Range")
            << QStringLiteral("Median") << QStringLiteral("Stddev");
        break;
    case ValueType::Date:
    case ValueType::RelativeDate:
        out << QStringLiteral("Earliest") << QStringLiteral("Latest") << QStringLiteral("Range");
        break;
    case ValueType::Boolean:
        out << QStringLiteral("Checked") << QStringLiteral("Unchecked");
        break;
    default:
        break;
    }
    return out;
}

QStringList BaseFile::customSummaryNames() const
{
    QStringList out;
    if (summaryFormulas)
        for (const auto& [n, f] : *summaryFormulas)
            out << n;
    return out;
}

FormulaPtr BaseFile::summaryFormula(const QString& name, std::optional<ValueType> type) const
{
    if (summaryFormulas)
        for (const auto& [n, f] : *summaryFormulas)
            if (n == name)
                return f;
    if (!type)
        return nullptr;
    static const QHash<QString, QString> anyFns = {
        {QStringLiteral("Empty"), QStringLiteral("values.filter(value.isType(\"null\")).length")},
        {QStringLiteral("Filled"), QStringLiteral("values.filter(!value.isType(\"null\")).length")},
        {QStringLiteral("Unique"), QStringLiteral("values.unique().length")},
    };
    static const QHash<QString, QString> numberFns = {
        {QStringLiteral("Average"), QStringLiteral("values.mean().round(2)")},
        {QStringLiteral("Min"), QStringLiteral("values.min()")},
        {QStringLiteral("Max"), QStringLiteral("values.max()")},
        {QStringLiteral("Sum"), QStringLiteral("values.sum().round(2)")},
        {QStringLiteral("Range"), QStringLiteral("values.max() - values.min()")},
        {QStringLiteral("Median"), QStringLiteral("values.median()")},
        {QStringLiteral("Stddev"), QStringLiteral("values.stddev().round(2)")},
    };
    static const QHash<QString, QString> dateFns = {
        {QStringLiteral("Earliest"), QStringLiteral("values.earliest()")},
        {QStringLiteral("Latest"), QStringLiteral("values.latest()")},
        {QStringLiteral("Range"), QStringLiteral("(values.latest() - values.earliest())")},
    };
    static const QHash<QString, QString> boolFns = {
        {QStringLiteral("Checked"), QStringLiteral("values.filter(value == true).length")},
        {QStringLiteral("Unchecked"), QStringLiteral("values.filter(value == false).length")},
    };
    QString text;
    switch (*type) {
    case ValueType::Number:
        text = numberFns.value(name);
        break;
    case ValueType::Date:
    case ValueType::RelativeDate:
        text = dateFns.value(name);
        break;
    case ValueType::Boolean:
        text = boolFns.value(name);
        break;
    default:
        break;
    }
    if (text.isEmpty())
        text = anyFns.value(name);
    if (text.isEmpty())
        return nullptr;
    static QHash<QString, FormulaPtr> cache;
    FormulaPtr& f = cache[text];
    if (!f)
        f = std::make_shared<Formula>(text);
    return f;
}

} // namespace oc::bases
