"""AU radio station table: generator --check, rejected inputs, and host tests on a fixture table."""
import re
import subprocess
import sys
import tempfile
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GEN = ROOT / 'tools/generate_radio_table.py'
HEADER = 'area_id,area_name,state,lat,lon,band,freq,dab_block,name,query'
TESTVILLE = 'A1,Testville,NSW,-33.8,151.2'
FIXTURE_ROWS = (ROOT / 'tests/fixtures/au_radio_fixture.csv').read_text(encoding='utf-8').splitlines()[1:]
BULK = [f'A3,Bulk,QLD,-27.5,153.0,FM,{870 + i},,Bulk FM {i:03d},Bulk FM {i:03d}' for i in range(100)]


def decode_table(text):
    """Read the generated .inc back: areas as (name, state, first, count), stations as (band, freq, name, dab_block, url)."""
    pool_body = text.split('inline constexpr char kPool[] =', 1)[1].split('    ;', 1)[0]
    pool = b''.join(re.sub(r'\\([0-7]{3})', lambda m: chr(int(m.group(1), 8)), lit).encode('latin-1')
                    for lit in re.findall(r'"((?:[^"\\]|\\[0-7]{3})*)"', pool_body))

    def string(offset):
        return pool[offset:pool.index(b'\0', offset)].decode('utf-8')

    areas_body = text.split('kAreas[] = {', 1)[1].split('};', 1)[0]
    areas = [(string(int(n)), string(int(st)), int(first), int(count))
             for _, _, _, n, st, first, count in
             re.findall(r'\{(-?\d+), (-?\d+), (\d+), (\d+), (\d+), (\d+), (\d+)\},', areas_body)]
    stations_body = text.split('kStations[] = {', 1)[1].split('};', 1)[0]
    stations = [(int(band), int(freq), string(int(name)), string(int(dab)), string(int(url)))
                for name, _, dab, freq, band, url in re.findall(r'\{(\d+), (\d+), (\d+), (\d+), (\d+), (\d+)\},', stations_body)]
    return areas, stations


# The committed CSV is generated from the ACMA workbook by tools/acma_to_stations.py; the generated table must match it.
subprocess.run([sys.executable, str(GEN), '--check'], cwd=ROOT, check=True)
real = (ROOT / 'main/apps/radio/au_radio_table_data.inc').read_text(encoding='utf-8')
source_lines = (ROOT / 'third_party/au-radio/stations.csv').read_text(encoding='utf-8').splitlines()
assert source_lines[0] == HEADER and len(source_lines) > 900
assert f'kAreaCount = {len({line.split(",")[0] for line in source_lines[1:]})};' in real
assert f'kStationCount = {len(source_lines) - 1};' in real
areas, stations = decode_table(real)
assert len(areas) > 400 and len(stations) == len(source_lines) - 1
for city, state in [('Sydney', 'NSW'), ('Melbourne', 'VIC'), ('Brisbane', 'QLD'), ('Adelaide', 'SA'),
                    ('Perth', 'WA'), ('Hobart', 'TAS'), ('Canberra', 'ACT'), ('Darwin', 'NT')]:
    matches = [a for a in areas if a[0] == city and a[1] == state]
    assert len(matches) == 1, city
    _, _, first, count = matches[0]
    rows = stations[first:first + count]
    assert 0 < count <= 96, city
    if city == 'Sydney':
        names = {(band, name) for band, _, name, _, _ in rows}
        assert (0, 'triple j') in names, 'triple j on FM'
        assert (0, 'ABC Sydney') not in names and (1, 'ABC Sydney') in names, 'ABC Sydney on AM 702'
        assert (0, 'KIIS 106.5') in names, 'commercial FM brand'
        assert (2, 'Double J') in names, 'DAB Double J'
        dab_blocks = {block for band, _, _, block, _ in rows if band == 2}
        assert dab_blocks == {'', '9C'}, 'DAB+ simulcasts have no block, national services are on 9C'
        assert (2, 'KIIS 106.5') in names, 'commercial brand simulcast on DAB+'

# DAB+-only areas fold into the FM/AM area with the most stations within 70 km, not the nearest.
# Sydney suburbs (Campbelltown, Windsor, ...) must land in nsw-sydney, never in Nowra or Lithgow.
SYDNEY_SUBURBS = ('Campbelltown', 'Windsor', 'Manly', 'Sutherland', 'Waverley')
area_names = {name for name, _, _, _ in areas}
for suburb in SYDNEY_SUBURBS:
    assert suburb not in area_names, f'{suburb} is a DAB+-only area folded into Sydney'
