#include "ui/views/bases/bases_render.h"

#include "core/properties/property_types.h"
#include "core/properties/property_yaml.h"
#include "render/services/image_cache.h"
#include "render/theme/icons.h"
#include "ui/views/bases/bases_style.h"
#include "ui/views/markdown/properties/properties_style.h"

#include <QDateTime>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>

#include <cmath>

namespace oc::basesui {

using namespace oc::bases;

namespace {

// One horizontal piece of a rendered value.
struct Run {
    enum class Kind { Text, Link, Tag, Checkbox, Icon, Image, Gap, Warning };
    Kind kind = Kind::Text;
    QString text;
    QFont font;
    QColor color;
    qreal width = 0;
    bool checked = false;
    QString icon;       // Icon / Warning
    QString imagePath;  // Image
    ValueHit hit;       // Link / Tag
    bool hasHit = false;
};

QString localeDateFormat()
{
    QString f = QLocale().dateFormat(QLocale::ShortFormat);
    if (!f.contains(QLatin1String("yyyy")))
        f.replace(QLatin1String("yy"), QLatin1String("yyyy"));
    static const QRegularExpression singleM(QStringLiteral("(?<!M)M(?!M)")), singleD(QStringLiteral("(?<!d)d(?!d)"));
    f.replace(singleM, QStringLiteral("MM"));
    f.replace(singleD, QStringLiteral("dd"));
    return f;
}

QString localeTimeFormat()
{
    QString f = QLocale().timeFormat(QLocale::ShortFormat);
    static const QRegularExpression singleH(QStringLiteral("(?<![hH])([hH])(?![hH])"));
    f.replace(singleH, QStringLiteral("\\1\\1"));
    return f;
}

// Obsidian's QE(): link text as shown ("Note#Heading" -> "Note > Heading").
QString linkDisplay(const QString& link)
{
    QStringList parts;
    for (const QString& p : link.split(u'#'))
        if (!p.isEmpty())
            parts << p;
    return parts.join(QLatin1String(" > ")).trimmed();
}

QFont tagFont(const QFont& base)
{
    QFont f = base;
    const qreal size = base.pixelSize() > 0 ? base.pixelSize() : base.pointSizeF() * 1.33;
    f.setPixelSize(std::max(8, int(std::round(size * 0.875)))); // --tag-size: --font-smaller
    return f;
}

void addText(QList<Run>& runs, const QString& text, const PaintOptions& o, const QColor& color = QColor())
{
    Run r;
    r.kind = Run::Kind::Text;
    r.text = text;
    r.font = o.font;
    r.color = color.isValid() ? color : o.color;
    r.width = QFontMetricsF(r.font).horizontalAdvance(text);
    runs << r;
}

void addLink(QList<Run>& runs, const QString& text, ValueHit::Kind kind, const QString& target, const QString& sourcePath,
             const PaintOptions& o)
{
    const Style& s = basesStyle();
    const propui::Style& ps = propui::style();
    Run r;
    r.kind = Run::Kind::Link;
    r.text = text;
    r.font = o.font;
    r.font.setUnderline(ps.linkUnderline);
    QColor c = s.link;
    if ((kind == ValueHit::Kind::Link) && o.isResolved && !o.isResolved(target, sourcePath)) {
        c = s.linkUnresolved;
        c.setAlphaF(c.alphaF() * s.linkUnresolvedOpacity);
    }
    r.color = c;
    r.width = QFontMetricsF(r.font).horizontalAdvance(text);
    r.hit = ValueHit{kind, target, sourcePath, {}};
    r.hasHit = true;
    runs << r;
}

void addTag(QList<Run>& runs, const QString& tag, const PaintOptions& o)
{
    const propui::Style& ps = propui::style();
    Run r;
    r.kind = Run::Kind::Tag;
    r.text = tag.startsWith(u'#') ? tag.mid(1) : tag;
    r.font = tagFont(o.font);
    r.color = ps.tagText;
    const qreal fs = r.font.pixelSize();
    r.width = QFontMetricsF(r.font).horizontalAdvance(r.text) + 2 * fs * 0.65;
    r.hit = ValueHit{ValueHit::Kind::Tag, tag.startsWith(u'#') ? tag : u'#' + tag, {}, {}};
    r.hasHit = true;
    runs << r;
}

void addGap(QList<Run>& runs, const PaintOptions& o)
{
    Run r;
    r.kind = Run::Kind::Gap;
    r.width = QFontMetricsF(o.font).horizontalAdvance(u' ');
    runs << r;
}

void addCheckbox(QList<Run>& runs, bool checked, const PaintOptions& o)
{
    Run r;
    r.kind = Run::Kind::Checkbox;
    r.checked = checked;
    const qreal size = QFontMetricsF(o.font).height() * 0.95;
    r.width = std::round(std::max<qreal>(12, std::min<qreal>(size, propui::style().checkboxSize)));
    runs << r;
}

void buildRuns(QList<Run>& runs, const Value& v, const PaintOptions& o, int depth = 0);

void buildList(QList<Run>& runs, const QList<Value>& items, const PaintOptions& o, int depth)
{
    for (qsizetype i = 0; i < items.size(); ++i) {
        if (i > 0)
            addGap(runs, o);
        buildRuns(runs, items[i], o, depth + 1);
    }
}

void buildRuns(QList<Run>& runs, const Value& v, const PaintOptions& o, int depth)
{
    if (depth > 8)
        return;
    const Style& s = basesStyle();
    switch (v.type()) {
    case ValueType::Null:
        return;
    case ValueType::Error: {
        Run w;
        w.kind = Run::Kind::Warning;
        w.icon = QStringLiteral("alert-triangle");
        w.width = s.iconXs + 4;
        QColor c = s.error;
        c.setAlphaF(0.6);
        w.color = c;
        runs << w;
        Run r;
        r.kind = Run::Kind::Text;
        r.text = v.text();
        r.font = s.uiSmaller;
        r.font.setUnderline(true);
        r.color = s.error;
        r.width = QFontMetricsF(r.font).horizontalAdvance(r.text);
        runs << r;
        return;
    }
    case ValueType::Boolean:
        addCheckbox(runs, v.boolValue(), o);
        return;
    case ValueType::Number: {
        const double n = v.number();
        addText(runs, std::isinf(n) ? QStringLiteral("∞") : v.toString(), o);
        return;
    }
    case ValueType::String:
    case ValueType::Html:
        addText(runs, v.type() == ValueType::Html ? QString(v.text()).remove(QRegularExpression(QStringLiteral("<[^>]*>"))) : v.text(), o);
        return;
    case ValueType::Tag:
        addTag(runs, v.text(), o);
        return;
    case ValueType::Link: {
        const Value* d = v.display();
        QString text;
        if (d)
            text = d->toString();
        else
            text = linkDisplay(v.text());
        addLink(runs, text, ValueHit::Kind::Link, v.text(), v.linkSourcePath(), o);
        return;
    }
    case ValueType::Url: {
        const Value* d = v.display();
        addLink(runs, d ? d->toString() : v.text(), ValueHit::Kind::Url, v.text(), {}, o);
        return;
    }
    case ValueType::File:
    case ValueType::ThisFile: {
        const QString path = v.filePath();
        QString name = path.mid(path.lastIndexOf(u'/') + 1);
        if (name.endsWith(QLatin1String(".md"), Qt::CaseInsensitive))
            name.chop(3);
        addLink(runs, name, ValueHit::Kind::File, path, {}, o);
        return;
    }
    case ValueType::Icon: {
        Run r;
        r.kind = Run::Kind::Icon;
        r.icon = v.text();
        r.width = s.iconS;
        r.color = o.color;
        runs << r;
        return;
    }
    case ValueType::Image: {
        const QString path = o.imagePath ? o.imagePath(v.text()) : QString();
        if (path.isEmpty())
            return;
        Run r;
        r.kind = Run::Kind::Image;
        r.imagePath = path;
        const QSize natural = ImageCache::instance().naturalSize(path);
        const qreal h = QFontMetricsF(o.font).height() + 6;
        r.width = natural.isValid() && natural.height() > 0 ? h * natural.width() / natural.height() : h;
        runs << r;
        return;
    }
    case ValueType::Date:
        addText(runs, formatDate(v), o);
        return;
    case ValueType::RelativeDate:
        addText(runs, v.relative(), o);
        return;
    case ValueType::List:
    case ValueType::TagList:
        buildList(runs, v.items(), o, depth);
        return;
    default:
        addText(runs, v.toString(), o);
        return;
    }
}

qreal totalWidth(const QList<Run>& runs)
{
    qreal w = 0;
    for (const Run& r : runs)
        w += r.width;
    return w;
}

void paintRuns(QPainter* p, const QRectF& rect, QList<Run>& runs, const PaintOptions& o, QList<ValueHit>* hits)
{
    const Style& s = basesStyle();
    const propui::Style& ps = propui::style();
    const qreal total = totalWidth(runs);
    qreal x = rect.left();
    if (o.align == PaintOptions::Align::Right && total < rect.width())
        x = rect.right() - total;
    else if (o.align == PaintOptions::Align::Center && total < rect.width())
        x = rect.left() + (rect.width() - total) / 2;
    const qreal cy = rect.center().y();
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    p->setRenderHint(QPainter::TextAntialiasing);
    for (Run& r : runs) {
        if (x >= rect.right())
            break;
        const qreal avail = rect.right() - x;
        const bool clipped = r.width > avail;
        switch (r.kind) {
        case Run::Kind::Gap:
            break;
        case Run::Kind::Text:
        case Run::Kind::Link: {
            p->setFont(r.font);
            p->setPen(r.color);
            const QFontMetricsF fm(r.font);
            const QString text = clipped && o.elide ? fm.elidedText(r.text, Qt::ElideRight, avail) : r.text;
            const QRectF tr(x, rect.top(), std::min(r.width, avail), rect.height());
            p->drawText(QRectF(x, rect.top(), clipped ? avail : r.width + 1, rect.height()), Qt::AlignLeft | Qt::AlignVCenter, text);
            if (r.hasHit && hits) {
                r.hit.rect = QRectF(x, cy - fm.height() / 2, tr.width(), fm.height());
                *hits << r.hit;
            }
            break;
        }
        case Run::Kind::Tag: {
            const QFontMetricsF fm(r.font);
            const qreal h = fm.height() + 2 * r.font.pixelSize() * 0.25;
            const QRectF pill(x, cy - h / 2, std::min(r.width, avail), h);
            p->setPen(Qt::NoPen);
            p->setBrush(ps.tagBg);
            p->drawRoundedRect(pill, std::min(ps.tagRadius, h / 2), std::min(ps.tagRadius, h / 2));
            p->setFont(r.font);
            p->setPen(r.color);
            const qreal padX = r.font.pixelSize() * 0.65;
            const QRectF textRect = pill.adjusted(padX, 0, -padX, 0);
            p->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
                        clipped && o.elide ? fm.elidedText(r.text, Qt::ElideRight, std::max<qreal>(0, textRect.width())) : r.text);
            if (hits) {
                r.hit.rect = pill;
                *hits << r.hit;
            }
            break;
        }
        case Run::Kind::Checkbox: {
            const QRectF box(x, cy - r.width / 2, r.width, r.width);
            drawCheckbox(p, box, r.checked, false);
            break;
        }
        case Run::Kind::Icon:
            drawIcon(p, QRectF(x, cy - r.width / 2, r.width, r.width), r.icon, int(r.width), r.color);
            break;
        case Run::Kind::Warning:
            drawIcon(p, QRectF(x, cy - s.iconXs / 2.0, s.iconXs, s.iconXs), r.icon, s.iconXs, r.color, 2);
            break;
        case Run::Kind::Image: {
            const qreal h = std::min(rect.height(), QFontMetricsF(o.font).height() + 6);
            const qreal w = std::min(r.width * h / std::max<qreal>(1, QFontMetricsF(o.font).height() + 6), avail);
            const std::shared_ptr<const QImage> img = ImageCache::instance().image(r.imagePath, int(std::ceil(w)), p->device() ? p->device()->devicePixelRatioF() : 1);
            if (img && !img->isNull())
                p->drawImage(QRectF(x, cy - h / 2, w, h), *img);
            break;
        }
        }
        x += r.width;
    }
    p->restore();
}

// A property value's text as the text widget shows it (String(value ?? "")).
QString propertyText(const QVariant& v) { return props::propertyValueText(v); }

} // namespace

