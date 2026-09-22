#pragma once

#include "render/layout/decoration.h"

#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/State>

#include <QHash>

#include <vector>

namespace oc {

class Document;

// Block-level styling of the raw Markdown source (Obsidian's "Source mode"): heading lines get
// heading fonts with faint '#', code/math/frontmatter lines monospace with a code background, list and
// quote markers faint, list lines use the list indent as tab stop and a hanging indent.
// Inline styling (bold, links, tags...) is layered on by the inline decorator (M2).
class SourceDecorator : public LineDecorator {
public:
    explicit SourceDecorator(const Document* doc) : m_doc(doc) {}
    void setDocument(const Document* doc) { m_doc = doc; }
    void setStyleSheet(const StyleSheet* ss) { m_styles = ss; }

    LineDecoration decorate(int line) override;
    StyleId quickLineStyle(int line) override;
    qreal quickVerticalPadding(int line) override;

    // Extra decorations merged into every line (search highlights, preedit...), set by the view.
    std::function<void(int line, LineDecoration&)> extra;

private:
    void highlightCode(int line, LineDecoration& d);

    struct FenceCache {
        quint64 version = 0;
        KSyntaxHighlighting::Definition def;
        std::vector<KSyntaxHighlighting::State> states; // state after body line i
        std::vector<QList<Mark>> marks;
    };

    const Document* m_doc;
    const StyleSheet* m_styles = nullptr;
    QHash<int, FenceCache> m_fenceCache; // key: fence opening line
};

} // namespace oc
