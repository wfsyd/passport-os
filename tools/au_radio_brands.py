#!/usr/bin/env python3
"""Write third_party/au-radio/brands.csv: ACMA callsigns mapped to on-air brands.

ACMA lists transmitters by callsign only, and a callsign such as 2JJJ does not
resolve to a radio stream. This file maps each callsign to the brand listeners know,
and the query that radio-browser can find the stream by.

The brand list is hand-curated, not derived from any dataset. It has two parts:
  - national ABC and SBS callsign patterns (2JJJ is triple j, 2ABCFM is ABC Classic,
    2SBSFM is SBS Radio, and so on), applied in every state;
  - well-known capital-city stations (commercial, community and ABC local services)
    that ACMA lists only by callsign.

The confidence column records how far each entry can be trusted:
  high    a national ABC/SBS callsign pattern, or a well-known ABC local callsign;
  medium  a station brand from memory. Wikipedia (partial, summarised) was used to
          confirm some Sydney entries only. Other medium rows are not checked.
Only callsigns that appear on an AM or FM sheet of the ACMA workbook get a row.

Output columns: state, callsign, band, freq, brand, query, source, confidence.
freq uses the same units as stations.csv (FM in 0.1 MHz, AM in kHz).

The workbook is not committed. Usage:
  python3 tools/au_radio_brands.py --xlsx PATH/TO/BroadcastTransmitterExcel.xlsx
"""
import argparse
import csv
import re
import sys
from collections import Counter
from pathlib import Path

import openpyxl

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
import acma_to_stations  # noqa: E402  (reads the ACMA sheets with the same filter rules)

ROOT = TOOLS.parent
DEFAULT_OUT = ROOT / 'third_party/au-radio/brands.csv'
COLUMNS = ['state', 'callsign', 'band', 'freq', 'brand', 'query', 'source', 'confidence']

# Callsign suffix after the state digit -> (brand, query).
NATIONAL = {
    'JJJ': ('triple j', 'triple j'),
    'ABCFM': ('ABC Classic', 'ABC Classic'),
    'RN': ('ABC Radio National', 'Radio National'),
    'ABCRN': ('ABC Radio National', 'Radio National'),
    'PB': ('ABC NewsRadio', 'ABC NewsRadio'),
    'SBSFM': ('SBS Radio', 'SBS Radio'),
    'EA': ('SBS Radio', 'SBS Radio'),
}
# Well-known ABC local callsigns (high confidence).
LOCAL = {
    '2BL': 'ABC Sydney', '3LO': 'ABC Melbourne', '4QR': 'ABC Brisbane', '5AN': 'ABC Adelaide',
    '6WF': 'ABC Perth', '7ZR': 'ABC Hobart', '8DDD': 'ABC Darwin', '2CN': 'ABC Canberra',
    '2NC': 'ABC Newcastle', '4QN': 'ABC Townsville',
}
# Capital-city and well-known stations (medium confidence), keyed by callsign.
MEDIUM = {
    '2UE': '2UE', '2GB': '2GB', '2CH': 'SEN 1170', '2SM': '2SM', '2KY': 'Sky Sports Radio',
    '2MBS': 'Fine Music', '2DAY': '2Day FM', '2SYD': 'Nova 96.9', '2MMM': 'Triple M Sydney',
    '2WFM': 'KIIS 106.5', '2PTV': 'Smooth 95.3', '2FBI': 'FBi Radio', '2SER': '2SER',
    '2CBA': 'Hope 103.2',
    '3AW': '3AW', '3CR': '3CR', '3KND': 'Kool n Deadly', '3RRR': '3RRR', '3PBS': 'PBS 106.7',
    '3SYN': 'SYN', '3MBS': 'Fine Music', '3MMM': 'Triple M Melbourne', '3MEL': 'Nova 100',
    '3PTV': 'Smooth 91.5', '3KKZ': 'Gold 104.3',
    '4BC': '4BC', '4KQ': '4KQ', '4MMM': 'Triple M Brisbane', '4BNE': 'Nova 106.9',
    '4MBS': '4MBS Classic', '4ZZZ': '4ZZZ', '4EB': '4EB', '4RPH': '4RPH',
    '5AA': 'FIVEaa', '5MMM': 'Triple M Adelaide', '5UV': 'Radio Adelaide',
    '6PR': '6PR', '6MMM': 'Triple M Perth', '6MIX': 'Mix 94.5', '6RTR': 'RTRFM', '6IX': '6IX',
    '7RPH': 'Print Radio Tasmania', '2CA': '2CA', '2CC': '2CC',
    '1RPH': 'Vision Australia Radio Canberra', '1ART': 'Artsound FM', '8MIX': 'Mix 104.9',
    '2HD': '2HD', '2NUR': '2NUR', '4SEA': 'Sea FM', '4GLD': 'Gold 92.5',
}
CAPITAL_AREAS = {'Sydney', 'Melbourne', 'Brisbane', 'Adelaide', 'Perth', 'Hobart', 'Canberra',
                 'Darwin', 'Gold Coast', 'Newcastle', 'Wollongong', 'Geelong', 'Sunshine Coast',
                 'Townsville', 'Cairns'}


def classify(callsign, state, area_name):
    """Return (brand, query, source, confidence) for a callsign, or None."""
    if callsign in LOCAL:
        brand = LOCAL[callsign]
        return brand, brand, 'ABC local callsign (well-known)', 'high'
    match = re.match(r'^\d(.+)$', callsign)
    if match and match.group(1) in NATIONAL:
        brand, query = NATIONAL[match.group(1)]
        return brand, query, 'national callsign pattern', 'high'
    if area_name in CAPITAL_AREAS and callsign in MEDIUM:
        brand = MEDIUM[callsign]
        if state == 'NSW':
            source = 'memory; Wikipedia (partial) used to confirm some Sydney entries'
        else:
            source = 'memory; not checked against a source'
        return brand, brand, source, 'medium'
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--xlsx', type=Path, required=True, help='ACMA BroadcastTransmitterExcel.xlsx')
    parser.add_argument('--out', type=Path, default=DEFAULT_OUT)
    args = parser.parse_args()
    workbook = openpyxl.load_workbook(args.xlsx, read_only=True, data_only=True)
    records = acma_to_stations.load_records(workbook, Counter())
    rows, seen = [], set()
    for r in records:
        if r['band'] == 'DAB' or not r['callsign']:
            continue
        key = (r['state'], r['callsign'], r['band'], r['freq'])
        if key in seen:
            continue
        seen.add(key)
        found = classify(r['callsign'], r['state'], r['area_name'])
        if found is None:
            continue
        brand, query, source, confidence = found
        rows.append([r['state'], r['callsign'], r['band'], r['freq'], brand, query, source, confidence])
    with args.out.open('w', encoding='utf-8', newline='') as target:
        writer = csv.writer(target, lineterminator='\n')
        writer.writerow(COLUMNS)
        writer.writerows(rows)
    print(f'wrote {len(rows)} brand rows to {args.out}: {dict(Counter(row[7] for row in rows))}')


if __name__ == '__main__':
    try:
        main()
    except (OSError, KeyError, ValueError) as error:
        print(f'au_radio_brands: {error}', file=sys.stderr)
        sys.exit(1)
