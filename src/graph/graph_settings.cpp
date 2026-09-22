#include "graph/graph_settings.h"

#include <QJsonArray>

namespace oc::graph {

namespace {
void readBool(const QJsonObject& o, const char* key, bool& out)
{
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isBool())
        out = v.toBool();
}
void readDouble(const QJsonObject& o, const char* key, double& out)
{
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isDouble())
        out = v.toDouble();
}
} // namespace

void GraphSettings::restoreDefaults()
{
    const GraphSettings d;
    showAttachments = d.showAttachments;
    hideUnresolved = d.hideUnresolved;
    showOrphans = d.showOrphans;
    showTags = d.showTags;
    localJumps = d.localJumps;
    localInterlinks = d.localInterlinks;
    localForelinks = d.localForelinks;
    localBacklinks = d.localBacklinks;
    colorGroups.clear();
    showArrow = d.showArrow;
    textFadeMultiplier = d.textFadeMultiplier;
    nodeSizeMultiplier = d.nodeSizeMultiplier;
    lineSizeMultiplier = d.lineSizeMultiplier;
    centerStrength = d.centerStrength;
    repelStrength = d.repelStrength;
    linkStrength = d.linkStrength;
    linkDistance = d.linkDistance;
}

GraphSettings GraphSettings::userDefaults()
{
    GraphSettings s;
    s.centerStrength = 0.518713248970312;
    s.repelStrength = 10;
    s.linkStrength = 1;
    s.linkDistance = 250;
    s.scale = 0.5443310539518174;
    s.showOrphans = true;
    return s;
}

QJsonObject GraphSettings::toJson(Variant variant) const
{
    QJsonObject o;
    o.insert(QStringLiteral("collapse-filter"), collapseFilter);
    o.insert(QStringLiteral("search"), search);
    if (variant == Variant::Local) {
        o.insert(QStringLiteral("localJumps"), localJumps);
        o.insert(QStringLiteral("localBacklinks"), localBacklinks);
        o.insert(QStringLiteral("localForelinks"), localForelinks);
        o.insert(QStringLiteral("localInterlinks"), localInterlinks);
    }
    o.insert(QStringLiteral("showTags"), showTags);
    o.insert(QStringLiteral("showAttachments"), showAttachments);
    o.insert(QStringLiteral("hideUnresolved"), hideUnresolved);
    if (variant == Variant::Global)
        o.insert(QStringLiteral("showOrphans"), showOrphans);
    o.insert(QStringLiteral("collapse-color-groups"), collapseColorGroups);
    QJsonArray groups;
    for (const ColorGroupSetting& g : colorGroups) {
        QJsonObject color;
        color.insert(QStringLiteral("a"), g.color.a);
        color.insert(QStringLiteral("rgb"), qint64(g.color.rgb));
        QJsonObject group;
        group.insert(QStringLiteral("query"), g.query);
        group.insert(QStringLiteral("color"), color);
        groups.append(group);
    }
    o.insert(QStringLiteral("colorGroups"), groups);
    o.insert(QStringLiteral("collapse-display"), collapseDisplay);
    o.insert(QStringLiteral("showArrow"), showArrow);
    o.insert(QStringLiteral("textFadeMultiplier"), textFadeMultiplier);
    o.insert(QStringLiteral("nodeSizeMultiplier"), nodeSizeMultiplier);
    o.insert(QStringLiteral("lineSizeMultiplier"), lineSizeMultiplier);
    o.insert(QStringLiteral("collapse-forces"), collapseForces);
    o.insert(QStringLiteral("centerStrength"), centerStrength);
    o.insert(QStringLiteral("repelStrength"), repelStrength);
    o.insert(QStringLiteral("linkStrength"), linkStrength);
    o.insert(QStringLiteral("linkDistance"), linkDistance);
    o.insert(QStringLiteral("scale"), scale);
    o.insert(QStringLiteral("close"), close);
    return o;
}

GraphSettings GraphSettings::fromJson(const QJsonObject& o)
{
    GraphSettings s;
    readBool(o, "collapse-filter", s.collapseFilter);
    if (const QJsonValue v = o.value(QLatin1String("search")); v.isString())
        s.search = v.toString();
    readBool(o, "showTags", s.showTags);
    readBool(o, "showAttachments", s.showAttachments);
    readBool(o, "hideUnresolved", s.hideUnresolved);
    readBool(o, "showOrphans", s.showOrphans);
    if (const QJsonValue v = o.value(QLatin1String("localJumps")); v.isDouble())
        s.localJumps = v.toInt(qRound(v.toDouble()));
    readBool(o, "localBacklinks", s.localBacklinks);
    readBool(o, "localForelinks", s.localForelinks);
    readBool(o, "localInterlinks", s.localInterlinks);
    readBool(o, "collapse-color-groups", s.collapseColorGroups);
    if (const QJsonValue v = o.value(QLatin1String("colorGroups")); v.isArray()) {
        for (const QJsonValue& gv : v.toArray()) {
            const QJsonObject go = gv.toObject();
            ColorGroupSetting g;
            g.query = go.value(QLatin1String("query")).toString();
            const QJsonObject co = go.value(QLatin1String("color")).toObject();
            g.color.a = co.value(QLatin1String("a")).toDouble(1);
            g.color.rgb = quint32(co.value(QLatin1String("rgb")).toInteger(0)) & 0xFFFFFFu;
            s.colorGroups.append(g);
        }
    }
    readBool(o, "collapse-display", s.collapseDisplay);
    readBool(o, "showArrow", s.showArrow);
    readDouble(o, "textFadeMultiplier", s.textFadeMultiplier);
    readDouble(o, "nodeSizeMultiplier", s.nodeSizeMultiplier);
    readDouble(o, "lineSizeMultiplier", s.lineSizeMultiplier);
    readBool(o, "collapse-forces", s.collapseForces);
    readDouble(o, "centerStrength", s.centerStrength);
    readDouble(o, "repelStrength", s.repelStrength);
    readDouble(o, "linkStrength", s.linkStrength);
    readDouble(o, "linkDistance", s.linkDistance);
    readDouble(o, "scale", s.scale);
    readBool(o, "close", s.close);
    return s;
}

} // namespace oc::graph
