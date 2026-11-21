#!/usr/bin/env python3
"""Convert the ACMA broadcast transmitter workbook into third_party/au-radio/stations.csv.

Input: BroadcastTransmitterExcel.xlsx from https://www.acma.gov.au/list-transmitters-licence-broadcast
(Creative Commons Attribution 3.0 Australia; see third_party/au-radio/README.md).
Only the AM, FM and DR sheets are read. Temporary Licences, DTV, Introduction and
Glossary are ignored.

Filter rules, applied in this order (counts are printed):
  1. Status must be Issued (drops Expired).
  2. State must be one of the eight AU states and territories (drops EXT, the external
     territories, which lie outside the coordinate bounds the table accepts).
  3. Purpose must be National, Commercial, Community or a Retransmission
     (drops HPON, the high power open narrowcasting services, and anything else).
  4. Coordinates must parse as degrees, minutes and seconds.
  5. Each AM/FM row is named by its on-air brand from third_party/au-radio/brands.csv
     (see tools/au_radio_brands.py). The lookup is state + callsign + band + freq,
     then state + callsign + band, then state + callsign. A row with no mapping is
     dropped, because the device cannot stream it.
  6. One AM/FM row per brand and band in an area. The highest power site is kept.
  7. DAB+ rows in the workbook are not used by name (their block names do not resolve
     to a stream). An area that has an ACMA DAB+ row gets, on DAB+:
       (a) the national DAB-only services on block 9C: Double J, ABC Jazz, SBS PopAsia;
       (b) every mapped National or Commercial AM/FM service of that area, once per
           brand, with dab_block empty (shown as DAB+). Commercial and ABC/SBS
           services simulcast on DAB+.
     A DAB+ row with no FM area within 50 km marks no area.
  8. An area with only DAB+ rows (no FM or AM) is folded into the nearest area within
     40 km that has FM or AM rows. Its DAB+ rows are merged there, deduplicated by name
     (the block of the existing row is kept). Without such an area it is kept as it is.
     Without this, a DAB+ transmitter site can become an area with three rows, which
     the device would show instead of the metro area.
  9. Areas with no rows left are dropped, so the device falls back to the directory
     search there.
  10. Each area keeps at most 96 rows (au_radio::kMaxListed), ranked National, then
     Commercial and DAB+, then Community, then Retransmission, each by power.

Areas: AM/FM rows form an area from the ACMA licence area (RA suffix removed) or,
when that is blank, from the area served. Areas are keyed by state and name. Each
area's lat/lon is the site of its highest power AM/FM row, and its state is that row's state.
Area ids are the state plus a slug, cut to 16 bytes, with a numeric suffix if needed.

Names: the brand from brands.csv (for example triple j, KIIS 106.5). Query is the
query from brands.csv, which radio-browser searches by name to find the stream.

Usage: acma_to_stations.py --xlsx PATH [--out third_party/au-radio/stations.csv]
                           [--brands third_party/au-radio/brands.csv]
"""
import argparse
import csv
import math
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

import openpyxl

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUT = ROOT / 'third_party/au-radio/stations.csv'
DEFAULT_BRANDS = ROOT / 'third_party/au-radio/brands.csv'
COLUMNS = ['area_id', 'area_name', 'state', 'lat', 'lon', 'band', 'freq', 'dab_block', 'name', 'query']
BRAND_COLUMNS = ['state', 'callsign', 'band', 'freq', 'brand', 'query', 'source', 'confidence']
STATES = ('NSW', 'VIC', 'QLD', 'SA', 'WA', 'TAS', 'ACT', 'NT')
PRIORITY = {'National': 0, 'Commercial': 1, 'Community': 2,
            'Retransmission': 3, 's212 Retransmission': 3}
DAB_PRIORITY = 1
BAND_ORDER = {'FM': 0, 'AM': 1, 'DAB': 2}
# National DAB+-only services, on block 9C, added to every area that has DAB+ coverage.
DAB_NATIONAL = [('Double J', 'Double J', '9C'), ('ABC Jazz', 'ABC Jazz', '9C'),
                ('SBS PopAsia', 'SBS PopAsia', '9C')]
DAB_SIMULCAST_PURPOSES = ('National', 'Commercial')
ROWS_PER_AREA = 96          # au_radio::kMaxListed in main/apps/radio/au_radio_table.h
DAB_MAX_KM = 50.0
DAB_ONLY_FOLD_KM = 40.0    # DAB+-only areas fold into an FM/AM area this close
MAX_TEXT = 40               # area_name, name, query limit in tools/generate_radio_table.py
MAX_ID = 16
DMS = re.compile(r'^\s*(\d+)\s+(\d+)\s+(\d+(?:\.\d+)?)\s*([NSEW])\s*$')
UPPER_TOKENS = {'NSW', 'VIC', 'QLD', 'SA', 'WA', 'NT', 'ACT', 'TAS', 'ABC', 'SBS', 'FM', 'AM'}


