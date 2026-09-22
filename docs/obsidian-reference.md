# Obsidian 1.13.7 reference values
**Colors.**
- Base palette, dark: 00 #1C1C1C · 05 #212121 · 10 #232323 · 20 #282828 · 25 #2e2e2e · 30 #333333 · 35 #3f3f3f · 40 #555555 · 50 #666666 · 60 #999999 · 70 #b3b3b3 · 100 #dadada.
- Base palette, light: 00 #fff · 05 #fcfcfc · 10 #fafafa · 20 #f6f6f6 · 25 #efefef · 30 #e4e4e4 · 35 #dadada · 40 #bdbdbd · 50 #ababab · 60 #707070 · 70 #5c5c5c · 100 #222.
- Named colors (light / dark):

| Color | Light | Dark |
|---|---|---|
| red | #e93147 | #fb464c |
| orange | #ec7500 | #e9973f |
| yellow | #e0ac00 | #e0de71 |
| green | #08b94e | #44cf6e |
| cyan | #00bfbc | #53dfdd |
| blue | #086ddd | #027aff |
| purple | #7852ee | #a882ff |
| pink | #d53984 | #fa99cd |

- Accent: hsl(258,88%,66%) = #8a5cf5.
  - accent-1: light #9873f7, dark #a68af9.
  - accent-2: light #a68af9, dark #c5b6fc.
- Semantic tokens:

| Token | Value |
|---|---|
| bg-primary | base-00 |
| bg-primary-alt | base-10 |
| bg-secondary | base-20 |
| bg-secondary-alt | base-05 (light) / base-30 (dark) |
| border | base-30 (hover 35, focus 40) |
| modifier-hover | mono-100 at 6.7% |
| form-field | base-00 / base-25 |
| text-normal / muted / faint | base-100 / base-70 / base-50 |
| text-accent | accent (light) / accent-1 (dark); hover accent-2 |
| interactive-accent | accent-1 (light) / accent (dark) |
| interactive-normal | base-00/base-30 (hover base-10/base-35) |
| selection | accent at 20% (light) / 33% (dark) |
| highlight | rgba(255,208,0,.4) |

**Type.**
- Text 16px; UI 12/13/15/20; line-height 1.5 (tight 1.3); p-spacing 1rem; file-line-width 700px; file margins 32px.
- Headings:

| | h1 | h2 | h3 | h4 | h5 | h6 |
|---|---|---|---|---|---|---|
| size (em) | 1.618 | 1.462 | 1.318 | 1.188 | 1.076 | 1 |
| line-height | 1.2 | 1.2 | 1.3 | 1.4 | 1.5 | 1.5 |
| weight (variable font) | 700 | 680 | 660 | 640 | 620 | 600 |
| letter-spacing (em) | −.015 | −.011 | −.008 | −.005 | −.002 | 0 |

- Bold = weight + 200. The inline title uses h1 values.

**Layout.**
- Chrome: ribbon 44, view header 40, titlebar 30, frame right space 126, sidedock 300 (user: left 300, right 200).
- Tabs: width 200 (max 320), radius 4 (active 6 6 0 0), font 13px.
- Radii s/m/l/xl = 4/8/12/24. Input height 30, radius 5. Icons 16/18/32, stroke 1.75, opacity .85. Scrollbar 12.
- Overlays: prompt 700 wide; modal max 1100×1000.
- Status bar: bottom-right, 12px, top-left radius 8.
- Dividers: 1px, 3px accent on hover.
- File tree: 13px, padding 4 8 4 24, indent 12, selection = accent at 15%.

**Elements.**
- Links: text-accent, underlined. Unresolved links: opacity .7, underline accent at 30%.
- Tags: .875em, accent background at 10% (hover 20%), padding .25em .65em, radius 2em.
- Code: bg primary-alt, .875em, radius 4, inline padding .15em .3em, block padding 12/16.
- Code tokens: comment faint, function yellow, keyword pink, operator red, property cyan, string green, tag red, value purple, punctuation muted.
- Quote: 2px interactive-accent bar plus 24px padding. HR: 2px, border color.
- Checkbox: text size, radius 4, fill interactive-accent.
- Lists: indent 2.25em, bullet .3em faint.
- Embeds: 2px accent bar, 24px padding. Table cells: padding 4 8, 1px border.
- Callouts: background = callout color at 10%, radius 4, padding 12 12 12 24, bold colored title, 16px icon.
- Callout colors: note, info and todo blue; tip, important and summary cyan; success green; question and warning orange; fail, error and bug red; example purple; quote #9e9e9e.
- Callout icons: note pencil · summary clipboard-list · info info · todo check-circle-2 · tip/important flame · success check · question help-circle · warning alert-triangle · fail x · error zap · bug bug · example list · quote quote.
- Callout aliases: summary = abstract/tldr, tip = hint, success = check/done, question = help/faq, warning = caution/attention, fail = failure/missing, error = danger, quote = cite.

