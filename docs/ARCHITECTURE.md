# Architecture (living document)

Libraries (static unless noted), lowest first:

| lib | depends on | contents |
|---|---|---|
| oc_text | QtCore | immutable chunked `Text`, byte-exact codec, `ChangeSet`, `Selection`, `Transaction`, `History`, diff/diff3 |
| oc_markdown | oc_text | resumable block parser with incremental resync, inline parser + cache, Obsidian rules, frontmatter, metadata extraction |
| oc_core | oc_text, oc_markdown | config (ClonePaths, ConfigStore, VaultRegistry), vault model + fs watcher + trash, documents, metadata cache, link resolver/updater, search, commands/hotkeys |
| oc_render | oc_core, QtGui/Svg, KSyntaxHighlighting, JKQTMathText (shared) | theme tokens + CSS evaluator, icons, layout engine (DisplayBlock, DocLayout, height map), block/inline widgets, math/code/image services |
| oc_editor | oc_render | EditorView (QAbstractScrollArea), live preview decorations + reveal policy, input/IME, markdown commands, hooks, vim |
| oc_ui | oc_editor | main window shell, workspace (splits/tabs/leaves), views, modals, overlays, core plugins |
| obsidian-cpp | oc_ui | executable: CLI, startup, headless screenshot/script driver |

Threading: GUI thread owns widgets and documents; `Text` snapshots are immutable and shareable; indexing,
search and background layout measurement run on QThreadPool; the graph simulation has its own thread.
