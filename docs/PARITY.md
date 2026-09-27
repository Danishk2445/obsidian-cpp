# Feature parity checklist

Status: ✅ done · 🟡 partial · ⬜ not started · (V) = verified against Obsidian oracle

## Shell (M0)
- ✅ Vault picker (recent vaults, create, open folder, Obsidian's registry as read-only suggestions)
- ✅ Frameless window with header-row drag, edge resize, min/max/close; native-frame option
- ✅ Ribbon, left/right sidedocks (icon tabs, collapse), floating status bar, notices
- ✅ Theme tokens (dark/light/system, accent), Lucide icons, QSS
- ✅ Workspace tabs/splits (M1/M4)

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
- ✅ All properties' context menu: property type, rename (F2; merges into an existing name after asking), delete.
- ✅ Backlinks in document: the backlinks pane below the text (border, header buttons at the right, natural height),
  per view ("backlinks" / "backlinkOpts" state), backlink:toggle-backlinks-in-document, the Backlinks option
  for new views.
- ⬜ URL bookmarks are kept and shown but can't be created here, graph bookmarks open the graph without its
  saved options; leaf groups aren't persisted (a "... for the current file" view follows its note until restart).
- Differences: unresolved outgoing links are also dimmed (Obsidian: icon only); dropping onto a collapsed bookmark
  group puts the item inside it (Obsidian: after it unless you wait for it to expand); moving an outline section to
  the very top adds no blank line before it.

## App commands, menus and integration (M4–M7)
- ✅ Editor commands with Obsidian's ids: checklist toggle / cycle (Obsidian's state machine, numbered items kept),
  insert footnote (`[^n]` + definition at the end), insert tag, readable line length / line numbers /
  spellcheck toggles (global, like Obsidian's plain callbacks), open link under cursor in a new split.
- ✅ Workspace commands: close others / close tab group / close others in tab group, copy URL, delete file, show
  trash (local trash only), toggle ribbon, toggle default new-pane mode, toggle light / dark, open help (F1), show
  debug info (modal with "Copy to clipboard").
- ✅ Pane menus: the "more options" and tab-header menus use Obsidian's section order (close, pane, open, action, find,
  info, info.copy → "Copy path", view, view.linked → "Open linked view", system, danger) with the core file-menu
  items (copy Obsidian URL / path from vault / from system root, open in default app, show in system explorer,
  reveal in navigation, move file to..., rename..., delete, open backlinks / outgoing links / outline / local graph /
  file properties).
- ✅ Rename prompts: "Note title" / "File name" (validation errors as tooltips); "Rename this heading..."
  (editor:rename-heading) and "Rename this block ID..." update every link to `note#Heading` / `note#^id` (aliases
  kept, Markdown links encoded) with "Updated N links in M files."; the note's own change is one undoable edit.
- ✅ Core plugin switches: disabling a core plugin hides its commands, ribbon actions, status-bar items and views
  (and detaches its leaves); enabling restores its sidebar leaf.
- ✅ Export to PDF: Obsidian's modal (include file name, page size, landscape, margin, downscale percent), save
  dialog, "Preparing PDF..." / "PDF Saved to X" notices, opens the PDF afterwards; settings in pdfExportSettings.
- ✅ Vaults: "Open another vault" / "Manage vaults..." switcher (fuzzy, "Active"), the vault picker.
- ✅ Single instance per clone config: a second launch hands its vault / file to the running process;
  `obsidian-cpp note.md` and `obsidian-cpp 'obsidian://open?vault=...&file=...'` targets; `--new-instance`.
- ✅ Desktop integration: icon, .desktop entry (Markdown files; it does not claim `obsidian://`), install rules
  (`cmake --install build/rel --prefix ~/.local`).
- ⬜ Window zoom (window:zoom-in / zoom-out / reset-zoom, Appearance "Zoom level"): Qt has no public runtime
  equivalent of Chromium's page zoom; use the font size setting or `QT_SCALE_FACTOR`.
- ✅ Line numbers ("Show line number"): gutter in the text column (numbers in --text-faint at --font-ui-smaller,
  tabular, --file-folding-offset before the text), editing modes only; baseline-aligned with the line (Obsidian:
  top-aligned).