def parse_dms(text):
    match = DMS.match(str(text or ''))
    if not match:
        return None
    degrees, minutes, seconds, hemisphere = match.groups()
    value = int(degrees) + int(minutes) / 60 + float(seconds) / 3600
    return -value if hemisphere in 'SW' else value


def distance_km(lat1, lon1, lat2, lon2):
    radius = 6371.0
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dp, dl = p2 - p1, math.radians(lon2 - lon1)
    a = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * radius * math.asin(math.sqrt(a))


def normal_licence_area(text):
    text = re.sub(r'\s+RA\d+$', '', ' '.join(text.split()))
    words = []
    for word in text.title().split(' '):
        words.append(word.upper() if word.upper() in UPPER_TOKENS else word)
    return ' '.join(words)


def clip(text):
    return text.encode('utf-8')[:MAX_TEXT].decode('utf-8', 'ignore').strip()


def read_sheet(workbook, name):
    rows = workbook[name].iter_rows(values_only=True)
    header = [str(h).strip() if h else '' for h in next(rows)]
    for raw in rows:
        if any(cell not in (None, '') for cell in raw):
            yield dict(zip(header, raw))


def load_records(workbook, skipped):
    """Return AM/FM and DAB records after the status, state, purpose and coordinate rules."""
    records = []
    for band, sheet in (('AM', 'AM'), ('FM', 'FM'), ('DAB', 'DR')):
        for row in read_sheet(workbook, sheet):
            if row.get('Status') != 'Issued':
                skipped['not issued'] += 1
                continue
            state = (row.get('State') or '').strip()
            if state not in STATES:
                skipped[f'state {state or "blank"} (outside the eight states)'] += 1
                continue
            lat, lon = parse_dms(row.get('Latitude')), parse_dms(row.get('Longitude'))
            if lat is None or lon is None:
                skipped['unparsable coordinates'] += 1
                continue
            purpose = (row.get('Purpose') or '').strip()
            if band != 'DAB' and purpose not in PRIORITY:
                skipped[f'purpose {purpose} (narrowcast or other)'] += 1
                continue
            licence_area = ' '.join((row.get('Licence Area') or '').split())
            if band == 'AM':
                freq = int(row['Frequency(kHz)'])
                power = float(row.get('Transmitter Power (W)') or 0)
                callsign = (row.get('Callsign') or '').strip()
                if not 522 <= freq <= 1728:
                    skipped['AM frequency out of range'] += 1
                    continue
            elif band == 'FM':
                freq = round(float(row['Frequency(MHz)']) * 10)
                power = float(row.get('Maximum ERP (W)') or 0)
                callsign = (row.get('Callsign') or '').strip()
                if not 870 <= freq <= 1080:
                    skipped['FM frequency out of range'] += 1
                    continue
            else:
                freq = 0
                power = float(row.get('Maximum ERP (W)') or 0)
                callsign = ''
                area_name = ' '.join(row['Area Served'].split())
                block = (row.get('Frequency Block') or '').strip()
                if not block:
                    skipped['DAB without block'] += 1
                    continue
            if band != 'DAB':
                area_name = normal_licence_area(licence_area) if licence_area else ' '.join(row['Area Served'].split())
            records.append({
                'band': band, 'freq': freq, 'power': power, 'callsign': callsign,
                'purpose': purpose, 'state': state, 'lat': lat, 'lon': lon,
                'area_name': clip(area_name),
                'block': (row.get('Frequency Block') or '').strip() if band == 'DAB' else '',
            })
    return records


def load_brands(path):
    """Map (state, callsign, band, freq), then (state, callsign, band), then (state, callsign) to (brand, query).

    High confidence entries are read first, so a fallback picks the surest entry for a callsign.
    """
    with path.open(encoding='utf-8', newline='') as source:
        reader = csv.DictReader(source)
        if reader.fieldnames != BRAND_COLUMNS:
            raise ValueError(f'{path}: header must be {",".join(BRAND_COLUMNS)}')
        rows = sorted(reader, key=lambda r: r['confidence'] != 'high')
    lookup = {}
    for r in rows:
        brand, query = r['brand'].strip(), r['query'].strip()
        if not brand or not query:
            raise ValueError(f'{path}: brand and query are required for {r["state"]} {r["callsign"]}')
        entry = (brand, query)
        state, callsign, band = r['state'], r['callsign'], r['band']
        lookup.setdefault((state, callsign, band, int(r['freq'])), entry)
        lookup.setdefault((state, callsign, band), entry)
        lookup.setdefault((state, callsign), entry)
    return lookup


