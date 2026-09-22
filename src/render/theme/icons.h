#pragma once

#include <QColor>
#include <QHash>
#include <QIcon>
#include <QPixmap>
#include <QString>

namespace oc {

// Lucide icon set (ISC) plus a few Obsidian-specific custom glyphs, rendered from SVG with a given
// stroke color/width. Obsidian names ("lucide-file-text", "sidebar-left", "help-circle", ...) are
// normalized to current Lucide names.
class IconProvider {
public:
    static IconProvider& instance();

    bool has(const QString& name) const;
    QString canonicalName(const QString& name) const;

    // sizePx is in logical pixels; dpr selects the backing resolution.
    QPixmap pixmap(const QString& name, int sizePx, const QColor& color, double strokeWidth = 1.75, qreal dpr = 1.0) const;
    // Icon with normal/active/disabled colors from the current theme (muted / normal / faint).
    QIcon icon(const QString& name, int sizePx = 18, double strokeWidth = 1.75) const;

    QByteArray svg(const QString& name, const QColor& color, double strokeWidth) const;

private:
    IconProvider();
    QHash<QString, QString> m_bodies;
    QHash<QString, QString> m_aliases;
    mutable QHash<QString, QPixmap> m_cache;
};

} // namespace oc