**Graph.**
- Colors: node text-muted; unresolved faint at .5; focused accent; tag green; attachment yellow; line base-35; arrow text-normal at .5.
- Physics:
  - forceX/Y with center = (0.01^(1−v) − 0.01)/0.99
  - link: distance linkDistance, strength (mapped linkStrength) / min(degree)
  - many-body: −repel³, distanceMin 30, θ .9
  - collide: r 60, strength .5
  - velocity ×0.6 per tick; alphaDecay 1−0.001^(1/300); alphaMin .001; 60 Hz
- Rendering:
  - node radius = mult·clamp(3√(w+1), 8, 30)
  - label size 14 + r/4
  - label alpha = clamp(log2(scale) + 1 − fade, 0, 1)
  - line width = mult/scale
  - wheel zoom 1.5^(−dy/120)
- User's `graph.json`: center .5187, repel 10, link 1, distance 250, scale .544, showOrphans on.

**Default hotkeys (Mod = Ctrl).**

| Keys | Command |
|---|---|
| Mod+P | command-palette:open |
| Mod+O | switcher:open |
| Mod+Shift+F | global-search:open |
| Mod+G | graph:open |
| Mod+, | app:open-settings |
| Mod+Alt+←/→ | app:go-back / go-forward |
| Mod+E | markdown:toggle-preview |
| Mod+; | markdown:add-metadata-property |
| Mod+N / Mod+Shift+N | file-explorer:new-file / new-file-in-new-pane |
| Mod+T / Mod+W / Mod+Shift+T / Mod+Shift+W | workspace:new-tab / close / undo-close-pane / close-window |
| Ctrl+Tab, Ctrl+PgDn / Ctrl+Shift+Tab, Ctrl+PgUp | workspace:next-tab / previous-tab |
| Mod+1–8 / Mod+9 | workspace:goto-tab-N / goto-last-tab |
| F2 | workspace:edit-file-title |
| Mod+S | editor:save-file |
| Alt+Enter / Mod+Enter / Mod+Alt+Enter | editor:follow-link / open-link-in-new-leaf / open-link-in-new-split |
| Mod+F / Mod+H | editor:open-search / open-search-replace |
| Mod+K | editor:insert-link |
| Mod+B / Mod+I | editor:toggle-bold / toggle-italics |
| Mod+/ | editor:toggle-comments |
| Mod+L | editor:toggle-checklist-status |
| Mod+D | editor:delete-paragraph |

- Unmodified keys, not commands: Alt+↑/↓ move line; Shift+Alt+↑/↓ copy line; Mod+Alt+↑/↓ add cursor; Mod+[ ] outdent/indent; Ctrl+Shift+[ ] fold/unfold; Ctrl+Alt+[ ] fold/unfold all; Shift+Mod+K delete line; Ctrl+Space autocomplete; F3/Shift+F3 in find.
- The full list of about 200 command ids is recorded in `resources/commands.json`.

**App defaults.**
- Editing: livePreview true; defaultViewMode source; propertiesInDocument visible; showInlineTitle; readableLineLength; strictLineBreaks false; showIndentGuide; foldHeading/foldIndent; spellcheck true; vimMode false.
- Indentation and pairing: useTab true; tabSize 4; autoPairBrackets/Markdown; smartIndentList; autoConvertHtml.
- Files and links: alwaysUpdateLinks **true (user)**; newLinkFormat shortest; useMarkdownLinks false; newFileLocation root; attachmentFolderPath "/"; promptDelete; trashOption system (local = .trash, none).
- PDF export: pageSize Letter, margin 0, downscale 100, includeName.
- Templates: `{{title}} {{date}} {{time}} {{date:FMT}}`, defaults YYYY-MM-DD / HH:mm.
- Unique note: YYYYMMDDHHmm. File recovery: 5 min interval, keep 7 days.