for name, state in (('Nowra', 'NSW'), ('Lithgow (City)', 'NSW')):
    for a in areas:
        if a[0] == name and a[1] == state:
            rows = stations[a[2]:a[2] + a[3]]
            assert all(band != 2 for band, _, _, _, _ in rows), f'{name} has DAB+ rows folded from Sydney'

FOLD_MODULE = ROOT / 'tools/acma_to_stations.py'
try:
    import importlib.util
    spec = importlib.util.spec_from_file_location('acma_to_stations', FOLD_MODULE)
    acma = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(acma)
except ImportError as error:
    print(f'SKIP fold rule test: {error}')
else:
    head = ['area_id', 'area_name', 'state', 'lat', 'lon', 'band', 'freq', 'dab_block', 'name', 'query']
    def row(area, name, state, lat, lon, band, freq, block, brand):
        return [area, name, state, f'{lat:.4f}', f'{lon:.4f}', band, str(freq), block, brand, brand]
    fold_rows = [
        # Sydney: three FM/AM stations. Nowra: one AM station, nearer to Campbelltown.
        row('nsw-sydney', 'Sydney', 'NSW', -33.82, 151.185, 'FM', 1005, '', 'triple j'),
        row('nsw-sydney', 'Sydney', 'NSW', -33.82, 151.185, 'FM', 1065, '', 'KIIS 106.5'),
        row('nsw-sydney', 'Sydney', 'NSW', -33.82, 151.185, 'AM', 702, '', 'ABC Sydney'),
        row('nsw-nowra', 'Nowra', 'NSW', -34.6181, 150.6975, 'AM', 603, '', 'ABC Radio National'),
        row('nsw-campbelltown', 'Campbelltown', 'NSW', -34.1472, 150.6708, 'DAB', 0, '9C', 'Double J'),
        row('nsw-campbelltown', 'Campbelltown', 'NSW', -34.1472, 150.6708, 'DAB', 0, '9C', 'ABC Jazz'),
    ]
    folded = acma.fold_dab_only_areas(fold_rows, Counter())
    folded_areas = {r[0] for r in folded}
    assert 'nsw-campbelltown' not in folded_areas, 'DAB+-only area folds'
    sydney = [r for r in folded if r[0] == 'nsw-sydney']
    assert ('DAB', 'Double J') in {(r[5], r[8]) for r in sydney}, 'folded rows reach the Sydney area'
    assert not [r for r in folded if r[0] == 'nsw-nowra' and r[5] == 'DAB'], 'nearer area with fewer stations loses'
    # Outside 70 km nothing folds.
    far = [row('wa-far', 'Far', 'WA', -20.0, 120.0, 'DAB', 0, '9C', 'Double J')] + fold_rows[:3]
    assert 'wa-far' in {r[0] for r in acma.fold_dab_only_areas(far, Counter())}
    print('fold rule test: PASS')

