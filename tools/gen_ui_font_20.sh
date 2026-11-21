#!/usr/bin/env bash
set -euo pipefail

# Regenerates main/fonts/ui_font_20.c (20px SourceHanSansSC title subset, 4bpp,
# LVGL lossless per-glyph compression). Options mirror the "Opts:" line in the
# generated file; the symbol set lives in main/fonts/ui_font_20_symbols.txt.
PROJ="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OTF="$PROJ/managed_components/lvgl__lvgl/scripts/built_in_font/SourceHanSansSC-Normal.otf"
SYMBOLS_FILE="$PROJ/main/fonts/ui_font_20_symbols.txt"
OUT="$PROJ/main/fonts/ui_font_20.c"

SYMBOLS=$(cat "$SYMBOLS_FILE")

npx --yes lv_font_conv@1.5.3 \
  --font "$OTF" \
  --range 0x20-0x7e \
  --symbols "$SYMBOLS" \
  --size 20 --bpp 4 --format lvgl \
  --lv-include lvgl.h --lv-font-name ui_font_20 \
  --lv-fallback buddy_font_16 \
  -o "$OUT"

echo "Generated ui_font_20.c successfully: $(ls -lh "$OUT")"
