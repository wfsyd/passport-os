#!/usr/bin/env python3
"""Extract Australian places from an all-the-cities cities.pbf into a CSV.

The npm tarball (MIT code, GeoNames CC BY 4.0 data) is downloaded and unpacked
by hand into an empty scratch directory. This script only reads the binary
protobuf layout; it never runs the package's JavaScript.

Usage: extract_all_the_cities.py PATH/TO/cities.pbf
Writes third_party/au-cities/au_cities.csv.
"""
import argparse
import csv
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / 'third_party/au-cities/au_cities.csv'
MIN_POPULATION = 15000
CAPITALS = {
    ('Canberra', '01'), ('Sydney', '02'), ('Darwin', '03'), ('Brisbane', '04'),
    ('Adelaide', '05'), ('Hobart', '06'), ('Melbourne', '07'), ('Perth', '08'),
}


def varint(buf, pos):
    shift = value = 0
    while True:
        byte = buf[pos]
        pos += 1
        value |= (byte & 0x7F) << shift
        shift += 7
        if not byte & 0x80:
            return value, pos


def zigzag(value):
    return (value >> 1) ^ -(value & 1)


def read_city(buf, pos, end):
    city = {'population': 0, 'dlat': 0, 'dlon': 0}
    names = {2: 'name', 3: 'country', 7: 'featureCode', 8: 'adminCode'}
    while pos < end:
        key, pos = varint(buf, pos)
        tag, wire = key >> 3, key & 7
        if wire == 0:
            value, pos = varint(buf, pos)
            if tag == 1:
                city['id'] = zigzag(value)
            elif tag == 9:
                city['population'] = value
            elif tag == 10:
                city['dlon'] = zigzag(value)
            elif tag == 11:
                city['dlat'] = zigzag(value)
        elif wire == 2:
            size, pos = varint(buf, pos)
            if tag in names:
                city[names[tag]] = buf[pos:pos + size].decode('utf-8')
            pos += size
        else:
            raise ValueError(f'unexpected wire type {wire} at tag {tag}')
    return city, pos


def read_all(path):
    buf = path.read_bytes()
    pos = lon = lat = 0
    while pos < len(buf):
        size, pos = varint(buf, pos)
        city, _ = read_city(buf, pos, pos + size)
        pos += size
        # Coordinates are delta-coded in units of 1e-5 degrees, shared across records.
        lon += city.pop('dlon')
        lat += city.pop('dlat')
        city['lon'] = lon / 1e5
        city['lat'] = lat / 1e5
        yield city


def timezone_for(name, admin1):
    if name == 'Broken Hill':
        return 'Australia/Broken_Hill'  # the Broken Hill municipality keeps Central time
    return {
        '01': 'Australia/Sydney', '02': 'Australia/Sydney', '03': 'Australia/Darwin',
        '04': 'Australia/Brisbane', '05': 'Australia/Adelaide', '06': 'Australia/Hobart',
        '07': 'Australia/Melbourne', '08': 'Australia/Perth',
    }[admin1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pbf', type=Path)
    args = parser.parse_args()
    rows, seen_capitals = [], set()
    for city in read_all(args.pbf):
        if city.get('country') != 'AU':
            continue
        key = (city['name'], city.get('adminCode', ''))
        capital = key in CAPITALS
        if city['population'] < MIN_POPULATION and not capital:
            continue
        if capital:
            seen_capitals.add(key)
        rows.append({
            'geonames_id': city['id'], 'name': city['name'], 'admin1': key[1],
            'lat': f"{round(city['lat'], 4):.4f}", 'lon': f"{round(city['lon'], 4):.4f}",
            'population': city['population'], 'timezone': timezone_for(city['name'], key[1]),
            'capital': int(capital),
        })
    if seen_capitals != CAPITALS:
        sys.exit(f'missing capitals: {sorted(CAPITALS - seen_capitals)}')
    rows.sort(key=lambda r: (r['admin1'], -r['population'], r['name'], r['geonames_id']))
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    with OUTPUT.open('w', newline='', encoding='utf-8') as out:
        writer = csv.DictWriter(out, fieldnames=list(rows[0]), lineterminator='\n')
        writer.writeheader()
        writer.writerows(rows)
    print(f'{len(rows)} AU places written to {OUTPUT}')


if __name__ == '__main__':
    main()