def find_brand(brands, record):
    for key in ((record['state'], record['callsign'], record['band'], record['freq']),
                (record['state'], record['callsign'], record['band']),
                (record['state'], record['callsign'])):
        if key in brands:
            return brands[key]
    return None


def make_area_ids(keys):
    """Stable ids from sorted (state, name) keys: state + slug, at most 16 bytes, unique."""
    ids, used = {}, set()
    for state, name in keys:
        slug = re.sub(r'[^a-z0-9]+', '-', name.lower()).strip('-')
        base = f'{state.lower()}-{slug}'[:MAX_ID].rstrip('-')
        candidate, n = base, 2
        while candidate in used:
            suffix = f'-{n}'
            candidate = base[:MAX_ID - len(suffix)].rstrip('-') + suffix
            n += 1
        used.add(candidate)
        ids[(state, name)] = candidate
    return ids


def fold_dab_only_areas(rows, skipped):
    """Fold each area that has only DAB+ rows into the nearest FM/AM area within 40 km.

    rows are output rows in COLUMNS order. The DAB+ rows are merged into the target area,
    deduplicated by name (the target's existing row, and its block, wins). A DAB+-only area
    with no FM/AM area in range is kept. Returns the new row list, with areas in their
    original order and rows in table order.
    """
    areas = {}      # area_id -> {'lat', 'lon', 'bands': set, 'rows': list}
    order = []
    for row in rows:
        area_id = row[0]
        if area_id not in areas:
            areas[area_id] = {'head': row[:5], 'lat': float(row[3]), 'lon': float(row[4]),
                              'bands': set(), 'rows': []}
            order.append(area_id)
        areas[area_id]['bands'].add(row[5])
        areas[area_id]['rows'].append(row)

    targets = {a: v for a, v in areas.items() if v['bands'] - {'DAB'}}
    for area_id in order:
        area = areas[area_id]
        if area_id in targets:
            continue
        best, best_km = None, DAB_ONLY_FOLD_KM
        for other_id, other in targets.items():
            km = distance_km(area['lat'], area['lon'], other['lat'], other['lon'])
            if km <= best_km:
                best, best_km = other_id, km
        if best is None:
            skipped['DAB+-only area with no FM or AM area within 40 km (kept)'] += 1
            continue
        target = areas[best]
        names = {row[8] for row in target['rows'] if row[5] == 'DAB'}
        merged = 0
        for row in area['rows']:
            if row[8] in names:
                skipped['DAB+-only area row already in target (deduplicated)'] += 1
                continue
            names.add(row[8])
            target['rows'].append(target['head'] + row[5:])
            merged += 1
        skipped['DAB+-only areas folded into a nearby FM/AM area'] += 1
        area['folded_into'] = best
        if len(target['rows']) > ROWS_PER_AREA:
            raise ValueError(f'{best} exceeds {ROWS_PER_AREA} rows after folding {area_id}')

    out = []
    for area_id in order:
        area = areas[area_id]
        if area.get('folded_into'):
            continue
        kept = sorted(area['rows'], key=lambda r: (BAND_ORDER[r[5]], int(r[6]), r[8]))
        out.extend(kept)
    return out