with tempfile.TemporaryDirectory(prefix='passport-au-radio-') as tmp:
    tmp = Path(tmp)
    source = tmp / 'fixture.csv'
    source.write_text('\n'.join([HEADER] + FIXTURE_ROWS + BULK) + '\n', encoding='utf-8')
    table = tmp / 'fixture.inc'
    subprocess.run([sys.executable, str(GEN), '--input', str(source), '--output', str(table)],
                   check=True, capture_output=True)
    subprocess.run([sys.executable, str(GEN), '--input', str(source), '--output', str(table), '--check'],
                   check=True, capture_output=True)
    text = table.read_text(encoding='utf-8')
    assert 'kAreaCount = 3;' in text and 'kStationCount = 105;' in text
    assert all(url == '' for _, _, _, _, url in decode_table(text)[1]), 'no streams.csv, no baked URLs'

    # Baked URLs from streams.csv: shared URLs are one pool string; the row gets its offset.
    streams = tmp / 'streams-fixture.csv'
    streams.write_text('name,url,codec,checked\n'
                       'Test FM 1,https://streams.example.test/fm1.mp3,mp3,2026-11-01\n'
                       'Test AM 1,https://streams.example.test/fm1.mp3,mp3,2026-11-01\n'
                       'Test DAB 1,http://streams.example.test/dab.aac,aac,2026-11-01\n', encoding='utf-8')
    baked = tmp / 'baked.inc'
    subprocess.run([sys.executable, str(GEN), '--input', str(source), '--streams', str(streams),
                    '--output', str(baked)], check=True, capture_output=True)
    _, baked_rows = decode_table(baked.read_text(encoding='utf-8'))
    urls = {name: url for _, _, name, _, url in baked_rows}
    assert urls['Test FM 1'] == urls['Test AM 1'] == 'https://streams.example.test/fm1.mp3'
    assert urls['Test DAB 1'] == 'http://streams.example.test/dab.aac' and urls['Test FM 2'] == ''
    assert baked.read_text(encoding='utf-8').count('streams.example.test/fm1.mp3') == 1, 'pool dedupes URLs'
    bad_streams = {
        'unknown station': 'Nope FM,https://streams.example.test/x.mp3,mp3,2026-11-01',
        'not http': 'Test FM 1,ftp://streams.example.test/x.mp3,mp3,2026-11-01',
        'codec': 'Test FM 1,https://streams.example.test/x.mp3,ogg,2026-11-01',
        'date': 'Test FM 1,https://streams.example.test/x.mp3,mp3,2026-13-01',
        'duplicate': 'Test FM 1,https://streams.example.test/x.mp3,mp3,2026-11-01\nTest FM 1,https://streams.example.test/y.mp3,mp3,2026-11-01',
        'quote': 'Test FM 1,https://streams.example.test/"x.mp3,mp3,2026-11-01',
    }
    for label, body in bad_streams.items():
        bad = tmp / 'bad-streams.csv'
        bad.write_text('name,url,codec,checked\n' + body + '\n', encoding='utf-8')
        result = subprocess.run([sys.executable, str(GEN), '--input', str(source), '--streams', str(bad),
                                 '--output', str(tmp / 'x.inc')], capture_output=True, text=True)
        assert result.returncode != 0, label

    # A changed source no longer matches the generated file.
    stale = tmp / 'stale.csv'
    stale.write_text(source.read_text(encoding='utf-8').replace('Test FM 1', 'Test FM 9'), encoding='utf-8')
    assert subprocess.run([sys.executable, str(GEN), '--input', str(stale), '--output', str(table), '--check'],
                          capture_output=True).returncode != 0

    # Invalid rows are rejected.
    bad_rows = {
        'band': f'{TESTVILLE},XYZ,1000,,Bad,Bad',
        'fm range': f'{TESTVILLE},FM,1200,,Bad,Bad',
        'am range': f'{TESTVILLE},AM,9000,,Bad,Bad',
        'dab freq': f'{TESTVILLE},DAB,5,9A,Bad,Bad',
        'dab block too long': f'{TESTVILLE},DAB,0,ABCDEFGHI,Bad,Bad',
        'outside AU': 'A9,Nowhere,XX,51.5,-0.1,FM,1000,,Bad,Bad',
        'empty name': f'{TESTVILLE},FM,1000,,,Bad',
        'duplicate': f'{TESTVILLE},FM,1000,,Test FM 1,Test FM 1',
        'area mismatch': 'A1,Other,NSW,-33.8,151.2,FM,1020,,Other FM,Other FM',
    }
    for label, row in bad_rows.items():
        bad = tmp / 'bad.csv'
        extra = [row] if label not in ('duplicate', 'area mismatch') else [FIXTURE_ROWS[0], row]
        bad.write_text('\n'.join([HEADER] + extra) + '\n', encoding='utf-8')
        result = subprocess.run([sys.executable, str(GEN), '--input', str(bad), '--output', str(tmp / 'x.inc')],
                                capture_output=True, text=True)
        assert result.returncode != 0, label

    exe = tmp / 'au-radio-table'
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-g',
                    '-Imain', f'-DAU_RADIO_DATA_INC="{table}"',
                    str(ROOT / 'tests/test_au_radio_table.cc'), str(ROOT / 'main/apps/radio/au_radio_table.cc'),
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

# The bundled table itself: Sydney GPS, picker order, and no DAB+-only area left beside an FM/AM area.
with tempfile.TemporaryDirectory(prefix='passport-au-radio-real-') as tmp:
    exe = Path(tmp) / 'au-radio-real-table'
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-g',
                    '-Imain', str(ROOT / 'tests/test_au_radio_real_table.cc'),
                    str(ROOT / 'main/apps/radio/au_radio_table.cc'), '-o', str(exe)], check=True, cwd=ROOT)
    subprocess.run([str(exe)], check=True)
