# Feature parity checklist

Status: ✅ done · 🟡 partial · ⬜ not started · (V) = verified against Obsidian oracle

## Shell (M0)
- ✅ Vault picker (recent vaults, create, open folder, Obsidian's registry as read-only suggestions)
- ✅ Frameless window with header-row drag, edge resize, min/max/close; native-frame option
- ✅ Ribbon, left/right sidedocks (icon tabs, collapse), floating status bar, notices
- ✅ Theme tokens (dark/light/system, accent), Lucide icons, QSS
- ⬜ Workspace tabs/splits (M1/M4)

(sections for M1..M9 are added as work lands)

## Inline parser (M2)
- ✅ `md::parseInline()` (src/markdown/inline_parser.cpp, rules in inline_rules.{h,cpp}): CommonMark 0.31.2 inlines
  (escapes, entities, code spans, emphasis, links/images incl. reference links, autolinks, raw HTML, breaks), GFM
  `~~strike~~` and extended autolinks, Obsidian `[[wikilinks]]`/`![[embeds]]` (subpaths, aliases, `\|` in table
  cells, embed sizes), `$math$`/`$$math$$`, `%%comments%%`, `#tags`, `==highlight==`, `[^ref]`, `^[inline]`, ` ^block-id`,
  image sizes `![alt|100x200](x)`. Mark children for every hideable piece of syntax (live preview).
- ✅ Agreement on the CommonMark 0.31.2 inline-section examples: 374/378 paragraphs = 98.9% with the spec's HTML,
  373/378 = 98.7% with cmark-gfm 0.29.0.gfm.13; random differential vs cmark-gfm green (tests/markdown/inline_spec_test).

### Inline parser divergences
From the CommonMark spec (intentional):
- GFM extended autolinks are always on: bare `https://…`, `www.…` and `a@b.co` are links (spec examples 602, 608,
  611, 612).
- Obsidian syntax wins where it starts first: `[[[foo]]]` is a wikilink to `[foo` (Obsidian's reading view does the
  same), `![[foo]]` is an embed, `$…$` is math, `%%…%%` a comment, `#tag` a tag, `[^x]` a footnote ref, `^[…]` an
  inline footnote (examples 548, 559, 590 are therefore excluded from the comparison).
- `~~` and `==` pair only as runs of exactly two characters (Obsidian); GFM also strikes `~one~`.
From cmark-gfm 0.29.0.gfm.13 (the differential oracle), where we follow CommonMark 0.31.2 / GFM instead:
- numeric references have at most 7 digits (example 28); Unicode symbols (S*) count as punctuation for flanking
  (`*£*bravo.`); an escaped character inside `<foo\+@bar.example.com>` stays an escape (example 606).
- cmark's core bugs: after a link, a later `[` re-enables earlier link openers (cmark nests links in
  `[a [b](c) [x]](d)`); its 0.29 `openers_bottom` is per character only (`**, a _*_*`); `![^x](y)` is read as `!` +
  link; entities are decoded before backslash escapes in link destinations (`\&quot;`). md4c (0.31) agrees with us
  on all of these; the random differential uses it as the tie-breaker.
- cmark-gfm's email autolinker keeps its dot count across a second `@` (`a@b.co@b` links `b.co@b`, but GFM needs a
  `.` in the domain).
- Mirrored on purpose: cmark-gfm checks autolink domains byte-wise, so a non-ASCII character ends the underscore
  check (`http://a.bcé_x` is a link); `check_domain`/`autolink_delim` trimming rules are cmark-gfm's.
Obsidian editor vs reading view (one tree serves both; the choice made):
- `$` math follows the editor: only the first later `$` on the line can close; it must not follow whitespace or be
  followed by a digit (`$5 and $10`, `$HOME and $PATH` stay text); no line crossing. Reading view skips invalid
  closers and allows one line break.
- `%%comment%%` may span lines (editor); reading view matches within one line.
- Tags need the start of the content or whitespace before `#` (both); reading view also accepts a tag glued to a
  preceding construct (`**b**#tag`), the editor does not, neither do we. `(#tag)` is not a tag (verified: both
  sides require whitespace).
- Wikilinks follow the reading view/metadata cache: `[[` + at least one character + first `]]` on the line, no `[[`
  inside; alias after the first `|` (unless it starts the trimmed text).
- A bare URL inside `^[inline footnote]` ends before the next `]` so the footnote still closes (the reading view
  tokenizes the footnote text on its own); GFM's "no bare URLs inside link brackets" applies to `[`/`![` only.
Open parity questions:
- `$` and `#tag` inside markdown link text: the editor disables both there, the reading view does not; we allow them.
- Table cells: the editor ends math at an unescaped `|`; cells arrive here already split by the block parser.
- Bare URLs: Obsidian's editor uses a liberal URL regex (many schemes such as `obsidian://`, `file:///`, `ssh://`,
  `www1.`, `example.com/path`, different trailing-punctuation rules); we implement GFM's extended autolinks only.
