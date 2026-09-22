#pragma once

#include <QColor>
#include <QList>
#include <QString>

#include <functional>

namespace oc {

// A CSS length kept as a linear combination so em/rem-relative tokens (e.g. --h1-size: 1.618em,
// --list-indent: calc(0.5625em * 4)) can be resolved later against the right font size.
struct CssLength {
    double px = 0;
    double em = 0;
    double rem = 0;
    double resolve(double emPx, double remPx = 16.0) const { return px + em * emPx + rem * remPx; }
    bool isZero() const { return px == 0 && em == 0 && rem == 0; }
};

struct CssValue {
    enum Kind { None, Number, Percent, Length, Angle, Time, Color, Ident, String, List };
    Kind kind = None;
    double num = 0;          // Number, Percent (0..100), Angle (deg), Time (ms)
    CssLength len;           // Length
    QColor color;            // Color
    QString text;            // Ident / String
    QList<CssValue> items;   // List (comma separated at top level, else space separated)
    bool commaList = false;

    bool isValid() const { return kind != None; }
    // Convenience conversions (return fallback when the kind doesn't fit).
    double toNumber(double fallback = 0) const;
    double toPx(double emPx, double remPx = 16.0, double fallback = 0) const;
    QColor toColor(const QColor& fallback = {}) const;
};

// Evaluates CSS custom-property values. `lookup(name)` returns the raw value of a custom property
// (or nullopt-equivalent via *found=false). Supports var() with fallbacks, calc()/min()/max()/clamp(),
// hsl()/hsla()/rgb()/rgba() (comma and space syntax), color-mix(in srgb|oklch|oklab, ...), hex colors,
// named basic colors, lengths (px/em/rem), %, deg, ms/s, strings and identifiers.
class CssEvaluator {
public:
    using Lookup = std::function<QString(const QString& name, bool* found)>;

    explicit CssEvaluator(Lookup lookup);

    // Textual expansion of var() references (recursive, cycle-safe).
    QString substitute(const QString& value) const;
    // Parse + compute. Unsupported constructs yield CssValue::None.
    CssValue evaluate(const QString& value) const;

    static QColor parseHexColor(QStringView s);
    static QColor namedColor(const QString& ident);
    // CSS color-mix with premultiplied alpha. space: "srgb", "oklab" or "oklch".
    static QColor mix(const QColor& a, double pa, const QColor& b, double pb, const QString& space);

private:
    QString substituteImpl(const QString& value, int depth) const;
    Lookup m_lookup;
};

} // namespace oc