def build(records, brands, skipped):
    am_fm = [r for r in records if r['band'] != 'DAB']
    dab = [r for r in records if r['band'] == 'DAB']

    # Areas and their main site (highest power AM/FM row, first one wins a tie).
    main = {}
    fm_areas = set()
    for r in am_fm:
        key = (r['state'], r['area_name'])
        if r['band'] == 'FM':
            fm_areas.add(key)
        if key not in main or r['power'] > main[key]['power']:
            main[key] = r
    area_ids = make_area_ids(sorted(main))

    # AM/FM: name by brand; one row per brand and band in an area, from the highest power site.
    by_brand = {}
    for r in am_fm:
        area = (r['state'], r['area_name'])
        entry = find_brand(brands, r)
        if entry is None:
            skipped['AM/FM with no brand mapping (cannot be streamed)'] += 1
            continue
        brand, query = clip(entry[0]), clip(entry[1])
        key = (area, r['band'], brand)
        if key in by_brand:
            skipped['AM/FM repeat (same brand and band in area)'] += 1
            if by_brand[key]['power'] >= r['power']:
                continue
        by_brand[key] = dict(r, brand=brand, query=query)

    # DAB+: nearest FM area within 50 km of the block's site. Only the areas reached are kept.
    dab_areas = set()
    for r in dab:
        best = (r['state'], r['area_name']) if (r['state'], r['area_name']) in fm_areas else None
        if best is None:
            best_km = DAB_MAX_KM
            for key in fm_areas:
                site = main[key]
                km = distance_km(r['lat'], r['lon'], site['lat'], site['lon'])
                if km <= best_km:
                    best, best_km = key, km
        if best is None:
            skipped['DAB+ with no FM area within 50 km'] += 1
            continue
        dab_areas.add(best)

    stations = defaultdict(list)
    by_area = defaultdict(list)
    for (area, band, _brand), r in by_brand.items():
        by_area[area].append(r)
        stations[area].append({'band': band, 'freq': r['freq'], 'power': r['power'],
                               'purpose': r['purpose'], 'name': r['brand'], 'query': r['query'],
                               'dab_block': ''})

    # DAB+ for areas with DAB+ coverage: the 9C national services, then simulcasts of mapped
    # National and Commercial services (each brand once, from its highest power site).
    for area in sorted(dab_areas):
        names = set()
        for name, query, block in DAB_NATIONAL:
            names.add(name)
            stations[area].append({'band': 'DAB', 'freq': 0, 'power': 0, 'purpose': 'DAB',
                                   'name': name, 'query': query, 'dab_block': block})
        for r in sorted(by_area[area], key=lambda x: (-x['power'], x['brand'])):
            if r['purpose'] not in DAB_SIMULCAST_PURPOSES or r['brand'] in names:
                continue
            names.add(r['brand'])
            stations[area].append({'band': 'DAB', 'freq': 0, 'power': r['power'],
                                   'purpose': r['purpose'], 'name': r['brand'],
                                   'query': r['query'], 'dab_block': ''})

    # Cap per area by priority, then restore table order. Areas left with no rows are dropped.
    out_rows = []
    for key in sorted(main, key=lambda k: (k[0], k[1])):
        items = stations.get(key, [])
        if not items:
            continue

        def rank(item):
            prio = DAB_PRIORITY if item['band'] == 'DAB' else PRIORITY[item['purpose']]
            return (prio, -item['power'], BAND_ORDER[item['band']], item['freq'], item['name'])

        if len(items) > ROWS_PER_AREA:
            skipped[f'over {ROWS_PER_AREA} rows (lower priority dropped)'] += len(items) - ROWS_PER_AREA
        kept = sorted(sorted(items, key=rank)[:ROWS_PER_AREA],
                      key=lambda i: (BAND_ORDER[i['band']], i['freq'], i['name']))
        site = main[key]
        for item in kept:
            out_rows.append([
                area_ids[key], key[1], key[0], f"{site['lat']:.4f}", f"{site['lon']:.4f}",
                item['band'], str(item['freq']), item['dab_block'], item['name'], item['query'],
            ])
    empty = len(main) - len({(r[2], r[1]) for r in out_rows})
    skipped['areas with no rows left (device falls back to directory search)'] += empty
    return fold_dab_only_areas(out_rows, skipped)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--xlsx', type=Path, required=True, help='ACMA BroadcastTransmitterExcel.xlsx')
    parser.add_argument('--out', type=Path, default=DEFAULT_OUT)
    parser.add_argument('--brands', type=Path, default=DEFAULT_BRANDS, help='callsign to brand map')
    args = parser.parse_args()
    skipped = Counter()
    brands = load_brands(args.brands)
    workbook = openpyxl.load_workbook(args.xlsx, read_only=True, data_only=True)
    records = load_records(workbook, skipped)
    rows = build(records, brands, skipped)
    with args.out.open('w', encoding='utf-8', newline='') as target:
        writer = csv.writer(target, lineterminator='\n')
        writer.writerow(COLUMNS)
        writer.writerows(rows)
    areas = len({r[0] for r in rows})
    print(f'read {len(records)} AM/FM/DAB rows after status, state, purpose and coordinate rules')
    for reason, count in sorted(skipped.items()):
        print(f'  dropped {count:5d}: {reason}')
    print(f'wrote {len(rows)} rows for {areas} areas to {args.out}')


if __name__ == '__main__':
    try:
        main()
    except (OSError, KeyError, ValueError) as error:
        print(f'acma_to_stations: {error}', file=sys.stderr)
        sys.exit(1)
