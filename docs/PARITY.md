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
  Cell content keeps `\|` as written and `parseInline()` does not know it is parsing a cell: outside code/math
  `\|` is an Escape (the backslash hides like any escape), inside `[[…]]` it is the alias separator, but inside a
  code span or math it stays `\|` where GFM shows `|`.
- Bare URLs: Obsidian's editor uses a liberal URL regex (many schemes such as `obsidian://`, `file:///`, `ssh://`,
  `www1.`, `example.com/path`, different trailing-punctuation rules); we implement GFM's extended autolinks only.

## Sidebar views (M3)
- ✅ Backlinks: linked mentions (links, embeds, property links resolving to the note) and unlinked mentions (name +
  aliases as whole words, any case, outside links and frontmatter; found with the search engine, re-checked
  incrementally), Obsidian's snippets (100 chars of line context, "Show more context" = list item / section /
  paragraph, per-snippet more-context buttons), Collapse results, sort orders, search filter, the "Link" button,
  "Backlinks for X" when pinned / opened for a note; state keys as Obsidian's.
- ✅ Outgoing links: one row per destination (headings, blocks, unresolved "Not created yet"), unlinked mentions of
  other notes with destination chips that link them (pipes escaped in tables).
- ✅ Tags view: vaultTags() counts, nested / flat, name / frequency orders, filter, folds ("tag-pane-fold").
- ✅ Outline: heading tree (plain-text headings), current section (cursor; scroll in reading view), auto-scroll,
  collapse / expand all, filter, click → eState {line}, drag a heading to move its section.
- ✅ Bookmarks: bookmarks.json (Obsidian's item shapes, unknown keys kept), groups, drag reorder (into groups),
  inline rename, remove, "Bookmark..." modal, the bookmarks:* commands, file-menu / editor-menu items, folds.
- ✅ All properties: names with counts and type icons (types.json + reserved), orders, filter, click → ["name"].
- ⬜ Backlinks in document (needs an EditorView bottom widget); All properties' context menu (type, rename,
  delete); URL bookmarks are kept and shown but can't be created here, graph bookmarks open the graph without its
  saved options; leaf groups aren't persisted (a "... for the current file" view follows its note until restart).
- Differences: unresolved outgoing links are also dimmed (Obsidian: icon only); dropping onto a collapsed bookmark
  group puts the item inside it (Obsidian: after it unless you wait for it to expand); moving an outline section to
  the very top adds no blank line before it.

## Settings (M7)
- ✅ Modal: Obsidian 1.13's sidebar layout (search, Options / Core plugins / Community plugins groups), setting
  cards (groups, lists, sub-pages with back button), global settings search (simple search over names,
  descriptions, aliases; grouped hits, "N more"), keyboard navigation in the sidebar, Esc closes a page first
- ✅ Tabs in 1.13's order: General, Appearance, Interface, Editor, Files and links, Hotkeys, Keychain (empty state:
  no plugins use secrets), Core plugins, Community plugins (restricted-mode explanation + built-in Greentext Guard)
- ✅ Plugin tabs: Backlinks, Command palette (pinned), Daily notes, File recovery, Note composer, Page preview,
  Quick switcher, Templates, Unique note creator, Greentext Guard
- ✅ Hotkeys: filter (name/id), filter by pressed chord, pills (Conflicts N, All, Assigned, Assigned by me,
  Unassigned), record / remove / restore default, conflicts marked with tooltips; hotkeys.json
- 🟡 Stored but not applied yet: line numbers, indentation guides, RTL, strict line breaks, properties in document,
  always focus new tabs, default file to open, delete attachments with files, URI callbacks, tab title
  bar, native menus, quick font size adjustment, backlinks in document, editing-mode status bar toggle
- 🟡 CSS snippets: only their CSS custom properties (body/:root/.theme-light/.theme-dark) are applied
- ⬜ Ribbon item reordering, community themes, translucency, zoom level, custom app icon

## Vim mode (M8)
- ✅ "Vim key bindings" (`vimMode`) toggles live on every open editor. Behaviour follows the vim.js Obsidian 1.13
  ships (CodeMirror-vim on its CM6 adapter); a 404-case key-sequence table is checked against that code run as an
  oracle (CodeMirror 6 + jsdom, indentUnit "\t", tabSize 4).
- ✅ Modes normal / insert / replace / visual / visual-line / visual-block; counts; motions (h j k l w W b B e E ge gE
  0 ^ $ g_ gg G | f F t T ; , % { } ( ) H M L gj gk g0 g^ g$ + - _ * # n N / ? gn gN ]` [` marks); operators
  (d c y > < = g~ gu gU gq gw, ~ u U in visual); text objects (w W s p quotes brackets b B tags); x X s S C D Y p P
  ]p [p J gJ r R ~ o O a A i I gI gi <C-a> <C-x> . u <C-r>; registers ("a-"z, "A-"Z, "0-"9, "-, "", "_, ".", ":",
  "/", "+ and "* = system clipboard); marks, jumps (<C-o>/<C-i> unless a hotkey owns them), macros (q @ @@);
  visual o O gv I A (block insert); insert-mode <C-w> <C-u> <C-o> <C-r>x <C-t> <C-d> <Esc> <C-[> <C-c>; user
  mappings (:map :nmap :imap :noremap … :unmap :mapclear).
- ✅ Ex: :{range} :s :%s :'<,'>s (flags g i c, count), :g :v :normal :sort :d :y :j :delmarks :marks :registers :noh
  :set (pcre, textwidth, insertModeEscKeysTimeout, filetype) :undo :redo :startinsert :image, plus :w :q :wq :x
  (Obsidian's "editor:save-file" / "workspace:close").
- ✅ The ":" / "/" command line and messages are a panel docked under the text (.cm-vim-panel styling); block cursor in
  --interactive-accent with the glyph in --text-on-accent; search matches use the search highlight.
- ✅ Obsidian's hotkeys keep precedence over vim (Obsidian resolves them in a capture-phase keydown listener): Mod+P,
  Mod+O (not vim's jump back), Mod+E, Mod+D (not half-page down), Mod+F, Mod+W … run their commands; unbound chords
  (<C-r> <C-v> <C-a> <C-x> <C-[> <C-y> …) and plain keys reach vim.
- ✅ Each vim command and each insert session is one undo step (Document History groups); undo/redo put the cursor at
  the start of the change like Obsidian's adapter.
- Differences (deliberate): a count past the line end moves / deletes to the end ("5l", "5x"; Obsidian's moveByChar
  does nothing); "g_", "za/zo/zc/zR/zM" and ":q/:wq/:x" exist (not in Obsidian's keymap); "it"/"at" find tags by
  scanning the text (Obsidian uses the syntax tree); ":j" with a range joins all its lines and ":y" takes the range
  (Obsidian: one line too few / the cursor line, and a blockwise "0); "* is the clipboard (Obsidian: not a register);
  "+p reads the clipboard synchronously; insert-mode deletions are part of "." (Obsidian records insertions only);
  ":s" uses the match's own captures for $1… (Obsidian re-runs the pattern on the matched text); undo steps are
  per command / insert session (CodeMirror joins changes within 500 ms).
- ⬜ Not done: langmap, :colorscheme, multi-cursor "run the command for every cursor" outside visual-block, image
  actions (g+ g- g= g<CR>), table-cell and embedded-image navigation with h/l/j/k, focusing the properties/title with k
  on the first line.
