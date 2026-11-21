"""AU radio station table: generator --check, rejected inputs, and host tests on a fixture table."""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GEN = ROOT / 'tools/generate_radio_table.py'
HEADER = 'area_id,area_name,state,lat,lon,band,freq,dab_block,name,query'
TESTVILLE = 'A1,Testville,TST,-33.8,151.2'
FIXTURE_ROWS = (ROOT / 'tests/fixtures/au_radio_fixture.csv').read_text(encoding='utf-8').splitlines()[1:]
BULK = [f'A3,Bulk,BK,-27.5,153.0,FM,{870 + i},,Bulk FM {i:03d},Bulk FM {i:03d}' for i in range(100)]

# The committed source is header-only until the ACMA rows are added; the generated table must match it.
subprocess.run([sys.executable, str(GEN), '--check'], cwd=ROOT, check=True)
real = (ROOT / 'main/apps/radio/au_radio_table_data.inc').read_text(encoding='utf-8')
assert 'kAreaCount = 0;' in real and 'kStationCount = 0;' in real
assert (ROOT / 'third_party/au-radio/stations.csv').read_text(encoding='utf-8').strip() == HEADER

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
        'dab block': f'{TESTVILLE},DAB,0,,Bad,Bad',
        'outside AU': 'A9,Nowhere,XX,51.5,-0.1,FM,1000,,Bad,Bad',
        'empty name': f'{TESTVILLE},FM,1000,,,Bad',
        'duplicate': f'{TESTVILLE},FM,1000,,Test FM 1,Test FM 1',
        'area mismatch': 'A1,Other,TST,-33.8,151.2,FM,1020,,Other FM,Other FM',
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
