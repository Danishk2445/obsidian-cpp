#!/usr/bin/env python3
"""Build resources/themes/obsidian-default.json from a dump of Obsidian's default CSS custom properties.

Input: text dump with '===  <selector> <n>' headers followed by '--name: value' lines (produced by reading
the installed Obsidian's app.css in-memory; only custom-property *values* are kept — design tokens).
Output JSON: {"body": {...}, "theme-light": {...}, "theme-dark": {...}} — our ThemeManager resolves
var()/calc()/hsl()/color-mix() over these.
"""
import json, re, sys

SKIP = re.compile(r'^--(safe-area|mobile|dropdown-background|keyboard|preferred-text-scale|font-default|font-monospace-default|'
                  r'.*-mask$|.*-filter$|pdf-)')
cur = None
out = {'body': {}, 'theme-light': {}, 'theme-dark': {}}
for line in open(sys.argv[1], encoding='utf-8'):
    line = line.rstrip('\n')
    m = re.match(r'^===\s+(\S.*?)\s+\d+$', line)
    if m:
        sel = m.group(1)
        cur = {'body': 'body', '.theme-light': 'theme-light', '.theme-dark': 'theme-dark'}.get(sel)
        continue
    if cur is None or not line.startswith('--'):
        continue
    name, _, value = line.partition(':')
    name, value = name.strip(), value.strip()
    if SKIP.match(name) or 'url(' in value or '!important' in value:
        continue
    # first definition wins per selector block (later same-selector blocks are media/overrides)
    out[cur].setdefault(name, value)
# Heading weights live in :root upstream (static-font variant); add them to body.
for i, w in enumerate(['700', '600', '600', '600', '600', '600'], 1):
    out['body'].setdefault(f'--h{i}-weight', w)
json.dump(out, open(sys.argv[2], 'w', encoding='utf-8'), indent=1, ensure_ascii=False)
print({k: len(v) for k, v in out.items()})
