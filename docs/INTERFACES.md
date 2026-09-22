# Frozen interfaces

Interfaces other modules/agents build against. Change deliberately and update all users.

## M0
- `oc::ClonePaths` (src/core/config/clone_paths.h): XDG-based clone dirs; per-vault config/data/cache dirs.
- `oc::ConfigStore` (src/core/config/config_store.h): per-vault Obsidian-shaped JSON files, cached, debounced writes.
- `oc::VaultRegistry` (src/core/config/vault_registry.h): `obsidian-cpp.json` vault list + global options.
- `oc::Theme` / `oc::ThemeManager` / `oc::currentTheme()` (src/render/theme/theme.h): token access by Obsidian CSS
  variable name (`color()`, `px()`, `number()`, fonts); `themeChanged()` signal; `generation()` for cache keys.
- `oc::IconProvider` (src/render/theme/icons.h): Lucide/Obsidian icon names → pixmaps/icons.
- `oc::Busy` (src/core/util/busy.h): in-flight work counter for headless idle detection.

(M1 adds oc_text, SyntaxTree, Vault, Document, View/Workspace, Command/Hotkey, Editor facade.)

## M1 (frozen headers — read them, they document the contracts)
- `src/text/*` — `Text` (immutable chunked lines, per-line EOL, CM6 '\n'-joined offsets), `codec` (byte-exact
  UTF-8 with surrogate escapes, `contentHash`), `ChangeSet`/`ChangeDesc` (CodeMirror 6 semantics),
  `Selection`/`SelRange`, `Transaction` (userEvent strings), `History` (CM6 grouping/mapping), `diff`/`merge3`.
- `src/markdown/syntax_tree.h` — `oc::md::SyntaxTree`: incremental block parser API (line roles, container
  markers, preorder block nodes, per-type info, budgets/resume, dump/equivalent).
- `src/core/vault/vault.h` — `Vault`, `TFile`/`TFolder`, `DiskState`, `DocumentStorage` (CAS writes), signals
  `created/modified/deleted/renamed(…, bool external)`, `onlineChanged`, trash modes, ignore rules.
- `src/ui/workspace/{view,view_registry,workspace}.h` — `View`/`FileView`, `ViewRegistry`, `Workspace`/
  `WorkspaceLeaf` (Obsidian getLeaf/openFile/setViewState semantics, workspace.json shape).
- `src/core/vault_session.h` — per-vault service bundle (`vault()`, `documents()`, `commands()`, `workspace()`,
  `viewRegistry()`, `config()`), i.e. Obsidian's `app`.

## M2 (frozen)
- `src/markdown/inline_tree.h` — `InlineTree` (preorder nodes in content offsets, Mark children for hideable
  syntax, per-type infos), `LeafContent` (content ↔ document mapping), token precedence rules (header comment).
- `SyntaxTree::leafContent / tableCellContent / inlineTree(...)` — lazy, cached inline parsing per leaf.
