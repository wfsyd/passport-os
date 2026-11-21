"""Validate the full offline payload and every city glyph, without the Internet."""
import gzip
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
subprocess.run([sys.executable, 'tools/generate_city_catalog.py', '--check'], cwd=ROOT, check=True)
raw = gzip.decompress((ROOT / 'main/apps/home/cities.json.gz').read_bytes())
rows = json.loads(raw)
assert rows == json.loads((ROOT / 'main/apps/home/cities.json').read_text('utf-8'))
assert len({r[0] for r in rows}) == len(rows)
assert len({r[4] for r in rows}) == 42  # 34 Chinese provinces plus 8 Australian states and territories
assert {'CN', 'HK', 'MO', 'TW', 'AU'} == {r[6] for r in rows}
au = [r for r in rows if r[6] == 'AU']
assert len(au) == 275, len(au)
assert all(re.fullmatch(r'AU(ACT|NSW|NT|QLD|SA|TAS|VIC|WA)\d{3}', r[0]) for r in au)
assert all(-44 <= r[2] <= -10 and 112 <= r[3] <= 154 for r in au)
assert all(r[1].isascii() and r[4].isascii() and r[5] == r[1] for r in au)
capitals = {'Canberra','Sydney','Darwin','Brisbane','Adelaide','Hobart','Melbourne','Perth'}
assert capitals <= {r[1] for r in au}
# Picker order: NSW, VIC, QLD, WA, SA, TAS, ACT, NT; each state's capital first, then A to Z.
au_states = ['NSW', 'VIC', 'QLD', 'WA', 'SA', 'TAS', 'ACT', 'NT']
au_capitals = dict(zip(au_states, ['Sydney', 'Melbourne', 'Brisbane', 'Perth',
                                   'Adelaide', 'Hobart', 'Canberra', 'Darwin']))
assert [r[0][2:-3] for r in au] == sorted((r[0][2:-3] for r in au), key=au_states.index)
for state in au_states:
    names = [r[1] for r in au if r[0][2:-3] == state]
    assert names[0] == au_capitals[state], (state, names[0])
    assert names[1:] == sorted(names[1:], key=str.casefold), state
assert [r[0] for r in au][:2] == ['AUNSW001', 'AUNSW002'] and au[0][1] == 'Sydney'
for key, maximum in ((1,47),(4,31),(5,47)):
    assert all(len(r[key].encode('utf-8')) <= maximum for r in rows)
assert all(-90 <= r[2] <= 90 and -180 <= r[3] <= 180 for r in rows)
glyphs = {int(c,16) for c in re.findall(r'/\* U\+([0-9A-Fa-f]{4,6})',
                                     (ROOT/'main/fonts/buddy_font_16.c').read_text('utf-8'))}
missing = {c for r in rows for c in r[1]+r[4]+r[5] if ord(c) not in glyphs}
assert not missing, f'missing city glyphs: {missing}'
zhuhai = next(r for r in rows if r[1]=='珠海' and r[4]=='广东')
assert zhuhai[2:4] == [22.2707,113.5767] and zhuhai[5]=='珠海'
chongqing = next(r for r in rows if r[1]=='重庆')
assert chongqing[2:4] == [29.563,106.5516]
print(f'Offline catalog: {len(rows)} places, all coordinates/ids/profile bounds and city glyphs: PASS')
