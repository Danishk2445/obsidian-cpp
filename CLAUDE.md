# obsidian-cpp — working notes for Claude

Native C++23 / Qt 6.11 clone of Obsidian. Plan: `/home/danish/.claude/plans/make-a-clone-of-crispy-truffle.md`
(milestones M0–M9). Reference values (theme tokens, hotkeys, graph physics…): `docs/obsidian-reference.md`.

## Build & test
```
cmake --preset dev && cmake --build --preset dev && ctest --preset dev     # Debug + ASan/UBSan
cmake --preset rel && cmake --build --preset rel                            # RelWithDebInfo (screenshots, perf)
ctest --preset perf                                                          # perf gates (label "perf")
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan    # TSan, label "threads" (tools/tsan.supp)
```
Tests run offscreen (`QT_QPA_PLATFORM=offscreen`). GUI gtests use `oc_add_test(... GUI)`.

## Headless runs / screenshots (always isolate XDG dirs!)
```
S=/tmp/claude-1000/.../scratchpad   # session scratchpad
XDG_CONFIG_HOME=$S/xdg/config XDG_DATA_HOME=$S/xdg/data XDG_CACHE_HOME=$S/xdg/cache \
QT_QPA_PLATFORM=offscreen build/rel/obsidian-cpp --vault <copy-of-vault> --no-register --deterministic \
  --theme dark --size 1400x850 [--open note.md] [--mode live|source|reading] --screenshot $S/shot.png
```
Synthetic perf vault: `tools/gen_vault.py --out $S/vaults/synthetic` (giant 5.5 MB note, 14k-char lines, fences, lists…).

## Invariants (do not break)
- NEVER open, write or test against the user's real vault (`/run/media/danish/New Volume/stuff/obsidian/obsidian-notes`).
  Use `tests/fixtures/*` or a scratch copy. Never touch `<vault>/.obsidian/` (config is fully separate).
- Clone state lives in `~/.config/obsidian-cpp`, `~/.local/share/obsidian-cpp`, `~/.cache/obsidian-cpp` (per vault id);
  nothing new is ever written inside a vault except user content (notes, attachments, .trash).
- Byte-exact saves: never rewrite a file that wasn't edited; preserve EOLs/BOM/final newline/trailing spaces/tabs.
- No Obsidian code/assets copied; only behaviour + design-token values (resources/themes) are mirrored.
- Frozen interfaces (docs/INTERFACES.md) change only deliberately, with all users updated.
- Local git only; never push.

## Layout
`src/core` (vault, config, metadata, search, commands) · `src/text` (Text/ChangeSet/History/codec) ·
`src/markdown` (parser) · `src/render` (theme, layout engine, widgets) · `src/editor` (EditorView, live preview) ·
`src/ui` (shell, workspace, views, modals, plugins) · `src/app` (main, CLI) · `third_party/jkqtplotter` (LGPL math, shared).
