#pragma once

#include <QColor>
#include <QHash>
#include <QImage>
#include <QString>

#include <memory>

namespace oc {

// Renders TeX math to images (JKQTMathText, native), cached. Errors render the raw TeX in the error
// colour like Obsidian does for MathJax errors.
class MathRenderer {
public:
    struct Result {
        QImage image;        // device pixels (devicePixelRatio set)
        QSizeF size;         // logical px
        qreal ascent = 0;    // logical px above the baseline
        bool error = false;
        QString errorText;
    };
    using ResultPtr = std::shared_ptr<const Result>;

    static MathRenderer& instance();

    // display = block math ($$), rendered with \displaystyle. fontPx = surrounding text size.
    ResultPtr render(const QString& tex, bool display, qreal fontPx, const QColor& color, qreal dpr = 1.0);
    bool available() const;
    void clear();

private:
    MathRenderer();
    QHash<QString, ResultPtr> m_cache;
};

} // namespace oc
