#!/usr/bin/env bash
set -euo pipefail

# Builds one firmware size-measurement variant and reports the app binary size.
# Usage: tools/size_report.sh <variant>
# Variants: base cert_cmn log_warn assert_silent nano_fmt combined codec_trim

variant="${1:?usage: $0 <base|cert_cmn|log_warn|assert_silent|nano_fmt|combined|codec_trim>}"
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
variant_cfg="/tmp/variant.cfg"
out_dir="${repo_root}/size-out"

case "${variant}" in
    base)
        : >"${variant_cfg}"
        ;;
    cert_cmn)
        cat >"${variant_cfg}" <<'EOF'
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL=n
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN=y
EOF
        ;;
    log_warn)
        cat >"${variant_cfg}" <<'EOF'
CONFIG_LOG_DEFAULT_LEVEL_INFO=n
CONFIG_LOG_DEFAULT_LEVEL_WARN=y
CONFIG_LOG_MAXIMUM_LEVEL_WARN=y
EOF
        ;;
    assert_silent)
        cat >"${variant_cfg}" <<'EOF'
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=n
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_SILENT=y
EOF
        ;;
    nano_fmt)
        cat >"${variant_cfg}" <<'EOF'
CONFIG_NEWLIB_NANO_FORMAT=y
CONFIG_LIBC_NEWLIB_NANO_FORMAT=y
EOF
        ;;
    combined)
        cat >"${variant_cfg}" <<'EOF'
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL=n
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN=y
CONFIG_LOG_DEFAULT_LEVEL_INFO=n
CONFIG_LOG_DEFAULT_LEVEL_WARN=y
CONFIG_LOG_MAXIMUM_LEVEL_WARN=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=n
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_SILENT=y
CONFIG_NEWLIB_NANO_FORMAT=y
CONFIG_LIBC_NEWLIB_NANO_FORMAT=y
EOF
        ;;
    codec_trim)
        cat >"${variant_cfg}" <<'EOF'
CONFIG_AUDIO_DECODER_G711_SUPPORT=n
CONFIG_AUDIO_DECODER_AMRNB_SUPPORT=n
CONFIG_AUDIO_DECODER_AMRWB_SUPPORT=n
CONFIG_AUDIO_DECODER_FLAC_SUPPORT=n
CONFIG_AUDIO_DECODER_VORBIS_SUPPORT=n
CONFIG_AUDIO_DECODER_ADPCM_SUPPORT=n
CONFIG_AUDIO_DECODER_ALAC_SUPPORT=n
CONFIG_AUDIO_DECODER_SBC_SUPPORT=n
CONFIG_AUDIO_DECODER_LC3_SUPPORT=n
CONFIG_AUDIO_DECODER_G722_SUPPORT=n
CONFIG_AUDIO_ENCODER_AAC_SUPPORT=n
CONFIG_AUDIO_ENCODER_G711_SUPPORT=n
CONFIG_AUDIO_ENCODER_AMRNB_SUPPORT=n
CONFIG_AUDIO_ENCODER_AMRWB_SUPPORT=n
CONFIG_AUDIO_ENCODER_ADPCM_SUPPORT=n
CONFIG_AUDIO_ENCODER_ALAC_SUPPORT=n
CONFIG_AUDIO_ENCODER_PCM_SUPPORT=n
CONFIG_AUDIO_ENCODER_SBC_SUPPORT=n
CONFIG_AUDIO_ENCODER_LC3_SUPPORT=n
CONFIG_AUDIO_ENCODER_G722_SUPPORT=n
CONFIG_AUDIO_SIMPLE_DEC_WAV_SUPPORT=n
CONFIG_AUDIO_SIMPLE_DEC_M4A_SUPPORT=n
CONFIG_AUDIO_SIMPLE_DEC_OGG_SUPPORT=n
EOF
        ;;
    *)
        echo "ERROR: unknown variant '${variant}'" >&2
        exit 2
        ;;
esac

if ! command -v idf.py >/dev/null 2>&1; then
    echo "ERROR: idf.py is not available; activate ESP-IDF 5.5.3 first." >&2
    exit 1
fi

build_dir="$(mktemp -d /tmp/ai-passport-size.XXXXXX)"
trap 'case "${build_dir}" in /tmp/ai-passport-size.*) rm -rf -- "${build_dir}" ;; esac' EXIT

echo "== Variant ${variant}: extra sdkconfig lines =="
cat "${variant_cfg}"
echo "== Building into ${build_dir} =="

if ! SDKCONFIG_DEFAULTS="${repo_root}/sdkconfig.defaults;${variant_cfg}" \
    idf.py -B "${build_dir}" \
    -D "SDKCONFIG=${build_dir}/sdkconfig" \
    -D PASSPORT_PUBLIC_RELEASE=ON -D PASSPORT_MUSE_USE_JOLLYBOT=ON build; then
    echo "ERROR: build failed for variant ${variant}" >&2
    echo "SIZE_RESULT ${variant} FAILED"
    exit 0
