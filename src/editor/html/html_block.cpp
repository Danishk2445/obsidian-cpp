#include "editor/html/html_block.h"

#include "editor/html/html_inline.h"
#include "editor/html/html_parse.h"
#include "render/services/image_cache.h"

#include <QAbstractTextDocumentLayout>
#include <QPainter>
#include <QPainterPath>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextObjectInterface>
#include <QTextTable>
#include <QUrl>

#include <cmath>

namespace oc {

namespace html {

// Draws <img src="ocimg:N"> through the ImageCache (async decode, placeholder until ready) at the size
// the attributes ask for, never wider than the block.
class ImageHandler : public QObject, public QTextObjectInterface {
    Q_OBJECT
    Q_INTERFACES(QTextObjectInterface)
public:
    QStringList paths;
    QColor placeholder;

    QString pathOf(const QTextFormat& f) const
    {
        const QString name = f.toImageFormat().name();
        if (!name.startsWith(QLatin1String("ocimg:")))
            return {};
        bool ok = false;
        const int i = name.mid(6).toInt(&ok);
        return ok && i >= 0 && i < paths.size() ? paths[i] : QString();
    }
    QSizeF intrinsicSize(QTextDocument* doc, int, const QTextFormat& f) override
    {
        const QTextImageFormat img = f.toImageFormat();
        const QString path = pathOf(f);
        const QSize natural = path.isEmpty() ? QSize() : ImageCache::instance().naturalSize(path);
        qreal w = natural.isValid() ? natural.width() : 16, h = natural.isValid() ? natural.height() : 16;
        if (img.width() > 0 && img.height() > 0) {
            w = img.width();
            h = img.height();
        } else if (img.width() > 0) {
            h = h * img.width() / std::max<qreal>(1, w);
            w = img.width();
        } else if (img.height() > 0) {
            w = w * img.height() / std::max<qreal>(1, h);
            h = img.height();
        }
        const qreal maxW = doc && doc->textWidth() > 0 ? doc->textWidth() : w;
        if (w > maxW) {
            h = h * maxW / w;
            w = maxW;
        }
        return {std::round(w), std::round(h)};
    }
    void drawObject(QPainter* p, const QRectF& r, QTextDocument*, int, const QTextFormat& f) override
    {
        const QString path = pathOf(f);
        const qreal dpr = p->device() ? p->device()->devicePixelRatioF() : 1.0;
        const auto img = path.isEmpty() ? nullptr : ImageCache::instance().image(path, int(std::ceil(r.width())), dpr);
        if (!img) {
            p->fillRect(r, placeholder);
            return;
        }
        p->save();
        p->setRenderHint(QPainter::SmoothPixmapTransform);
        QPainterPath clip;
        clip.addRoundedRect(r, 4, 4); // --image-radius
        p->setClipPath(clip, Qt::IntersectClip);
        p->drawImage(r, *img);
        p->restore();
    }
};

namespace {

QString cssColor(const QColor& c)
{
    if (!c.isValid())
        return QStringLiteral("transparent");
    if (c.alpha() == 255)
        return c.name(QColor::HexRgb);
    return QStringLiteral("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alphaF(), 0, 'f', 3);
}

QString escapeAttr(const QString& s) { return s.toHtmlEscaped(); }

// Attributes QTextDocument understands (and that are harmless).
bool keepAttribute(const QString& tag, const QString& name)
{
    static const QSet<QString> common = {QStringLiteral("align"), QStringLiteral("style"), QStringLiteral("dir")};
    if (common.contains(name))
        return true;
    if (tag == u"font")
        return name == u"color" || name == u"size" || name == u"face";
    if (tag == u"table")
        return name == u"border" || name == u"cellpadding" || name == u"cellspacing" || name == u"width" || name == u"bgcolor";
    if (tag == u"td" || tag == u"th")
        return name == u"colspan" || name == u"rowspan" || name == u"width" || name == u"valign" || name == u"bgcolor";
    if (tag == u"tr")
        return name == u"valign" || name == u"bgcolor";
    if (tag == u"ol")
        return name == u"start" || name == u"type";
    if (tag == u"ul")
        return name == u"type";
    if (tag == u"hr")
        return name == u"width";
    return false;
}

QString openTag(const QString& name, const Tag& t, const QString& extraStyle = {})
{
    QString out = QLatin1Char('<') + name;
    QString style = extraStyle;
    for (const Attribute& a : t.attrs) {
        if (!keepAttribute(name, a.name))
            continue;
        if (a.name == u"style") {
            style += a.value;
            if (!style.isEmpty() && !style.endsWith(u';'))
                style += u';';
            continue;
        }
        out += QLatin1Char(' ') + a.name + QStringLiteral("=\"") + escapeAttr(a.value) + QLatin1Char('"');
    }
    if (!style.isEmpty())
        out += QStringLiteral(" style=\"") + escapeAttr(style) + QLatin1Char('"');
    return out + QLatin1Char('>');
}

// Tags passed to QTextDocument as they are (it knows them).
bool isPassThrough(const QString& n)
{
    static const QSet<QString> set = {
        QStringLiteral("p"),   QStringLiteral("div"),   QStringLiteral("span"),   QStringLiteral("b"),     QStringLiteral("strong"),
        QStringLiteral("i"),   QStringLiteral("em"),    QStringLiteral("u"),      QStringLiteral("s"),     QStringLiteral("sub"),
        QStringLiteral("sup"), QStringLiteral("small"), QStringLiteral("big"),    QStringLiteral("tt"),    QStringLiteral("samp"),
        QStringLiteral("var"), QStringLiteral("cite"),  QStringLiteral("dfn"),    QStringLiteral("br"),    QStringLiteral("hr"),
        QStringLiteral("h1"),  QStringLiteral("h2"),    QStringLiteral("h3"),     QStringLiteral("h4"),    QStringLiteral("h5"),
        QStringLiteral("h6"),  QStringLiteral("ul"),    QStringLiteral("ol"),     QStringLiteral("li"),    QStringLiteral("dl"),
        QStringLiteral("dt"),  QStringLiteral("dd"),    QStringLiteral("blockquote"), QStringLiteral("pre"), QStringLiteral("table"),
        QStringLiteral("thead"), QStringLiteral("tbody"), QStringLiteral("tfoot"), QStringLiteral("tr"),   QStringLiteral("td"),
        QStringLiteral("th"),  QStringLiteral("caption"), QStringLiteral("center"), QStringLiteral("font"), QStringLiteral("nobr"),
        QStringLiteral("address")};
    return set.contains(n);
}

// Sectioning / grouping elements QTextDocument doesn't know: rendered as <div>.
bool isDivLike(const QString& n)
{
    static const QSet<QString> set = {QStringLiteral("section"), QStringLiteral("article"), QStringLiteral("header"),  QStringLiteral("footer"),
                                      QStringLiteral("main"),    QStringLiteral("nav"),     QStringLiteral("aside"),   QStringLiteral("figure"),
                                      QStringLiteral("figcaption"), QStringLiteral("hgroup"), QStringLiteral("fieldset"), QStringLiteral("legend"),
                                      QStringLiteral("form"),    QStringLiteral("dialog"),  QStringLiteral("search"),  QStringLiteral("menu"),
                                      QStringLiteral("dir"),     QStringLiteral("body"),    QStringLiteral("html")};
    return set.contains(n);
}

// Elements whose content is dropped with them (sanitized away / not renderable).
bool dropsContent(const QString& n)
{
    static const QSet<QString> set = {QStringLiteral("script"),  QStringLiteral("style"),  QStringLiteral("template"), QStringLiteral("title"),
                                      QStringLiteral("noscript"), QStringLiteral("iframe"), QStringLiteral("object"),   QStringLiteral("webview"),
                                      QStringLiteral("svg"),     QStringLiteral("math"),   QStringLiteral("head"),     QStringLiteral("textarea"),
                                      QStringLiteral("select"),  QStringLiteral("video"),  QStringLiteral("audio"),    QStringLiteral("canvas"),
                                      QStringLiteral("button")};
    return set.contains(n);
}

int pixelAttr(const QString& v)
{
    QString s = v.trimmed();
    if (s.endsWith(QLatin1String("px")))
        s.chop(2);
    bool ok = false;
    const double d = s.toDouble(&ok);
    return ok && d > 0 ? int(std::lround(d)) : -1;
}

} // namespace

BlockStyle BlockStyle::from(const StyleSheet& ss)
{
    BlockStyle s;
    const LineMetrics& normal = ss.line(StyleId::Normal);
    s.font = normal.font;
    s.text = normal.color;
    s.link = ss.spec(StyleId::ExternalLink).format.foreground().color();
    s.faint = ss.faint();
    const QTextCharFormat& code = ss.spec(StyleId::InlineCode).format;
    s.codeFg = code.foreground().color();
    s.codeBg = ss.codeBackground();
    s.monoFamily = code.fontFamilies().toStringList().value(0);
    s.codePx = code.property(QTextFormat::FontPixelSize).toInt();
    if (s.codePx <= 0)
        s.codePx = int(std::lround(ss.textSize() * 0.875));
    s.highlight = ss.spec(StyleId::Highlight).format.background().color();
    s.hr = ss.hrColor();
    s.tableBorder = ss.tableBorderColor();
    s.tableHeaderBg = ss.tableHeaderBackground();
    s.quoteBar = ss.quoteBarColor();
    s.lineBox = normal.lineBox;
    s.pSpacing = ss.pSpacing();
    s.listIndent = ss.listIndent();
    QString headings;
    for (int level = 1; level <= 6; ++level) {
        const LineMetrics& h = ss.line(StyleId(int(StyleId::Heading1) + level - 1));
        s.headingLineBox[size_t(level - 1)] = h.lineBox;
        s.headingPx[size_t(level - 1)] = h.font.pixelSize();
        headings += QStringLiteral("h%1 { font-size: %2px; font-weight: %3; color: %4; margin-top: %5px; margin-bottom: %5px; }\n")
                        .arg(level)
                        .arg(h.font.pixelSize())
                        .arg(int(h.font.weight()))
                        .arg(cssColor(h.color))
                        .arg(s.pSpacing);
    }
    // Obsidian's `.markdown-rendered` element styles, in QTextDocument's CSS subset.
    s.css = QStringLiteral("a { color: %1; text-decoration: underline; }\n"
                           "p { margin-top: %2px; margin-bottom: %2px; }\n"
                           "ul, ol { margin-top: 0px; margin-bottom: 0px; }\n"
                           "blockquote { margin-top: 0px; margin-bottom: 0px; margin-left: %3px; margin-right: 0px; }\n"
                           "pre { font-family: '%4'; font-size: %5px; background-color: %6; color: %7; margin-top: %2px; margin-bottom: %2px; }\n"
                           "table { border-collapse: collapse; margin-top: %2px; margin-bottom: %2px; }\n"
                           "td, th { border-width: 1px; border-style: solid; border-color: %8; padding-top: 4px; padding-bottom: 4px; "
                           "padding-left: 8px; padding-right: 8px; vertical-align: top; }\n"
                           "th { font-weight: 600; background-color: %9; text-align: left; }\n"
                           "dd { margin-left: %10px; }\n")
                .arg(cssColor(s.link))
                .arg(s.pSpacing)
                .arg(ss.quoteIndent())
                .arg(s.monoFamily)
                .arg(s.codePx)
                .arg(cssColor(s.codeBg))
                .arg(cssColor(s.codeFg))
                .arg(cssColor(s.tableBorder))
                .arg(cssColor(s.tableHeaderBg))
                .arg(s.listIndent)
          + headings;
    return s;
}

} // namespace html

using html::Tag;
using html::Token;

HtmlBlockWidget::Rewrite HtmlBlockWidget::rewrite(QStringView src, const html::BlockStyle& st, const ResolveFn& resolve, QList<bool>* detailsOpen)
{
    Rewrite r;
    const QList<Token> tokens = html::tokenize(src);
    QString& out = r.html;
    out.reserve(src.size() + 64);

    // <details>: a closed one shows only its <summary>, prefixed by a disclosure triangle that toggles it.
    struct Details {
        int index = 0;
        bool open = false;
        bool inSummary = false;
        bool emitted = false; // its <div> was written
    };
    QList<Details> details;
    int detailsCount = 0;
    auto hidden = [&] {
        for (const Details& d : details)
            if (!d.open && !d.inSummary)
                return true;
        return false;
    };
    QString skipUntil; // dropping the content of <script>, <style>...
    int preDepth = 0;
    const QString inlineCode = QStringLiteral("font-family: '%1'; font-size: %2px; background-color: %3; color: %4;")
                                   .arg(st.monoFamily)
                                   .arg(st.codePx)
                                   .arg(html::cssColor(st.codeBg))
                                   .arg(html::cssColor(st.codeFg));
    static const QString hairPad = QStringLiteral("&#8201;"); // CSS padding 0.25em around kbd / code

    for (const Token& t : tokens) {
        if (!skipUntil.isEmpty()) {
            if (t.kind == Token::Kind::Tag && t.tag.closing && t.tag.name == skipUntil)
                skipUntil.clear();
            continue;
        }
        if (t.kind == Token::Kind::Text) {
            if (hidden())
                continue;
            const QStringView text = src.mid(t.from, t.to - t.from);
            if (!text.trimmed().isEmpty())
                r.visible = true;
            out += text;
            continue;
        }
        const Tag& tag = t.tag;
        const QString& n = tag.name;
        if (tag.comment || n.isEmpty())
            continue;
        if (html::dropsContent(n)) {
            if (!tag.closing && !tag.selfClosing)
                skipUntil = n;
            continue;
        }
        if (n == u"details") {
            if (!tag.closing) {
                Details d;
                d.index = detailsCount++;
                if (detailsOpen) {
                    if (d.index >= detailsOpen->size())
                        detailsOpen->push_back(tag.hasAttr(u"open"));
                    d.open = detailsOpen->at(d.index);
                } else {
                    d.open = tag.hasAttr(u"open");
                }
                d.emitted = !hidden();
                if (d.emitted)
                    out += QStringLiteral("<div>");
                details.push_back(d);
            } else if (!details.isEmpty()) {
                const Details d = details.takeLast();
                if (d.emitted)
                    out += QStringLiteral("</div>");
            }
            continue;
        }
        if (n == u"summary") {
            if (details.isEmpty())
                continue;
            Details& d = details.last();
            if (!tag.closing) {
                d.inSummary = true;
                if (!hidden()) {
                    // The whole summary is the toggle (an anchor styled like text).
                    out += QStringLiteral("<div><a href=\"oc-details:%1\" style=\"color: %2; text-decoration: none;\">"
                                          "<span style=\"font-size: %3px;\">%4</span>&nbsp;")
                               .arg(d.index)
                               .arg(html::cssColor(st.text))
                               .arg(int(std::lround(st.font.pixelSize() * 0.6)))
                               .arg(d.open ? QStringLiteral("&#9660;") : QStringLiteral("&#9654;"));
                    r.visible = true;
                }
            } else if (d.inSummary) {
                if (!hidden())
                    out += QStringLiteral("</a></div>");
                d.inSummary = false;
            }
            continue;
        }
        if (hidden())
            continue;
        if (n == u"img") {
            if (tag.closing)
                continue;
            const QString srcAttr = tag.attr(u"src").trimmed();
            QString path;
            if (!srcAttr.isEmpty() && resolve) {
                if (srcAttr.startsWith(QLatin1String("file:"), Qt::CaseInsensitive))
                    path = QUrl(srcAttr).toLocalFile();
                else if (!html::hasUrlScheme(srcAttr) && !srcAttr.startsWith(QLatin1String("//")))
                    path = resolve(QUrl::fromPercentEncoding(srcAttr.toUtf8()));
            }
            if (!path.isEmpty() && ImageCache::instance().naturalSize(path).isValid()) {
                QString img = QStringLiteral("<img src=\"ocimg:%1\"").arg(r.images.size());
                if (const int w = html::pixelAttr(tag.attr(u"width")); w > 0)
                    img += QStringLiteral(" width=\"%1\"").arg(w);
                if (const int h = html::pixelAttr(tag.attr(u"height")); h > 0)
                    img += QStringLiteral(" height=\"%1\"").arg(h);
                out += img + QLatin1Char('>');
                r.images.push_back(path);
                r.visible = true;
            } else if (const QString alt = tag.attr(u"alt").simplified(); !alt.isEmpty()) {
                // remote / missing images: the alt text (nothing is fetched from the network)
                out += QStringLiteral("<span style=\"color: %1;\">%2</span>").arg(html::cssColor(st.faint), alt.toHtmlEscaped());
                r.visible = true;
            }
            continue;
        }
        if (n == u"a") {
            if (tag.closing) {
                out += QStringLiteral("</a>");
            } else {
                QString a = QStringLiteral("<a");
                if (tag.hasAttr(u"href"))
                    a += QStringLiteral(" href=\"") + tag.attr(u"href").trimmed().toHtmlEscaped() + QLatin1Char('"');
                out += a + QLatin1Char('>');
            }
            continue;
        }
        if (n == u"mark") {
            out += tag.closing ? QStringLiteral("</span>")
                               : QStringLiteral("<span style=\"background-color: %1; color: %2;\">").arg(html::cssColor(st.highlight), html::cssColor(st.text));
            continue;
        }
        if (n == u"kbd" || (n == u"code" && preDepth == 0)) {
            if (tag.closing)
                out += hairPad + QStringLiteral("</span>");
            else
                out += html::openTag(QStringLiteral("span"), tag, inlineCode) + hairPad;
            continue;
        }
        if (n == u"code") // inside <pre>
            continue;
        if (n == u"strike" || n == u"del") {
            out += tag.closing ? QStringLiteral("</s>") : html::openTag(QStringLiteral("s"), tag);
            continue;
        }
        if (n == u"ins") {
            out += tag.closing ? QStringLiteral("</u>") : html::openTag(QStringLiteral("u"), tag);
            continue;
        }
        if (n == u"pre")
            preDepth = std::max(0, preDepth + (tag.closing ? -1 : 1));
        QString name = n;
        if (html::isDivLike(n))
            name = QStringLiteral("div");
        else if (!html::isPassThrough(n))
            continue; // unknown element: its content stays, the tag goes (like DOMPurify's KEEP_CONTENT)
        if (n == u"br" || n == u"hr" || n == u"li")
            r.visible = true;
        if (tag.closing)
            out += QStringLiteral("</") + name + QLatin1Char('>');
        else if (html::isVoidElement(n))
            out += html::openTag(name, tag).chopped(1) + QStringLiteral(" />");
        else
            out += html::openTag(name, tag);
    }
    return r;
}

HtmlBlockWidget::HtmlBlockWidget(const QString& html, const StyleSheet& styles, ResolveFn resolveAttachment)
    : m_source(html)
    , m_style(html::BlockStyle::from(styles))
    , m_resolve(std::move(resolveAttachment))
{
    m_signature = qHashMulti(21, m_source);
    build();
}

HtmlBlockWidget::~HtmlBlockWidget() = default;

void HtmlBlockWidget::build()
{
    const Rewrite rw = rewrite(m_source, m_style, m_resolve, &m_detailsOpen);
    m_qtHtml = rw.html;
    m_visible = rw.visible;
    m_doc.reset();
    if (!m_images)
        m_images = std::make_unique<html::ImageHandler>();
    m_images->paths = rw.images;
    m_images->placeholder = m_style.codeBg;
    m_doc = std::make_unique<QTextDocument>();
    m_doc->setUndoRedoEnabled(false);
    m_doc->setDocumentMargin(0);
    m_doc->setDefaultFont(m_style.font);
    m_doc->setIndentWidth(m_style.listIndent);
    m_doc->setDefaultStyleSheet(m_style.css);
    m_doc->documentLayout()->registerHandler(QTextFormat::ImageObject, m_images.get());
    m_doc->setHtml(m_qtHtml);

    // Browser layout details QTextDocument doesn't do: a trailing <br> adds no line, the outer margins of the
    // first and last block don't count (the note's own spacing surrounds the block), CSS line boxes.
    const qreal basePx = m_style.font.pixelSize();
    for (QTextBlock b = m_doc->begin(); b.isValid(); b = b.next()) {
        const QString t = b.text();
        if (t.endsWith(QChar(QChar::LineSeparator))) {
            QTextCursor c(m_doc.get());
            c.setPosition(b.position() + int(t.size()) - 1);
            c.setPosition(b.position() + int(t.size()), QTextCursor::KeepAnchor);
            c.removeSelectedText();
        }
        QTextBlockFormat bf = b.blockFormat();
        const int level = bf.headingLevel();
        qreal lineH;
        if (level >= 1 && level <= 6) {
            lineH = m_style.headingLineBox[size_t(level - 1)];
        } else {
            int px = 0;
            for (auto it = b.begin(); !it.atEnd() && px <= 0; ++it)
                px = it.fragment().charFormat().font().pixelSize();
            if (px <= 0)
                px = int(basePx);
            const bool inTable = QTextCursor(b).currentTable() != nullptr;
            lineH = std::round(px * (inTable ? 1.3 : m_style.lineBox / std::max<qreal>(1, basePx)));
        }
        bf.setLineHeight(lineH, QTextBlockFormat::MinimumHeight);
        if (b == m_doc->begin())
            bf.setTopMargin(0);
        if (b == m_doc->lastBlock())
            bf.setBottomMargin(0);
        QTextCursor(b).setBlockFormat(bf);
    }
    const QList<QTextFrame*> frames = m_doc->rootFrame()->childFrames();
    if (!frames.isEmpty()) {
        auto zeroMargin = [](QTextFrame* f, bool top) {
            QTextFrameFormat ff = f->frameFormat();
            if (top)
                ff.setTopMargin(0);
            else
                ff.setBottomMargin(0);
            f->setFrameFormat(ff);
        };
        if (m_doc->begin().position() >= frames.first()->firstPosition() - 1)
            zeroMargin(frames.first(), true);
        if (m_doc->lastBlock().position() <= frames.last()->lastPosition() + 1)
            zeroMargin(frames.last(), false);
    }
    m_width = -1;
}

qreal HtmlBlockWidget::layout(qreal width)
{
    if (width == m_width)
        return m_height;
    m_width = width;
    if (!m_visible) {
        m_height = 0;
        return 0;
    }
    m_doc->setTextWidth(width);
    m_height = std::ceil(m_doc->size().height());
    return m_height;
}

void HtmlBlockWidget::paint(QPainter* p, const QRectF& rect)
{
    if (!m_visible)
        return;
    p->save();
    p->translate(rect.topLeft());
    QAbstractTextDocumentLayout::PaintContext ctx;
    ctx.palette.setColor(QPalette::Text, m_style.text);
    ctx.palette.setColor(QPalette::Link, m_style.link);
    ctx.palette.setColor(QPalette::Dark, m_style.hr);
    ctx.clip = QRectF(0, 0, rect.width(), rect.height());
    m_doc->documentLayout()->draw(p, ctx);
    p->restore();
}

bool HtmlBlockWidget::click(const QPointF& local, Qt::KeyboardModifiers)
{
    if (!m_visible)
        return false;
    const QString anchor = m_doc->documentLayout()->anchorAt(local);
    if (!anchor.startsWith(QLatin1String("oc-details:")))
        return false;
    bool ok = false;
    const int i = anchor.mid(11).toInt(&ok);
    if (!ok || i < 0 || i >= m_detailsOpen.size())
        return false;
    m_detailsOpen[i] = !m_detailsOpen[i];
    build();
    return true;
}

QString HtmlBlockWidget::linkAt(const QPointF& local) const
{
    if (!m_visible)
        return {};
    const QString anchor = m_doc->documentLayout()->anchorAt(local);
    if (anchor.isEmpty() || anchor.startsWith(QLatin1String("oc-details:")))
        return {};
    return html::hasUrlScheme(anchor) ? anchor : QUrl::fromPercentEncoding(anchor.toUtf8());
}

QString HtmlBlockWidget::plainText() const { return m_visible ? m_doc->toPlainText() : QString(); }

namespace html {

std::shared_ptr<BlockWidget> blockWidgetFor(const QString& source, const StyleSheet& styles, bool reading, int depth,
                                            const HtmlBlockWidget::ResolveFn& resolveAttachment,
                                            QHash<quint64, std::shared_ptr<BlockWidget>>& cache)
{
    // Keyed by content (not position): typing above a block keeps its widget.
    const quint64 key = qHashMulti(uint(0x48544d4c), source, depth, reading, styles.generation());
    std::shared_ptr<BlockWidget> w = cache.value(key);
    if (!w) {
        w = std::make_shared<HtmlBlockWidget>(source, styles, resolveAttachment);
        cache.insert(key, w);
    }
    if (!reading && static_cast<HtmlBlockWidget*>(w.get())->isEmpty())
        return nullptr;
    return w;
}

std::optional<BlockRange> blockRangeFor(const md::SyntaxTree& tree, const Text& text, int node, const StyleSheet& styles, bool reading, int depth,
                                        const HtmlBlockWidget::ResolveFn& resolveAttachment, QHash<quint64, std::shared_ptr<BlockWidget>>& cache)
{
    const md::BlockNode& n = tree.node(node);
    const QString source = text.slice(text.lineStart(n.fromLine), text.lineEnd(n.toLine - 1));
    std::shared_ptr<BlockWidget> w = blockWidgetFor(source, styles, reading, depth, resolveAttachment, cache);
    if (!w)
        return std::nullopt;
    int to = n.toLine;
    if (reading && static_cast<HtmlBlockWidget*>(w.get())->isEmpty() && to < tree.lineCount() && tree.role(to) == md::LineRole::Blank
        && tree.leafAt(to) < 0)
        ++to;
    return BlockRange{n.fromLine, to, std::move(w)};
}

} // namespace html

} // namespace oc

#include "html_block.moc"