- ✅ Pop-out windows: workspace.json "floating" split with "window" items (x / y / width / height / maximize /
  zoom), restored on start; frameless like the main window (tab row = title bar + window buttons; no ribbon,
  sidedocks or status bar) or native; own modals / notices / suggest / page previews; hotkeys, command palette
  and quick switcher act on the focused window; the active leaf follows window focus; commands
  workspace:move-to-new-window / open-in-new-window / new-window / close-window, editor:open-link-in-new-window;
  "Open in new window" (file menus, bookmarks), "Move to new window" (tab menu); Mod+Alt+Shift+click on links;
  tab drags between windows, out of every window = a new window at the drop point; closing a pop-out closes its
  tabs (pinned too; undo reopens them), closing the main window keeps them for the next start. Wayland: no
  window positions (saved as Qt reports them), tabs leave a window as a native drag.
- ⬜ Stacked tabs, always-on-top, per-window zoom (round-tripped only).

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
- ✅ Applied: line numbers, properties in document, backlinks in document, quick font size adjustment (Ctrl+wheel
  changes baseFontSize by 1 in 10...30, not within 500 ms of a plain scroll), always focus new tabs (off: links and
  files opened in a new tab load in the background; "+" / New tab still focus), show tab title bar (the view header).
- 🟡 Stored but not applied yet: indentation guides, RTL, strict line breaks, default file to open, delete
  attachments with files, URI callbacks, native menus, editing-mode status bar toggle
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

## Media views and embeds (M8)
- ✅ Views for Obsidian's extension lists: "image" (bmp png jpg jpeg gif svg webp avif), "audio" (mp3 wav m4a 3gp
  flac ogg oga opus), "video" (mp4 webm ogv mov mkv), "pdf"; opened from the file explorer, links, the quick switcher.
- ✅ Image view: centred, max-width 100%, scrolls; zoom like 1.13's image lightbox (Ctrl+wheel ×1…×10 around the
  pointer, double-click ×2.5 / back, drag to pan); animated GIF/WebP; live reload.
- ✅ PDF view (QtPdf, the PDF.js viewer's look): toolbar (sidebar toggle + options, zoom out | in + display options:
  fit width / height, single / two-page odd / even, "Adapt to theme"; page input "of N" / "(3 of 10)" with page
  labels), thumbnails / table of contents sidebar (resizable), continuous scroll, PDF.js zoom steps and presets,
  Ctrl+wheel zoom, text selection + copy (Ctrl+C / Ctrl+A / double-click word), links, find bar (N of M matches,
  match case, highlight all, whole words, F3 / Mod+G / Enter), "#page=N" / "#page=N&offset=x,y[,zoom]" subpaths
  from links and eState, arrow keys turn pages when fitted, password prompt, reload on modify; "Copy link to page".
- ✅ Audio / video: Chromium-style controls (play, time, seek, mute + hover volume, speed menu; video: overlay bar,
  fullscreen, click to play); a video without a picture plays as audio.
- ✅ Embeds in Live Preview, Reading view and hover previews: `![[x.pdf]]` (page high, `#page=`, `#height=`),
  `![[a.mp3]]` (42 px bar), `![[v.mp4]]` (natural size); hosted widgets near the viewport, pooled / destroyed
  off-screen (playing media survives); "Edit this block" (Live Preview) and the "Open" menu on PDF embeds; hovering a
  PDF / audio / video link previews it (PDF 450×400 without toolbar); embeds inside note previews resolve.
- ⬜ PDF annotations (popups, "Copy link to annotation"), `#selection=` highlights, named destinations, the
  selection context menu's "Copy as quote" / "Copy link to selection", PDF outline "current item" tracking.
