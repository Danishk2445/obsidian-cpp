#pragma once

// Painting Bases values the way Obsidian renders them (renderTo of each value class):
//  - formula / file values (.bases-rendered-value): text, numbers ("∞" for infinities), links
//    (.internal-link: the display text, else the link text with "#" shown as " > ", unresolved
//    links faded), files (their short name as a link), URLs, tags (pills, without "#"), lists
//    (items separated by a gap), dates (the locale's date input format), relative dates,
//    durations, booleans (a disabled checkbox), icons, images (a thumbnail as tall as the line),
//    HTML (as text), errors (warning icon + message in --text-error);
//  - note properties (the property widgets in their static state): text (links shown as links),
//    numbers, checkboxes, dates, lists / tags / aliases as pills.
// Painting records hit rectangles for links, URLs, tags and files (clicks, hover previews).

#include "core/bases/base_value.h"

#include <QColor>
#include <QFont>
#include <QList>
#include <QRectF>
#include <QString>

#include <functional>

class QPainter;

namespace oc::basesui {

struct ValueHit {
    enum class Kind { Link, File, Url, Tag };
    Kind kind = Kind::Link;
    QString target;     // Link: the link text; File: the vault path; Url: the URL; Tag: "#tag"
    QString sourcePath; // links: where they resolve from
    QRectF rect;
};

struct PaintOptions {
    QFont font;
    QColor color;
    enum class Align { Left, Right, Center };
    Align align = Align::Left;
    bool elide = true;
    // Does a link resolve? (link text, source path)
    std::function<bool(const QString&, const QString&)> isResolved;
    // An image file's absolute path for a vault path / link ("" when none).
    std::function<QString(const QString& target)> imagePath;
};

// Paints `v` in `rect` and returns its natural width. `hits` (optional) receives the clickable parts.
qreal paintValue(QPainter* p, const QRectF& rect, const bases::Value& v, const PaintOptions& o, QList<ValueHit>* hits = nullptr);
qreal measureValue(const bases::Value& v, const PaintOptions& o);

// A note property value as its property widget shows it; `type` is the widget ("text", "number",
// "checkbox", "date", "datetime", "multitext", "tags", "aliases", "unknown").
qreal paintProperty(QPainter* p, const QRectF& rect, const QVariant& raw, const QString& type, const QString& sourcePath,
                    const PaintOptions& o, QList<ValueHit>* hits = nullptr);
qreal measureProperty(const QVariant& raw, const QString& type, const PaintOptions& o);

// The date input's display format ("MM/dd/yyyy" in en_US), with or without the time.
QString formatDate(const bases::Value& date);
QString formatDateText(const QString& isoText, bool withTime); // "2024-01-15" / "2024-01-15T10:30"

// Draw a Lucide icon centered in `rect`.
void drawIcon(QPainter* p, const QRectF& rect, const QString& name, int size, const QColor& color, double stroke = 1.75);

// Paint an Obsidian checkbox (checked / unchecked, disabled look when !enabled).
void drawCheckbox(QPainter* p, const QRectF& box, bool checked, bool enabled = false);

} // namespace oc::basesui