QString formatDate(const Value& date)
{
    if (std::isnan(date.dateMs()))
        return QString();
    const QDateTime dt = QDateTime::fromMSecsSinceEpoch(qint64(date.dateMs()));
    return date.hasTime() ? QLocale().toString(dt, localeDateFormat() + u' ' + localeTimeFormat()) : QLocale().toString(dt.date(), localeDateFormat());
}

QString formatDateText(const QString& text, bool withTime)
{
    if (withTime) {
        if (auto dt = props::parseLenientDateTime(text))
            return QLocale().toString(*dt, localeDateFormat() + u' ' + localeTimeFormat());
    } else if (auto d = props::parseLenientDate(text)) {
        return QLocale().toString(*d, localeDateFormat());
    }
    return text;
}

void drawIcon(QPainter* p, const QRectF& rect, const QString& name, int size, const QColor& color, double stroke)
{
    QString n = name;
    if (n.startsWith(QLatin1String("lucide-")))
        n = n.mid(7);
    const qreal dpr = p->device() ? p->device()->devicePixelRatioF() : qApp->devicePixelRatio();
    const QPixmap pm = IconProvider::instance().pixmap(n, size, color, stroke, dpr);
    const QPointF c = rect.center();
    p->drawPixmap(QPointF(std::round(c.x() - size / 2.0), std::round(c.y() - size / 2.0)), pm);
}

