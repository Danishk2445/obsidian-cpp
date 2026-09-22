#!/usr/bin/env python3
"""Pack lucide-static SVG icons into one compact JSON map: name -> inner SVG markup.

All Lucide icons share the same <svg> wrapper (24x24 viewBox, stroke=currentColor, stroke-width 2,
round caps/joins), so only the child elements are stored. IconProvider re-wraps them at runtime with
the requested stroke color and width (Obsidian draws icons with stroke-width 1.75).

usage: tools/pack_icons.py <lucide-static/icons dir> resources/icons/lucide.json
"""
import json, os, re, sys

src, dst = sys.argv[1], sys.argv[2]
out = {}
for fn in sorted(os.listdir(src)):
    if not fn.endswith('.svg'):
        continue
    text = open(os.path.join(src, fn), encoding='utf-8').read()
    text = re.sub(r'<!--.*?-->', '', text, flags=re.S)
    m = re.search(r'<svg[^>]*>(.*)</svg>', text, flags=re.S)
    if not m:
        print('skip', fn, file=sys.stderr)
        continue
    body = m.group(1)
    body = re.sub(r'\s*\n\s*', ' ', body).strip()
    body = re.sub(r'\s*/>', '/>', body)
    body = re.sub(r'>\s+<', '><', body)
    out[fn[:-4]] = body
with open(dst, 'w', encoding='utf-8') as f:
    json.dump(out, f, separators=(',', ':'), ensure_ascii=False, sort_keys=True)
print(f'{len(out)} icons -> {dst} ({os.path.getsize(dst)} bytes)')
