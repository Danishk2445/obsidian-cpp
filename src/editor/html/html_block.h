#pragma once

#include "markdown/syntax_tree.h"
#include "render/layout/decoration.h"
#include "render/layout/style_sheet.h"

#include <QColor>
#include <QFont>
#include <QHash>
#include <QList>
#include <QStringList>

#include <array>
#include <functional>
#include <memory>
#include <optional>

class QTextDocument;

namespace oc {

namespace html {
class ImageHandler;

// Theme values the HTML block renderer uses (copied from a StyleSheet, which may be rebuilt later).
struct BlockStyle {
    QFont font;                 // --font-text, --font-text-size
    QColor text, link, faint, codeFg, codeBg, highlight, hr, tableBorder, tableHeaderBg, quoteBar;
    QString monoFamily;
    int codePx = 14;
    qreal lineBox = 24;         // px of one line of body text
    std::array<qreal, 6> headingLineBox{};
    std::array<int, 6> headingPx{};
    qreal pSpacing = 16;
    qreal listIndent = 36;
    QString css;                // QTextDocument default style sheet

    static BlockStyle from(const StyleSheet& styles);
};
} // namespace html

// A top-level HTML block (CommonMark HTML block: `<p align="center">…</p>`, `<table>…`, `<details>…`,
// `<img src=…>` alone on a line...) rendered like Obsidian's sanitized HTML: the markup is rewritten
// for QTextDocument (script/style & co. dropped, <details> folded behind a disclosure triangle,
// <mark>/<kbd>/<code> styled from the theme, vault images drawn through the ImageCache) and laid out
// at the content width. The source stays untouched; Live Preview shows it while the selection is in
// the block (the decorator doesn't produce the widget then).
class HtmlBlockWidget : public BlockWidget {
public:
    using ResolveFn = std::function<QString(const QString& linkpath)>;

    HtmlBlockWidget(const QString& html, const StyleSheet& styles, ResolveFn resolveAttachment);
    ~HtmlBlockWidget() override;

    // Renders nothing visible (only tags, comments, script/style): zero height; Live Preview keeps the source.
    bool isEmpty() const { return !m_visible; }
    qreal layout(qreal width) override;
    void paint(QPainter* p, const QRectF& rect) override;
    quint64 signature() const override { return m_signature; }
    bool click(const QPointF& local, Qt::KeyboardModifiers mods) override; // <summary> toggles its <details>
    QString linkAt(const QPointF& local) const override;                    // <a href>

    // Introspection (tests).
    const QString& qtHtml() const { return m_qtHtml; }
    QString plainText() const;
    const QTextDocument* document() const { return m_doc.get(); }

    // The rewrite of `html` for QTextDocument. `detailsOpen` holds the state of each <details> in order
    // (entries are appended from the `open` attribute the first time).
    struct Rewrite {
        QString html;
        QStringList images;   // absolute paths, referenced as src="ocimg:<index>"
        bool visible = false; // any text, image, rule or line break
    };
    static Rewrite rewrite(QStringView html, const html::BlockStyle& style, const ResolveFn& resolveAttachment, QList<bool>* detailsOpen);

private:
    void build();

    QString m_source;
    html::BlockStyle m_style;
    ResolveFn m_resolve;
    QList<bool> m_detailsOpen;
    QString m_qtHtml;
    bool m_visible = false;
    std::unique_ptr<html::ImageHandler> m_images;
    std::unique_ptr<QTextDocument> m_doc;
    quint64 m_signature = 0;
    qreal m_width = -1;
    qreal m_height = 0;
};

namespace html {
// The widget for an HTML block's source, from `cache` when possible. nullptr when Live Preview should keep
// the source (the block renders nothing visible).
std::shared_ptr<BlockWidget> blockWidgetFor(const QString& source, const StyleSheet& styles, bool reading, int depth,
                                            const HtmlBlockWidget::ResolveFn& resolveAttachment,
                                            QHash<quint64, std::shared_ptr<BlockWidget>>& cache);
// The block range of a top-level HTML block node (nullopt: keep the source). In Reading view a block that
// renders nothing also covers the blank line after it, so it adds no paragraph gap (CSS margins collapse
// through an empty element).
std::optional<BlockRange> blockRangeFor(const md::SyntaxTree& tree, const Text& text, int node, const StyleSheet& styles, bool reading,
                                        int depth, const HtmlBlockWidget::ResolveFn& resolveAttachment,
                                        QHash<quint64, std::shared_ptr<BlockWidget>>& cache);
} // namespace html

} // namespace oc
