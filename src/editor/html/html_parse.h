#pragma once

// Raw HTML helpers for rendering Markdown's inline and block HTML (not a full HTML parser): single-tag
// parsing with attributes, Obsidian's element classes (app.js: the inline/block tag lists used to pick
// a span or a block widget, and the tags it never renders), the `style="…"` subset that inline
// rendering understands, and a fragment tokenizer for the block renderer.

#include <QColor>
#include <QList>
#include <QString>
#include <QStringView>

namespace oc::html {

struct Attribute {
    QString name;  // lowercase
    QString value; // entity-decoded ("" for a bare attribute)
};

struct Tag {
    QString name;          // lowercase; "" for comments, processing instructions, declarations, CDATA
    bool closing = false;  // </name>
    bool selfClosing = false;
    bool comment = false;  // <!-- --> <? ?> <!X> <![CDATA[ ]]>
    QList<Attribute> attrs;

    bool hasAttr(QStringView name) const;
    QString attr(QStringView name) const;
};

// Parses one complete tag ("<a href='x'>", "</b>", "<br/>", "<!-- c -->"). False if `src` isn't one.
bool parseTag(QStringView src, Tag* out);
// Decodes character references (&amp; &#39; &#x1F;) in text or attribute values.
QString decodeEntities(QStringView s);

// Elements without content (<br>, <img>...).
bool isVoidElement(QStringView name);
// Obsidian never renders these (app.js F3): script style link meta object embed webview.
bool isExcludedElement(QStringView name);
// Obsidian's inline test (app.js Lm): the inline tag list, the block tag list, else the browser's default
// display (unknown elements are inline).
bool isInlineElement(QStringView name);

// The subset of an inline `style` attribute applied to inline HTML.
struct InlineStyle {
    QColor color;
    QColor background;
    int weight = 0;         // 0 = unset (QFont weight otherwise)
    int italic = -1;        // -1 unset, 0/1
    int underline = -1;
    int strike = -1;
    qreal sizePx = 0;       // absolute font size (px), 0 = unset
    qreal sizeScale = 0;    // relative font size (em, %, smaller/larger), 0 = unset

    bool isEmpty() const
    {
        return !color.isValid() && !background.isValid() && weight == 0 && italic < 0 && underline < 0 && strike < 0 && sizePx <= 0
               && sizeScale <= 0;
    }
};
InlineStyle parseInlineStyle(QStringView css);
// CSS colour: names, #hex, rgb()/rgba()/hsl()/hsla(); invalid if unsupported (var() etc.).
QColor parseCssColor(QStringView value);
// <font size="1".."7" | "+1" | "-1"> in px.
qreal fontSizeAttrPx(QStringView value, qreal basePx);

// A fragment split into text runs and tags (for the block renderer).
struct Token {
    enum class Kind : quint8 { Text, Tag };
    Kind kind = Kind::Text;
    qsizetype from = 0;
    qsizetype to = 0;
    Tag tag; // Kind::Tag
};
QList<Token> tokenize(QStringView html);

} // namespace oc::html