void drawCheckbox(QPainter* p, const QRectF& box, bool checked, bool enabled)
{
    const propui::Style& ps = propui::style();
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    const QRectF r = box.adjusted(0.5, 0.5, -0.5, -0.5);
    const qreal radius = std::min(ps.checkboxRadius, r.height() / 3);
    if (checked) {
        p->setPen(Qt::NoPen);
        p->setBrush(ps.checkboxFill);
        p->drawRoundedRect(r, radius, radius);
        QPen pen(ps.checkboxMarker, std::max<qreal>(1.5, r.width() / 8));
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p->setPen(pen);
        QPainterPath check;
        check.moveTo(r.left() + r.width() * 0.25, r.top() + r.height() * 0.52);
        check.lineTo(r.left() + r.width() * 0.43, r.top() + r.height() * 0.7);
        check.lineTo(r.left() + r.width() * 0.76, r.top() + r.height() * 0.32);
        p->setBrush(Qt::NoBrush);
        p->drawPath(check);
    } else {
        p->setPen(QPen(ps.checkboxBorder, 1));
        p->setBrush(Qt::NoBrush);
        p->drawRoundedRect(r, radius, radius);
    }
    if (!enabled)
        Q_UNUSED(enabled);
    p->restore();
}

qreal paintValue(QPainter* p, const QRectF& rect, const Value& v, const PaintOptions& o, QList<ValueHit>* hits)
{
    QList<Run> runs;
    buildRuns(runs, v, o);
    paintRuns(p, rect, runs, o, hits);
    return totalWidth(runs);
}

