#include "render/theme/icons.h"

#include "render/theme/theme.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QSvgRenderer>

namespace oc {

IconProvider& IconProvider::instance()
{
    static IconProvider p;
    return p;
}

IconProvider::IconProvider()
{
    for (const char* res : {":/oc/icons/lucide.json", ":/oc/icons/custom.json"}) {
        QFile f(QString::fromLatin1(res));
        if (!f.open(QIODevice::ReadOnly)) {
            qWarning("oc: missing icon resource %s", res);
            continue;
        }
        const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
        for (auto it = o.begin(); it != o.end(); ++it)
            m_bodies.insert(it.key(), it.value().toString());
    }
    // Obsidian / older Lucide names -> current Lucide names.
    m_aliases = {
        {QStringLiteral("sidebar-left"), QStringLiteral("panel-left")},
        {QStringLiteral("sidebar-right"), QStringLiteral("panel-right")},
        {QStringLiteral("sidebar-open"), QStringLiteral("panel-left-open")},
        {QStringLiteral("vertical-three-dots"), QStringLiteral("ellipsis-vertical")},
        {QStringLiteral("more-vertical"), QStringLiteral("ellipsis-vertical")},
        {QStringLiteral("more-horizontal"), QStringLiteral("ellipsis")},
        {QStringLiteral("help-circle"), QStringLiteral("circle-help")},
        {QStringLiteral("check-circle"), QStringLiteral("circle-check")},
        {QStringLiteral("check-circle-2"), QStringLiteral("circle-check")},
        {QStringLiteral("alert-triangle"), QStringLiteral("triangle-alert")},
        {QStringLiteral("alert-circle"), QStringLiteral("circle-alert")},
        {QStringLiteral("x-circle"), QStringLiteral("circle-x")},
        {QStringLiteral("dice"), QStringLiteral("dice-5")},
        {QStringLiteral("quote-glyph"), QStringLiteral("quote")},
        {QStringLiteral("documents"), QStringLiteral("files")},
        {QStringLiteral("document"), QStringLiteral("file")},
        {QStringLiteral("edit"), QStringLiteral("square-pen")},
        {QStringLiteral("pencil-line"), QStringLiteral("pen-line")},
        {QStringLiteral("sheets-in-box"), QStringLiteral("archive")},
        {QStringLiteral("left-arrow"), QStringLiteral("arrow-left")},
        {QStringLiteral("right-arrow"), QStringLiteral("arrow-right")},
        {QStringLiteral("left-arrow-with-tail"), QStringLiteral("arrow-left")},
        {QStringLiteral("right-arrow-with-tail"), QStringLiteral("arrow-right")},
        {QStringLiteral("cross"), QStringLiteral("x")},
        {QStringLiteral("search-glyph"), QStringLiteral("search")},
        {QStringLiteral("reading-glasses"), QStringLiteral("book-open")},
        {QStringLiteral("gear"), QStringLiteral("settings")},
        {QStringLiteral("folder-closed"), QStringLiteral("folder-closed")},
        {QStringLiteral("layout-grid"), QStringLiteral("layout-grid")},
        {QStringLiteral("dot-network"), QStringLiteral("git-fork")},
        {QStringLiteral("filled-pin"), QStringLiteral("pin")},
        {QStringLiteral("sort-asc"), QStringLiteral("arrow-up-narrow-wide")},
        {QStringLiteral("sort-desc"), QStringLiteral("arrow-down-wide-narrow")},
        {QStringLiteral("file-plus-2"), QStringLiteral("file-plus")},
        {QStringLiteral("unlink"), QStringLiteral("unlink")},
    };
}

QString IconProvider::canonicalName(const QString& name) const
{
    QString n = name;
    if (n.startsWith(u"lucide-"))
        n = n.mid(7);
    if (m_bodies.contains(n))
        return n;
    const QString a = m_aliases.value(n);
    if (!a.isEmpty() && m_bodies.contains(a))
        return a;
    return n;
}

bool IconProvider::has(const QString& name) const { return m_bodies.contains(canonicalName(name)); }

QByteArray IconProvider::svg(const QString& name, const QColor& color, double strokeWidth) const
{
    const QString body = m_bodies.value(canonicalName(name));
    const QString svg = QStringLiteral(
                            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"24\" height=\"24\" viewBox=\"0 0 24 24\" fill=\"none\" "
                            "stroke=\"%1\" stroke-opacity=\"%2\" stroke-width=\"%3\" stroke-linecap=\"round\" stroke-linejoin=\"round\">%4</svg>")
                            .arg(color.name(QColor::HexRgb))
                            .arg(QString::number(color.alphaF(), 'f', 3))
                            .arg(QString::number(strokeWidth, 'f', 2))
                            .arg(QString(body).replace(QStringLiteral("currentColor"), color.name(QColor::HexRgb)));
    return svg.toUtf8();
}

QPixmap IconProvider::pixmap(const QString& name, int sizePx, const QColor& color, double strokeWidth, qreal dpr) const
{
    const QString key = QStringLiteral("%1|%2|%3|%4|%5").arg(canonicalName(name)).arg(sizePx).arg(color.rgba()).arg(strokeWidth).arg(dpr);
    auto it = m_cache.constFind(key);
    if (it != m_cache.cend())
        return *it;
    const int dev = std::max(1, int(std::ceil(sizePx * dpr)));
    QPixmap pm(dev, dev);
    pm.fill(Qt::transparent);
    if (m_bodies.contains(canonicalName(name))) {
        // Lucide strokes are designed at 24px; keep the visual stroke width constant across sizes like
        // CSS does (stroke-width is in viewBox units, so scale it by 24/size).
        const double sw = strokeWidth * 24.0 / std::max(1, sizePx);
        QSvgRenderer r(svg(name, color, sw));
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        r.render(&p, QRectF(0, 0, dev, dev));
    }
    pm.setDevicePixelRatio(dpr);
    m_cache.insert(key, pm);
    return pm;
}

QIcon IconProvider::icon(const QString& name, int sizePx, double strokeWidth) const
{
    const ThemePtr t = currentTheme();
    QIcon ic;
    for (qreal dpr : {1.0, 2.0}) {
        ic.addPixmap(pixmap(name, sizePx, t->color(QStringLiteral("--icon-color"), QColor(0xb3, 0xb3, 0xb3)), strokeWidth, dpr), QIcon::Normal);
        ic.addPixmap(pixmap(name, sizePx, t->color(QStringLiteral("--icon-color-focused"), QColor(0xda, 0xda, 0xda)), strokeWidth, dpr), QIcon::Active);
        ic.addPixmap(pixmap(name, sizePx, t->color(QStringLiteral("--text-faint"), QColor(0x66, 0x66, 0x66)), strokeWidth, dpr), QIcon::Disabled);
        ic.addPixmap(pixmap(name, sizePx, t->color(QStringLiteral("--icon-color-active"), QColor(0xa6, 0x8a, 0xf9)), strokeWidth, dpr), QIcon::Selected);
    }
    return ic;
}

} // namespace oc
