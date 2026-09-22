#!/usr/bin/env python3
"""Generate synthetic vaults shaped like the user's real vault (aggregate survey), for perf tests.

  tools/gen_vault.py --out DIR [--seed 1] [--notes 247] [--graph-notes 0]

Produces:
  giant.md        ~5.5 MB, ~64k lines, some lines up to 14k chars, lists/headings/bold, no code
  fences.md       ~1.4 MB with ~1300 fenced blocks (mostly no language; bash/c/ini/sh)
  lists.md        ~13.9k tab-indented list items, depth <= 4
  headings.md     ~300 headings
  tables.md       ~286 GFM tables
  notes/...       N ordinary notes in nested folders with wikilinks (20 duplicated basenames),
                  bash `[[ -f x ]]` lines outside fences, `$VAR` shell text, tags, inline code
  graph/...       optional: M more notes with Zipf-distributed links (graph perf)
"""
import argparse, os, random

WORDS = ("the of and to in is you that it he was for on are as with his they at be this have from or one had by "
         "word but not what all were we when your can said there use an each which she do how their if will up other "
         "about out many then them these so some her would make like him into time has look two more write go see number "
         "no way could people my than first water been call who oil its now find long down day did get come made may part "
         "kernel config mount partition package driver network server client module service script").split()

def sentence(rng, n=None):
    n = n or rng.randint(6, 18)
    s = ' '.join(rng.choice(WORDS) for _ in range(n))
    return s[0].upper() + s[1:] + '.'

def paragraph(rng, chars):
    out = []
    size = 0
    while size < chars:
        s = sentence(rng)
        if rng.random() < 0.08:
            w = rng.choice(WORDS); s = s.replace(w, f'**{w}**', 1)
        if rng.random() < 0.05:
            w = rng.choice(WORDS); s = s.replace(w, f'`{w}`', 1)
        out.append(s); size += len(s) + 1
    return ' '.join(out)[:chars]

def list_block(rng, items, max_depth=4):
    lines, depth = [], 0
    for _ in range(items):
        depth = max(0, min(max_depth - 1, depth + rng.choice((-1, 0, 0, 1))))
        marker = '-' if rng.random() < 0.9 else f'{rng.randint(1, 9)}.'
        lines.append('\t' * depth + f'{marker} ' + sentence(rng, rng.randint(3, 12)))
    return lines

def write(path, lines, final_newline=True):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + ('\n' if final_newline else ''))

def gen_giant(rng, path, target_bytes=5_500_000, target_lines=64_000):
    lines, size = [], 0
    while size < target_bytes and len(lines) < target_lines * 2:
        r = rng.random()
        if r < 0.02:
            block = [f'# {sentence(rng, 4)[:-1]}', '']
        elif r < 0.06:
            block = [f'## {sentence(rng, 3)[:-1]}', '']
        elif r < 0.30:
            block = list_block(rng, rng.randint(3, 20)) + ['']
        elif r < 0.305:
            block = [paragraph(rng, rng.randint(7000, 14266)), '']   # the very long lines
        else:
            block = [paragraph(rng, rng.randint(20, 300))]
        for l in block:
            lines.append(l); size += len(l.encode()) + 1
    write(path, lines, final_newline=False)

def gen_fences(rng, path, blocks=1303):
    langs = [''] * 96 + ['bash', 'bash', 'c', 'ini', 'sh']
    lines = []
    for i in range(blocks):
        lines += [f'### Step {i}', sentence(rng), '']
        lang = rng.choice(langs)
        lines.append('```' + lang)
        for _ in range(rng.randint(5, 70)):
            if lang in ('bash', 'sh', ''):
                lines.append(rng.choice(['sudo pacman -S ', 'echo $HOME ', 'ls -la /', 'if [[ -f x ]]; then ', 'cat /etc/']) + rng.choice(WORDS))
            elif lang == 'c':
                lines.append(f'int {rng.choice(WORDS)} = {rng.randint(0, 99)}; // {rng.choice(WORDS)}')
            else:
                lines.append(f'{rng.choice(WORDS)} = {rng.choice(WORDS)}')
        lines += ['```', '']
    write(path, lines)

def gen_tables(rng, path, tables=286):
    lines = []
    for t in range(tables):
        cols = rng.randint(2, 5)
        lines.append('| ' + ' | '.join(rng.choice(WORDS) for _ in range(cols)) + ' |')
        lines.append('| ' + ' | '.join(rng.choice([':---', '---', ':---:', '---:']) for _ in range(cols)) + ' |')
        for _ in range(rng.randint(2, 8)):
            lines.append('| ' + ' | '.join(sentence(rng, rng.randint(1, 4)) for _ in range(cols)) + ' |')
        lines.append('')
    write(path, lines)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--notes', type=int, default=247)
    ap.add_argument('--graph-notes', type=int, default=0)
    a = ap.parse_args()
    rng = random.Random(a.seed)
    out = a.out
    gen_giant(rng, os.path.join(out, 'giant.md'))
    gen_fences(rng, os.path.join(out, 'fences.md'))
    write(os.path.join(out, 'lists.md'), list_block(rng, 13_900))
    hl = []
    for i in range(300):
        hl += [f'{"#" * rng.choice((1, 1, 2, 2, 3))} Heading {i}', paragraph(rng, rng.randint(50, 400)), '']
    write(os.path.join(out, 'headings.md'), hl)
    gen_tables(rng, os.path.join(out, 'tables.md'))

    folders = ['notes/' + '/'.join(rng.choice(WORDS) for _ in range(rng.randint(1, 5))) for _ in range(40)]
    dup_names = [f'shared {w}' for w in rng.sample(WORDS, 20)]
    names = []
    for i in range(a.notes):
        name = rng.choice(dup_names) if i < 68 else f'note {i} {rng.choice(WORDS)}'
        names.append((rng.choice(folders), name))
    for folder, name in names:
        lines = [f'# {name}', '']
        for _ in range(rng.randint(3, 40)):
            r = rng.random()
            if r < 0.1:
                lines.append(f'See [[{rng.choice(names)[1]}]] and [[{rng.choice(names)[1]}|alias]].')
            elif r < 0.13:
                lines.append('if [[ -f /etc/hosts ]]; then echo $HOME; fi')
            elif r < 0.16:
                lines.append(f'#{rng.choice(WORDS)} #{rng.choice(WORDS)}/{rng.choice(WORDS)}')
            elif r < 0.4:
                lines += list_block(rng, rng.randint(2, 10))
            else:
                lines.append(paragraph(rng, rng.randint(40, 500)))
        write(os.path.join(out, folder, name + '.md'), lines, final_newline=rng.random() < 0.7)

    if a.graph_notes:
        n = a.graph_notes
        for i in range(n):
            links = set()
            for _ in range(rng.randint(0, 6)):
                j = min(n - 1, int(rng.paretovariate(1.2)) - 1)
                links.add(j if rng.random() < 0.5 else rng.randrange(n))
            body = [f'# G{i}', ''] + [f'- [[G{j}]]' for j in sorted(links) if j != i]
            write(os.path.join(out, 'graph', f'd{i % 50}', f'G{i}.md'), body)
    print('generated vault at', out)

if __name__ == '__main__':
    main()