qreal measureValue(const Value& v, const PaintOptions& o)
{
    QList<Run> runs;
    buildRuns(runs, v, o);
    return totalWidth(runs);
}

namespace {

void buildPropertyRuns(QList<Run>& runs, const QVariant& raw, const QString& type, const QString& sourcePath, const PaintOptions& o)
{
    if (props::isNullProperty(raw) && type != u"checkbox")
        return;
    if (type == u"checkbox") {
        addCheckbox(runs, raw.toBool(), o);
        return;
    }
    if (type == u"date" || type == u"datetime") {
        addText(runs, formatDateText(propertyText(raw), type == u"datetime"), o);
        return;
    }
    if (type == u"number") {
        addText(runs, raw.typeId() == QMetaType::QString ? raw.toString() : Value::jsNumber(raw.toDouble()), o);
        return;
    }
    if (type == u"multitext" || type == u"tags" || type == u"aliases") {
        const QStringList items = props::propertyValueList(raw);
        for (qsizetype i = 0; i < items.size(); ++i) {
            if (i > 0) {
                Run g;
                g.kind = Run::Kind::Gap;
                g.width = propui::style().pillGap;
                runs << g;
            }
            const QString& item = items[i];
            if (type == u"tags") {
                addTag(runs, item, o);
            } else if (item.startsWith(QLatin1String("[[")) && item.endsWith(QLatin1String("]]"))) {
                QString inner = item.mid(2, item.size() - 4);
                const qsizetype pipe = inner.indexOf(u'|');
                const QString target = pipe >= 0 ? inner.left(pipe) : inner;
                addLink(runs, pipe >= 0 ? inner.mid(pipe + 1) : linkDisplay(target), ValueHit::Kind::Link, target, sourcePath, o);
            } else if (!item.contains(u' ') && Value::isUrl(item) && item.contains(QLatin1String("://"))) {
                addLink(runs, item, ValueHit::Kind::Url, item, {}, o);
            } else {
                addText(runs, item, o);
            }
        }
        return;
    }
    if (type == u"unknown") {
        addText(runs, props::jsonText(raw), o);
        return;
    }
    // text: a link value shows as a link
    const QString text = propertyText(raw);
    if (text.startsWith(QLatin1String("[[")) && text.endsWith(QLatin1String("]]")) && !text.contains(u'\n')) {
        QString inner = text.mid(2, text.size() - 4);
        const qsizetype pipe = inner.indexOf(u'|');
        const QString target = pipe >= 0 ? inner.left(pipe) : inner;
        addLink(runs, pipe >= 0 ? inner.mid(pipe + 1) : linkDisplay(target), ValueHit::Kind::Link, target, sourcePath, o);
        return;
    }
    if (!text.contains(u' ') && Value::isUrl(text) && text.contains(QLatin1String("://"))) {
        addLink(runs, text, ValueHit::Kind::Url, text, {}, o);
        return;
    }
    QString line = text;
    line.replace(u'\n', u' ');
    addText(runs, line, o);
}

} // namespace

qreal paintProperty(QPainter* p, const QRectF& rect, const QVariant& raw, const QString& type, const QString& sourcePath, const PaintOptions& o,
                    QList<ValueHit>* hits)
{
    QList<Run> runs;
    buildPropertyRuns(runs, raw, type, sourcePath, o);
    paintRuns(p, rect, runs, o, hits);
    return totalWidth(runs);
}

qreal measureProperty(const QVariant& raw, const QString& type, const PaintOptions& o)
{
    QList<Run> runs;
    buildPropertyRuns(runs, raw, type, QString(), o);
    return totalWidth(runs);
}

} // namespace oc::basesui
