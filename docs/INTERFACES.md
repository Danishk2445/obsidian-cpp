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