fi

echo "== Size: ${variant} =="
idf.py -B "${build_dir}" size

if [[ "${variant}" == "codec_trim" ]]; then
    echo "== sdkconfig codec options: ${variant} =="
    grep -E "CONFIG_AUDIO_(DECODER|ENCODER|SIMPLE_DEC)" "${build_dir}/sdkconfig" || true
    echo "== Size components (top 30): ${variant} =="
    idf.py -B "${build_dir}" size-components 2>&1 | head -30 || true
fi

if [[ "${variant}" == "base" ]]; then
    mkdir -p "${out_dir}"

    echo "== Size components: ${variant} =="
    idf.py -B "${build_dir}" size-components

    echo "== Size files: ${variant} (first 80 lines) =="
    files_log="$(mktemp /tmp/ai-passport-size-files.XXXXXX)"
    idf.py -B "${build_dir}" size-files >"${files_log}" 2>&1 || true
    head -80 "${files_log}" || true
    rm -f -- "${files_log}"

    cp -- "${build_dir}"/*.map "${out_dir}/"
    idf.py -B "${build_dir}" size --format json2 --output-file "${out_dir}/size-${variant}.json"

    echo "=== MAP BREAKDOWN START ==="
    map_file="$(find "${build_dir}" -maxdepth 1 -name '*.map' | head -1)"
    elf_file="${map_file%.map}.elf"
    echo "map file: ${map_file}"
    echo "elf file: ${elf_file}"

    echo "== 2a: libmain.a name in map =="
    grep -m3 "libmain.a" "${map_file}" || echo "NOT FOUND: libmain.a in map"
    echo "== 2a: libpassport_muse.a name in map =="
    grep -m3 "libpassport_muse.a" "${map_file}" || echo "NOT FOUND: libpassport_muse.a in map"
    echo "== 2a: libesp-wifi-connect.a name in map =="
    grep -m3 "libesp-wifi-connect.a" "${map_file}" || echo "NOT FOUND: libesp-wifi-connect.a in map"

    echo "== esp_idf_size --help (first 60 lines) =="
    python -m esp_idf_size --help 2>&1 | head -60 || true

    echo "== 2a: libmain.a per-object breakdown =="
    if ! python -m esp_idf_size --archive_details esp-idf/main/libmain.a "${map_file}" 2>&1; then
        echo "CMD FAILED: libmain.a archive details"
    fi

    echo "== 2b: libpassport_muse.a archive details (first 60 lines) =="
    python -m esp_idf_size --archive_details esp-idf/passport_muse/libpassport_muse.a "${map_file}" 2>&1 | head -60 \
        || echo "CMD FAILED: libpassport_muse.a archive details"

    echo "== 2b: libesp-wifi-connect.a archive details (first 60 lines) =="
    python -m esp_idf_size --archive_details esp-idf/esp-wifi-connect/libesp-wifi-connect.a "${map_file}" 2>&1 | head -60 \
        || echo "CMD FAILED: libesp-wifi-connect.a archive details"

    echo "== 2c: largest symbols in final ELF (last 150 by size) =="
    riscv32-esp-elf-nm -S --size-sort -C "${elf_file}" 2>&1 | tail -150 \
        || echo "CMD FAILED: riscv32-esp-elf-nm -S --size-sort -C ${elf_file}"

    echo "== 2d: ELF section headers =="
    riscv32-esp-elf-objdump -h "${elf_file}" 2>&1 \
        || echo "CMD FAILED: riscv32-esp-elf-objdump -h ${elf_file}"
    echo "=== MAP BREAKDOWN END ==="
fi

app_bin=""
if [[ -f "${build_dir}/project_description.json" ]]; then
    app_bin="$(python3 -c '
import json, os, sys
d = json.load(open(sys.argv[1]))
b = d.get("app_bin", "")
if b and not os.path.isabs(b):
    b = os.path.join(sys.argv[2], b)
print(b)
' "${build_dir}/project_description.json" "${build_dir}")"
fi
if [[ -z "${app_bin}" || ! -f "${app_bin}" ]]; then
    app_bin="$(find "${build_dir}" -maxdepth 1 -name '*.bin' \
        ! -name 'bootloader*' ! -name 'partition-table*' \
        -printf '%s %p\n' | sort -rn | head -1 | cut -d' ' -f2-)"
fi

if [[ -z "${app_bin}" || ! -f "${app_bin}" ]]; then
    echo "ERROR: app binary not found for variant ${variant}" >&2
    echo "SIZE_RESULT ${variant} FAILED"
    exit 0
fi

echo "SIZE_RESULT ${variant} $(stat -c %s "${app_bin}")"
