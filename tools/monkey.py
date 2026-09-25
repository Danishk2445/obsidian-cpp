#!/usr/bin/env python3
"""Random-input smoke test: drives a headless obsidian-cpp (ideally the ASan/UBSan dev build) with random opens,
typing, keys and commands on a scratch copy of a fixture vault, and reports crashes, sanitizer reports and hangs.

  tools/monkey.py --bin build/dev/obsidian-cpp --work $S/monkey --runs 20 --steps 300 [--seed N]

Each run gets a fresh vault copy and fresh XDG dirs under --work; failing runs keep their script and log there.
"""
import argparse, os, random, re, shutil, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FIXTURE = ROOT / "tests/fixtures/vault-basic"

# Commands that end the session or wait on a real desktop.
EXCLUDE = {"app:quit", "workspace:close-window", "app:reload", "app:open-sandbox-vault", "app:open-help",
           "workspace:export-pdf"}

KEYS = ["Enter", "Backspace", "Delete", "Tab", "Shift+Tab", "Escape", "ArrowUp", "ArrowDown", "ArrowLeft",
        "ArrowRight", "Shift+ArrowLeft", "Shift+ArrowRight", "Shift+ArrowDown", "Home", "End", "PageUp", "PageDown",
        "Mod+Z", "Mod+Shift+Z", "Mod+A", "Mod+C", "Mod+X", "Mod+V", "Mod+E", "Mod+B", "Mod+I", "Mod+K", "Mod+L",
        "Mod+D", "Mod+/", "Mod+]", "Mod+[", "Alt+ArrowUp", "Alt+ArrowDown", "Shift+Alt+ArrowDown",
        "Mod+Alt+ArrowDown", "Mod+Shift+K", "Mod+P", "Mod+O", "Mod+F", "Mod+H", "Mod+T", "Mod+W", "Mod+Shift+T",
        "Ctrl+Tab", "Mod+Alt+ArrowLeft", "Mod+Alt+ArrowRight", "Mod+;", "Ctrl+Space", "F2", "F3", "Mod+Enter",
        "Alt+Enter", "Mod+Shift+F", "Mod+G", "Mod+N", "Mod+,", "Mod+S"]

SNIPPETS = ["hello ", "[[", "]]", "![[", "**", "*", "==", "~~", "`", "```\n", "$", "$$", "#", "#tag ", "> ", ">green",
            "- ", "- [ ] ", "1. ", "\t", "\n", "\n\n", "|a|b|\n|-|-|\n", "<br>", "<kbd>x</kbd>", "%%", "[^1]", "^blk",
            "---\n", "https://example.com ", "日本語 ", "🙂", "\u00a0", "[x](y) ", "\\", "{{", "(", ")", "\"", "'"]


def commands():
    ids = set()
    for p in (ROOT / "src").rglob("*.cpp"):
        for m in re.finditer(r'"([a-z][a-z-]*:[a-z0-9-]+)"', p.read_text(errors="replace")):
            ids.add(m.group(1))
    import json
    ids |= {c["id"] for c in json.loads((ROOT / "resources/commands.json").read_text())}
    ids |= {f"editor:set-heading-{n}" for n in range(7)} | {f"workspace:goto-tab-{n}" for n in range(1, 9)}
    return sorted(i for i in ids if i not in EXCLUDE and not i.endswith("-") and not i.startswith(("obsidian:", "http")))


def script(rng, notes, cmds, steps):
    out = [f"open {rng.choice(notes)} live"]
    for _ in range(steps):
        r = rng.random()
        if r < 0.10:
            out.append(f"try open {rng.choice(notes)} {rng.choice(['live', 'source', 'reading', ''])}".rstrip())
        elif r < 0.25:
            out.append(f"try cursor {rng.randint(0, 60)} {rng.choice(['0', 'end', str(rng.randint(0, 40))])}")
        elif r < 0.50:
            text = "".join(rng.choice(SNIPPETS) for _ in range(rng.randint(1, 4)))
            out.append("try type " + text.replace("\\", "\\\\").replace("\n", "\\n").replace("\t", "\\t"))
        elif r < 0.78:
            out.append(f"try key {rng.choice(KEYS)}")
        elif r < 0.97:
            out.append(f"try command {rng.choice(cmds)}")
        else:
            out.append(f"wait {rng.randint(10, 400)}")
    out.append("quit")
    return "\n".join(out) + "\n"


SAN = re.compile(r"(ERROR: AddressSanitizer|runtime error:|ERROR: LeakSanitizer|SUMMARY: \w+Sanitizer|"
                 r"Segmentation fault|ASSERT failure|QFATAL|terminate called)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default=str(ROOT / "build/dev/obsidian-cpp"))
    ap.add_argument("--work", required=True)
    ap.add_argument("--runs", type=int, default=10)
    ap.add_argument("--steps", type=int, default=250)
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--timeout", type=int, default=240)
    ap.add_argument("--vault", default=str(FIXTURE), help="vault to copy for each run (never the real one)")
    a = ap.parse_args()
    fixture = Path(a.vault).resolve()
    if str(fixture).startswith("/run/media/"):  # never the user's real vault (removable drive), only copies
        sys.exit("refusing to use a vault on removable media; pass a scratch copy")
    work = Path(a.work)
    work.mkdir(parents=True, exist_ok=True)
    notes = sorted(str(p.relative_to(fixture)) for p in fixture.rglob("*.md")
                   if not any(part.startswith(".") for part in p.relative_to(fixture).parts))
    cmds = commands()
    base_seed = a.seed if a.seed is not None else random.randrange(1 << 30)
    failures = 0
    for i in range(a.runs):
        seed = base_seed + i
        rng = random.Random(seed)
        d = work / f"run-{seed}"
        shutil.rmtree(d, ignore_errors=True)
        shutil.copytree(fixture, d / "vault", ignore=shutil.ignore_patterns(".git"))
        (d / "script.txt").write_text(script(rng, notes, cmds, a.steps))
        env = dict(os.environ, QT_QPA_PLATFORM="offscreen", QT_FORCE_STDERR_LOGGING="1", OC_SCRIPT_TRACE="1",
                   XDG_CONFIG_HOME=str(d / "xdg/config"), XDG_DATA_HOME=str(d / "xdg/data"),
                   XDG_CACHE_HOME=str(d / "xdg/cache"),
                   ASAN_OPTIONS="detect_leaks=0:abort_on_error=0", UBSAN_OPTIONS="print_stacktrace=1")
        cmd = [a.bin, "--vault", str(d / "vault"), "--no-register", "--deterministic", "--size", "1200x800",
               "--script", str(d / "script.txt")]
        try:
            p = subprocess.run(cmd, env=env, capture_output=True, text=True, errors="replace", timeout=a.timeout)
            log, code = p.stdout + p.stderr, p.returncode
        except subprocess.TimeoutExpired as e:
            log, code = (e.stdout or b"").decode(errors="replace") + (e.stderr or b"").decode(errors="replace"), "hang"
            last = [l for l in log.splitlines() if l.startswith("oc-script ")][-1:]
            print(f"   hung after: {last[0] if last else '?'}")
        (d / "log.txt").write_text(log)
        bad = code != 0 or SAN.search(log)
        print(f"seed {seed}: exit {code}" + ("  <-- FAIL" if bad else ""), flush=True)
        if bad:
            failures += 1
            m = SAN.search(log)
            if m:
                s = log[max(0, m.start() - 200):m.start() + 1500]
                print("   " + s.replace("\n", "\n   "))
        else:
            shutil.rmtree(d, ignore_errors=True)
    print(f"{failures} failing run(s) of {a.runs}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
