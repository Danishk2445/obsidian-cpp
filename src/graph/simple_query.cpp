#include "graph/simple_query.h"

#include <memory>
#include <vector>

namespace oc::graph {

namespace {

struct Term {
    enum class Op { Any, Path, File, Tag } op = Op::Any;
    QString value; // lower-cased
    bool negate = false;
};
using Group = std::vector<Term>; // AND

bool tagMatches(const QString& tagId, const QString& want)
{
    QString t = tagId.toLower();
    if (t.startsWith(QLatin1Char('#')))
        t.remove(0, 1);
    return t == want || t.startsWith(want + QLatin1Char('/'));
}

bool termMatches(const Term& term, const GraphData& g, int i)
{
    const GraphNode& n = g.node(i);
    bool hit = false;
    switch (term.op) {
    case Term::Op::Any:
        hit = n.id.contains(term.value, Qt::CaseInsensitive) || n.label.contains(term.value, Qt::CaseInsensitive);
        break;
    case Term::Op::Path:
        hit = n.id.contains(term.value, Qt::CaseInsensitive);
        break;
    case Term::Op::File: {
        const qsizetype slash = n.id.lastIndexOf(QLatin1Char('/'));
        hit = QStringView(n.id).mid(slash + 1).contains(term.value, Qt::CaseInsensitive);
        break;
    }
    case Term::Op::Tag:
        if (n.kind == NodeKind::Tag) {
            hit = tagMatches(n.id, term.value);
        } else if (g.isFinalized()) {
            for (int t : g.outgoing(i)) {
                if (g.node(t).kind == NodeKind::Tag && tagMatches(g.node(t).id, term.value)) {
                    hit = true;
                    break;
                }
            }
        }
        break;
    }
    return hit != term.negate;
}

std::vector<Group> parse(const QString& query)
{
    std::vector<Group> groups(1);
    const qsizetype n = query.size();
    qsizetype i = 0;
    while (i < n) {
        while (i < n && query[i].isSpace())
            ++i;
        if (i >= n)
            break;
        Term term;
        if (query[i] == QLatin1Char('-') && i + 1 < n && !query[i + 1].isSpace()) {
            term.negate = true;
            ++i;
        }
        // operator prefix
        static const struct {
            const char* name;
            Term::Op op;
        } ops[] = {{"path:", Term::Op::Path}, {"file:", Term::Op::File}, {"tag:", Term::Op::Tag}};
        for (const auto& o : ops) {
            const QLatin1String name(o.name);
            if (QStringView(query).mid(i).startsWith(name, Qt::CaseInsensitive)) {
                term.op = o.op;
                i += name.size();
                break;
            }
        }
        QString value;
        if (i < n && query[i] == QLatin1Char('"')) {
            const qsizetype end = query.indexOf(QLatin1Char('"'), i + 1);
            value = query.mid(i + 1, end < 0 ? -1 : end - i - 1);
            i = end < 0 ? n : end + 1;
        } else {
            const qsizetype start = i;
            while (i < n && !query[i].isSpace())
                ++i;
            value = query.mid(start, i - start);
        }
        if (!term.negate && term.op == Term::Op::Any && value == u"OR") {
            if (!groups.back().empty())
                groups.emplace_back();
            continue;
        }
        if (term.op == Term::Op::Tag && value.startsWith(QLatin1Char('#')))
            value.remove(0, 1);
        term.value = value.toLower();
        if (term.value.isEmpty())
            continue;
        groups.back().push_back(std::move(term));
    }
    std::erase_if(groups, [](const Group& g) { return g.empty(); });
    return groups;
}

} // namespace

NodePredicate compileSimpleQuery(const QString& query)
{
    auto groups = std::make_shared<const std::vector<Group>>(parse(query));
    if (groups->empty())
        return {};
    return [groups](const GraphData& g, int i) {
        for (const Group& group : *groups) {
            bool all = true;
            for (const Term& t : group) {
                if (!termMatches(t, g, i)) {
                    all = false;
                    break;
                }
            }
            if (all)
                return true;
        }
        return false;
    };
}

} // namespace oc::graph
