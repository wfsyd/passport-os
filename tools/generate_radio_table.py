#!/usr/bin/env python3
"""Build the flash station table for Australian radio (table mode).

Input: third_party/au-radio/stations.csv (see the README beside it).
Output: main/apps/radio/au_radio_table_data.inc, included by au_radio_table.cc.
Strings go into one NUL-terminated pool; rows refer to them by byte offset.
Stations are grouped by area in first-seen order so each area is a contiguous run.
"""
import argparse
import csv
import datetime
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SOURCE = ROOT / 'third_party/au-radio/stations.csv'
DEFAULT_OUTPUT = ROOT / 'main/apps/radio/au_radio_table_data.inc'
COLUMNS = ['area_id', 'area_name', 'state', 'lat', 'lon', 'band', 'freq', 'dab_block', 'name', 'query']
STREAM_COLUMNS = ['name', 'url', 'codec', 'checked']
STREAM_CODECS = ('mp3', 'aac', 'hls')
BAND_CODES = {'FM': 0, 'AM': 1, 'DAB': 2}
FM_DECIHZ = (870, 1080)   # 87.0 to 108.0 MHz, in 0.1 MHz units
AM_KHZ = (522, 1728)
AU_LAT = (-44.0, -9.0)   # Torres Strait islands lie north of 10 S
AU_LON = (112.0, 154.0)
MAX_NAME_BYTES = 40
MAX_QUERY_BYTES = 40
MAX_URL_BYTES = 255         # RadioStation::url is 256 bytes, with its NUL
MAX_POOL_BYTES = 65535
MAX_ROWS = 65535


def fail(line, message):
    raise ValueError(f'line {line}: {message}')


def load_rows(path):
    with path.open(encoding='utf-8', newline='') as source:
        reader = csv.reader(source)
        if next(reader, None) != COLUMNS:
            raise ValueError(f'{path}: header must be {",".join(COLUMNS)}')
        rows = []
        for line, raw in enumerate(reader, start=2):
            if not any(cell.strip() for cell in raw):
                continue
            if len(raw) != len(COLUMNS):
                fail(line, f'expected {len(COLUMNS)} columns, got {len(raw)}')
            rows.append((line, dict(zip(COLUMNS, (cell.strip() for cell in raw)))))
    return rows


def load_streams(path, station_names):
    """Map station name to baked stream URL from streams.csv. A missing file means none."""
    if not path.exists():
        return {}
    streams = {}
    with path.open(encoding='utf-8', newline='') as source:
        reader = csv.reader(source)
        if next(reader, None) != STREAM_COLUMNS:
            raise ValueError(f'{path}: header must be {",".join(STREAM_COLUMNS)}')
        for line, raw in enumerate(reader, start=2):
            if not any(cell.strip() for cell in raw):
                continue
            if len(raw) != len(STREAM_COLUMNS):
                fail(line, f'{path.name}: expected {len(STREAM_COLUMNS)} columns, got {len(raw)}')
            r = dict(zip(STREAM_COLUMNS, (cell.strip() for cell in raw)))
            check_text(line, 'name', r['name'], MAX_NAME_BYTES)
            check_text(line, 'url', r['url'], MAX_URL_BYTES)
            if not r['url'].startswith(('http://', 'https://')):
                fail(line, 'url must start with http:// or https://')
            if r['codec'] not in STREAM_CODECS:
                fail(line, 'codec must be mp3, aac or hls')
            # 'pattern': a URL built from the broadcaster's known stream pattern, not yet checked.
            if r['checked'] != 'pattern':
                try:
                    datetime.date.fromisoformat(r['checked'])
                except ValueError:
                    fail(line, 'checked must be an ISO date (YYYY-MM-DD) or pattern')
            if r['name'] not in station_names:
                fail(line, f'{r["name"]} is not a station name in stations.csv')
            if r['name'] in streams:
                fail(line, f'duplicate stream for {r["name"]}')
            streams[r['name']] = r['url']
    return streams


def check_text(line, field, value, limit):
    if not value:
        fail(line, f'{field} is empty')
    if len(value.encode('utf-8')) > limit:
        fail(line, f'{field} exceeds {limit} bytes')
    if any(ord(c) < 0x20 or c in '"\\' for c in value):
        fail(line, f'{field} has control, quote or backslash characters')