- Differences: the PDF view remembers its page and zoom in the view state (Obsidian reopens at the top); embeds in
  callout bodies / table cells and exported PDFs are static renderings (PDF export prints the embedded page); the
  video view fits tall videos to the view height instead of scrolling; `|width` is ignored on PDF / media embeds
  (like Obsidian's CSS).

## Properties (M7)
- ✅ Properties panel under the inline title (Live Preview; read-only in Reading): heading menu (add, sort, clear),
  rows with type icon menu (type, cut / copy / paste as `obsidian/properties`, remove), key and value suggestions,
  editors for text (links shown as links), list / tags / aliases pills, number, checkbox, date, date & time,
  nested maps as JSON; type mismatch warning ("Display as ...?"); invalid YAML box with "Show source"; Obsidian's
  keyboard navigation; each edit is one undoable transaction. `propertiesInDocument` visible / hidden / source.
- ✅ Byte-exact minimal frontmatter patches (untouched lines keep bytes and EOLs; changed values in Obsidian's YAML
  style, checked against Obsidian's serializer output); types.json in the clone config; vault-wide rename / delete.
- ✅ Commands: markdown:add-metadata-property (Mod+;), add-alias, clear-metadata-properties, toggle-fold-properties.
- Differences: Obsidian rewrites the whole block on every change (we patch minimally); Reading view is read-only
  (Obsidian allows editing there); values parsed by yaml-cpp (`012`, `0x10` stay strings); Qt date editors.
- ⬜ Drag-reordering properties, per-file collapsed state, narrow layout, "File properties" sidebar fallback.

## Note embeds (M3/M7)
- ✅ `![[Note]]` (title line, frontmatter hidden), `#Heading` / `#Heading#Sub` sections, `#^block` (list items with
  children), `#[^1]` footnotes, "Unable to find “x” in Note", “x” is not created yet. Click to create., depth 5,
  cycles, tall embeds scroll (max-height token), open-link icon, links inside follow / hover-preview, live refresh
  from open documents and disk; Live Preview reveal; Reading view, hover previews and PDF export.
- Differences: text inside an embed can't be selected; a cycle stops at the first repeat (Obsidian nests to depth 5);
  checkboxes inside embeds don't toggle; embeds in callouts / table cells are static renderings.

## Canvas (M9)
- ✅ JSON Canvas 1.0: byte-stable reader / writer in Obsidian's layout (untouched files never rewritten, unknown keys
  kept), text / file (notes with subpaths, images, PDF / audio / video, canvases as minimaps) / link / group cards,
  edges (sides, ends, colours, labels, reconnect), select / box-select / move with snapping / clone / axis lock /
  resize / connect / delete / group / recolour, undo (100 snapshots), copy / paste (`obsidian/canvas`), align /
  distribute, pan / zoom / fit (Shift+1 / Shift+2), controls, card and selection menus, "Canvas help", read-only
  mode, settings tab, `![[x.canvas]]` embeds, canvases as link sources (graph, backlinks), rename follow-ups.
- Differences: link cards show the URL (no web view); "Export as image" without tiling / logo options; global search
  doesn't search canvases; cards are sized to media only on creation.

## Raw HTML
- ✅ Inline HTML in Live Preview, Reading view, table cells, callouts and PDF export (`src/editor/html`): tags paired
  per paragraph and hidden, content formatted — `b strong i em cite dfn var u ins s strike del mark code kbd tt samp
  sub sup small big`, `a href` (follows like a link), `span`/`font`/any element with `style` colour, background,
  weight, style, decoration, size and `<font color size>`; `<br>` breaks the line; `<img src>` of a vault file
  renders as an image. Obsidian's never-rendered tags (script style link meta object embed webview) stay source in
  Live Preview; Reading view drops script/style with their content and stray / unknown tags like a browser.
- ✅ Live Preview reveal like Obsidian's HTML widget: the outermost element shows its source while the selection
  touches it (whole lines when it is alone on them); an element that is never closed stays source.
- ✅ Top-level HTML blocks (`<p align=center>`, `<div>`, `<table>`, `<details>`, `<img>` alone on a line...) render
  as a block (QTextDocument, sanitized, theme styles for tables / kbd / code / mark / headings / links), revealed in
  Live Preview while the selection is inside; `<details>` opens / closes on its summary; `<a href>` inside follows.
  Blocks rendering nothing (`<div align=center>` alone, comments, script) keep their source in Live Preview and take
  no room in Reading view.
- Differences: markdown inside inline HTML renders (Obsidian's Live Preview widget shows it raw; its Reading view
  renders it); a block element opened in one HTML block and closed in another (`<div>` … blank line … `</div>`) is
  rendered per block in both modes (Obsidian's Live Preview makes it one widget); remote images show their alt text;
  no hover "</>" edit button (click the block to edit); iframes, audio/video tags and inline SVG aren't rendered.
