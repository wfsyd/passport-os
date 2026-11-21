"""AU radio station table: generator --check, rejected inputs, and host tests on a fixture table."""
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GEN = ROOT / 'tools/generate_radio_table.py'
HEADER = 'area_id,area_name,state,lat,lon,band,freq,dab_block,name,query'
TESTVILLE = 'A1,Testville,NSW,-33.8,151.2'
FIXTURE_ROWS = (ROOT / 'tests/fixtures/au_radio_fixture.csv').read_text(encoding='utf-8').splitlines()[1:]
BULK = [f'A3,Bulk,QLD,-27.5,153.0,FM,{870 + i},,Bulk FM {i:03d},Bulk FM {i:03d}' for i in range(100)]


def decode_table(text):
    """Read the generated .inc back: areas as (name, state, first, count), stations as (band, freq, name, dab_block)."""
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
    stations = [(int(band), int(freq), string(int(name)), string(int(dab)))
                for name, _, dab, freq, band in re.findall(r'\{(\d+), (\d+), (\d+), (\d+), (\d+)\},', stations_body)]
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
        names = {(band, name) for band, _, name, _ in rows}
        assert (0, 'triple j') in names, 'triple j on FM'
        assert (0, 'ABC Sydney') not in names and (1, 'ABC Sydney') in names, 'ABC Sydney on AM 702'
        assert (0, 'KIIS 106.5') in names, 'commercial FM brand'
        assert (2, 'Double J') in names, 'DAB Double J'
        dab_blocks = {block for band, _, _, block in rows if band == 2}
        assert dab_blocks == {'', '9C'}, 'DAB+ simulcasts have no block, national services are on 9C'
        assert (2, 'KIIS 106.5') in names, 'commercial brand simulcast on DAB+'

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
