#pragma once

// Test-only renderers for md::InlineTree:
//  - renderHtml(): cmark-compatible HTML (escaping, href percent-encoding, image alt text) so a paragraph
//    can be compared with `cmark-gfm --to html` output. Obsidian constructs get simple, stable markup.
//  - dumpSexpr(): S-expression of the node tree for golden tests. Non-Mark nodes print as
//    (Type{info} children...), Mark nodes as (MarkKind "text"), uncovered content as "text".

#include "markdown/inline_tree.h"

#include <QString>

namespace oc::md::test {

// cmarkGfmCompat: like cmark-gfm's HTML writer, a Strong directly inside a Strong emits no tags.
QString renderHtml(const InlineTree& tree, bool cmarkGfmCompat = false);      // inline HTML of the whole content
QString renderParagraph(const InlineTree& tree, bool cmarkGfmCompat = false); // "<p>" + renderHtml + "</p>\n"
QString dumpSexpr(const InlineTree& tree);
QString dumpSexpr(const QString& content, const QList<LinkRefDefinition>& refs = {}); // parse + dump

const char* typeName(InlineType t);
const char* markName(MarkKind k);

} // namespace oc::md::test
