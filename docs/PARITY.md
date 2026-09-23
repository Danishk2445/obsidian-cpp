# Feature parity checklist

Status: ✅ done · 🟡 partial · ⬜ not started · (V) = verified against Obsidian oracle

## Shell (M0)
- ✅ Vault picker (recent vaults, create, open folder, Obsidian's registry as read-only suggestions)
- ✅ Frameless window with header-row drag, edge resize, min/max/close; native-frame option
- ✅ Ribbon, left/right sidedocks (icon tabs, collapse), floating status bar, notices
- ✅ Theme tokens (dark/light/system, accent), Lucide icons, QSS
- ⬜ Workspace tabs/splits (M1/M4)

(sections for M1..M9 are added as work lands)

## Block parser (M1)
- ✅ CommonMark 0.31.2 block algorithm + GFM tables + Obsidian blocks (frontmatter, `$$` math, `%%` comments,
  callouts, footnote definitions, task items), incremental with budgets / `resume()`
- ✅ Differential tests (`markdown_spec_diff_test`): 650/652 CommonMark spec examples agree with cmark-gfm
  0.29.0.gfm.13 and with cmark 0.31.2 (99.7 %; the 2 are frontmatter, below), 17/17 GFM table cases, 3000/3000
  random documents (35 only modulo the tightness quirks below)
- ✅ Incremental == full parse on ~50k random edits (default / zero budget + resume / edits while incomplete),
  golden corpus typed in and deleted character by character (`markdown_incremental_test`, `markdown_golden_test`)

### Parser divergences
Intended (Obsidian over CommonMark):
- **Frontmatter**: line 0 exactly `---` (trailing spaces/tabs allowed) with a later line exactly `---` is a
  Frontmatter block, not a thematic break + setext heading (spec examples 96, 98). Unclosed → ordinary Markdown.
- **`$$` math / `%%` comment blocks** start only where a block can start (line content after container markers,
  ≤ 3 spaces) and may interrupt a paragraph; they close at the first following line *containing* `$$` / `%%`
  (text around it stays in the block) and otherwise run to the end of their container. A line holding both
  `$$ … $$` is a one-line math block only when nothing follows the closing `$$`; `%% … %%` on one line and
  `$$x$$ text` are inline (paragraph). Obsidian also lets an inline `%%` opened mid-line hide text across blank
  lines; that is left to the inline parser within one paragraph.
- **Callouts**: a blockquote whose first line matches `[!type]±` is a Callout. Its title line is a paragraph
  that can be continued (lazily too) — the continuation becomes the callout's first body paragraph, as in
  Obsidian's blockquote-based rendering — but never turns into a setext heading or a table header (`---` under a
  title is a thematic break inside the callout, `===` is body text).
- **Footnote definitions** (`[^label]:`, label without whitespace) can interrupt a paragraph and continue over
  lines indented ≥ 4 columns, lazy paragraph lines and any whitespace-only line (cmark-gfm only continues over
  truly empty lines; its footnote extension is not available here to compare).
- **Tasks**: `[c]` for any single character `c` except `]` right after a list marker, followed by space/tab/end
  of line; non-BMP characters are reported as their first UTF-16 unit (`ListItemInfo::taskChar` is a `QChar`).

Kept on purpose where cmark differs from the spec / other implementations (tolerated by the differential test):
- cmark keeps a thematic break open over following blank lines, so a list whose only blank lines follow a
  thematic break inside an item is tight for cmark; it is loose here (spec + remark semantics).
- cmark-gfm treats a table without body rows as "ending with a blank line", making its list loose; not here.

Representation (same block structure, different node shapes than cmark's XML):
- Link reference definitions at the start of a paragraph become a `LinkRefDef` node (cmark drops them and keeps
  one paragraph node); a paragraph of only refdefs is a `LinkRefDef` node.
- Container ranges exclude trailing blank lines; leaf ranges are exact (unclosed fences / math / comments
  include the empty last line of a document that ends with a newline).
- Table cell content (`tableCellContent`) keeps `\|` as written: GFM removes the backslash before inline parsing,
  so `` `x \| y` `` renders `x | y` there. `LeafContent` cannot drop a character inside one line (segments are
  joined with `\n`), so the inline parser has to treat `\|` in table cells as a plain `|`.
- `ListItemInfo::contentCol` is the character offset where the content starts on the item's first line (for an
  empty item, the end of the marker line), not CommonMark's W + N column count.
