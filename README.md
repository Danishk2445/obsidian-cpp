# obsidian-cpp

A native C++23 / Qt 6 clone of [Obsidian](https://obsidian.md): a Markdown knowledge base over a local folder
(a *vault*). It mirrors Obsidian 1.13's behaviour and look — Live Preview / Source / Reading views, links and
backlinks, graph, search, properties, canvas, bases, vim mode, PDF export, PDF/media viewers — without any of its
code. See `docs/PARITY.md` for the feature checklist and known differences.

Your Obsidian setup is never touched: the clone keeps its own configuration and never reads or writes
`<vault>/.obsidian/`. The only files it writes inside a vault are your own content (notes, attachments, `.trash`),
and only when you change them — opening and browsing a vault leaves every byte as it was.

## Requirements (Arch Linux)

```
sudo pacman -S --needed base-devel cmake ninja qt6-base qt6-svg qt6-multimedia qt6-webengine \
    syntax-highlighting sonnet kio yaml-cpp zstd sqlite xxhash gtest hunspell-en_us
```

`qt6-webengine` provides QtPdf (PDF view / export); `hunspell-en_us` is the spellcheck dictionary. JKQtMathText (LaTeX
math, LGPL) is vendored in `third_party/` and built as a shared library.

## Build and run

```
cmake --preset rel && cmake --build --preset rel      # RelWithDebInfo build in build/rel
build/rel/obsidian-cpp                                # vault picker
build/rel/obsidian-cpp ~/notes                        # open a folder as a vault
build/rel/obsidian-cpp ~/notes/Ideas.md               # open a note (hands it to a running instance)
build/rel/obsidian-cpp 'obsidian://open?vault=notes&file=Ideas'
```

`obsidian-cpp --help` lists the options (`--mode`, `--theme`, `--native-frame`, `--new-instance`, ...).

## Install

```
cmake --install build/rel --prefix ~/.local
```

installs `bin/obsidian-cpp`, its private math library (`lib/obsidian-cpp/`), an icon and a `.desktop` entry that
opens Markdown files. It does not claim `obsidian://` links, so the real Obsidian keeps those.

## Where things are stored

| What | Where |
|---|---|
| vault list | `~/.config/obsidian-cpp/obsidian-cpp.json` |
| per-vault settings (Obsidian-shaped `app.json`, `appearance.json`, `hotkeys.json`, `workspace.json`, plugins, ...) | `~/.config/obsidian-cpp/vaults/<id>/` |
| file recovery snapshots, recent commands | `~/.local/share/obsidian-cpp/<id>/` |
| caches | `~/.cache/obsidian-cpp/<id>/` |

## Development

```
cmake --preset dev && cmake --build --preset dev && ctest --preset dev -LE perf   # Debug + ASan/UBSan
cmake --preset perf && cmake --build --preset perf && OC_PERF_ASSERT=1 ctest --preset perf   # perf gates
```

- `CLAUDE.md` — working notes (invariants, headless screenshots with isolated XDG dirs).
- `docs/ARCHITECTURE.md`, `docs/INTERFACES.md` — module layout and the stable interfaces between modules.
- `tools/gen_vault.py` — synthetic vaults for perf work; `tools/monkey.py` — random-input crash hunting on a scratch
  copy of a vault; `build/rel/oc-vault-stats <vault>` — read-only link / index statistics.
- Tests by label: unit and GUI tests (default), `perf` (timing gates), `app` (the binary itself on a fresh fixture
  copy with isolated XDG dirs and an optional seeded vault config — `tests/app/run_app.cmake`).