def build(rows, streams=None):
    """Return (areas, stations, pool) for the validated rows. streams maps name to baked URL."""
    streams = streams or {}
    areas = {}   # area_id -> dict
    order = []   # area ids in first-seen order
    grouped = {}  # area_id -> list of station dicts
    seen = set()
    for line, r in rows:
        check_text(line, 'area_id', r['area_id'], 16)
        check_text(line, 'area_name', r['area_name'], MAX_NAME_BYTES)
        check_text(line, 'state', r['state'], 8)
        check_text(line, 'name', r['name'], MAX_NAME_BYTES)
        check_text(line, 'query', r['query'], MAX_QUERY_BYTES)
        try:
            lat, lon = float(r['lat']), float(r['lon'])
        except ValueError:
            fail(line, 'lat and lon must be decimal degrees')
        if not (AU_LAT[0] <= lat <= AU_LAT[1] and AU_LON[0] <= lon <= AU_LON[1]):
            fail(line, 'coordinates are outside Australia')
        area = {'name': r['area_name'], 'state': r['state'], 'lat': lat, 'lon': lon}
        if r['area_id'] in areas:
            if areas[r['area_id']] != area:
                fail(line, f'area {r["area_id"]} differs from its first row')
        else:
            areas[r['area_id']] = area
            order.append(r['area_id'])
            grouped[r['area_id']] = []
        band = r['band'].upper()
        if band not in BAND_CODES:
            fail(line, 'band must be FM, AM or DAB')
        try:
            freq = int(r['freq'] or '0')
        except ValueError:
            fail(line, 'freq must be an integer')
        if band == 'FM':
            if not FM_DECIHZ[0] <= freq <= FM_DECIHZ[1] or r['dab_block']:
                fail(line, 'FM needs freq in 0.1 MHz within 870-1080 and no dab_block')
        elif band == 'AM':
            if not AM_KHZ[0] <= freq <= AM_KHZ[1] or r['dab_block']:
                fail(line, 'AM needs freq in kHz within 522-1728 and no dab_block')
        else:
            if freq != 0:
                fail(line, 'DAB needs freq 0')
            if r['dab_block']:
                check_text(line, 'dab_block', r['dab_block'], 8)
        key = (r['area_id'], band, r['name'])
        if key in seen:
            fail(line, f'duplicate station {r["name"]} in {r["area_id"]}')
        seen.add(key)
        grouped[r['area_id']].append({'name': r['name'], 'query': r['query'],
                                      'dab_block': r['dab_block'], 'freq': freq, 'band': band,
                                      'url': streams.get(r['name'], '')})
    stations_flat = []
    area_rows = []
    pool = bytearray(b'\0')
    offsets = {'': 0}

    def intern(text):
        if text not in offsets:
            offsets[text] = len(pool)
            pool.extend(text.encode('utf-8') + b'\0')
        return offsets[text]

    for area_id in order:
        first = len(stations_flat)
        for s in grouped[area_id]:
            stations_flat.append((intern(s['name']), intern(s['query']), intern(s['dab_block']),
                                  s['freq'], BAND_CODES[s['band']], intern(s['url'])))
        a = areas[area_id]
        area_rows.append((round(a['lat'] * 10000), round(a['lon'] * 10000), intern(area_id),
                          intern(a['name']), intern(a['state']), first,
                          len(stations_flat) - first))
    if len(pool) > MAX_POOL_BYTES or len(stations_flat) > MAX_ROWS:
        raise ValueError('table exceeds 16-bit offsets')
    return area_rows, stations_flat, bytes(pool)


def c_string(data):
    """Escape bytes as C literal pieces; octal escapes are always three digits."""
    out = []
    for b in data:
        if 0x20 <= b < 0x7f and b not in (0x22, 0x5c, 0x3f):
            out.append(chr(b))
        else:
            out.append(f'\\{b:03o}')
    return ''.join(out)


def render(area_rows, stations, pool):
    lines = [
        '// Generated by tools/generate_radio_table.py from third_party/au-radio/stations.csv.',
        '// Do not edit. Included by main/apps/radio/au_radio_table.cc inside au_radio::data.',
        f'inline constexpr std::size_t kAreaCount = {len(area_rows)};',
        f'inline constexpr std::size_t kStationCount = {len(stations)};',
        'inline constexpr char kPool[] =',
    ]
    for start in range(0, len(pool), 48):
        lines.append(f'    "{c_string(pool[start:start + 48])}"')
    lines.append('    ;')
    lines.append('inline constexpr Area kAreas[] = {')
    for lat, lon, area_id, name, state, first, count in area_rows:
        lines.append(f'    {{{lat}, {lon}, {area_id}, {name}, {state}, {first}, {count}}},')
    lines.append('    {},')
    lines.append('};')
    lines.append('inline constexpr Station kStations[] = {')
    for name, query, dab, freq, band, url in stations:
        lines.append(f'    {{{name}, {query}, {dab}, {freq}, {band}, {url}}},')
    lines.append('    {},')
    lines.append('};')
    return ('\n'.join(lines) + '\n').encode('utf-8')


def generate(source, output, streams_path=None):
    rows = load_rows(source)
    # streams.csv sits beside stations.csv unless --streams names another file.
    streams = load_streams(streams_path or source.with_name('streams.csv'), {r['name'] for _, r in rows})
    area_rows, stations, pool = build(rows, streams)
    return render(area_rows, stations, pool), len(area_rows), len(stations), output


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, default=DEFAULT_SOURCE)
    parser.add_argument('--output', type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument('--streams', type=Path, default=None,
                        help='baked stream URLs (default: streams.csv beside --input)')
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    try:
        text, areas, stations, output = generate(args.input, args.output, args.streams)
    except (ValueError, OSError) as error:
        print(f'radio table: {error}', file=sys.stderr)
        sys.exit(1)
    if args.check:
        if not output.exists() or output.read_bytes() != text:
            print(f'regenerate {output}', file=sys.stderr)
            sys.exit(1)
    else:
        output.write_bytes(text)
    print(f'AU radio table: {areas} areas, {stations} stations, {len(text)} bytes')
