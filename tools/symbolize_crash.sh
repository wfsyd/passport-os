#!/usr/bin/env bash
# Debug helper: build like tools/validate.sh --firmware, then symbolize crash addresses.
set -uo pipefail
dir=/tmp/sym-build
SDKCONFIG_DEFAULTS="$PWD/sdkconfig.defaults" idf.py -B "$dir" -D "SDKCONFIG=$dir/sdkconfig" \
    -D PASSPORT_PUBLIC_RELEASE=ON -D PASSPORT_MUSE_USE_JOLLYBOT=ON build >/tmp/build.log 2>&1 || { tail -50 /tmp/build.log; exit 1; }
elf=$(ls "$dir"/*.elf | head -1)
echo "ELF: $elf"
echo "ELF SHA256: $(sha256sum "$elf" | cut -c1-9) (device log showed 4064c8356)"
echo "=== SYMBOLS ==="
for a in 0x420c020a 0x420bd0d4 0x420c16a0 0x420cf4ae 0x4221e936 0x4221e92a 0x420d1694 0x4221f744 0x420b7338 0x420b6de8 0x42012076; do
    printf '%s: ' "$a"; riscv32-esp-elf-addr2line -pfiaC -e "$elf" "$a"
done
echo "=== TASK STACK ADDR 0x3fcb7ec0 owner (nm near) ==="
riscv32-esp-elf-nm -n -C "$elf" | awk '$1>="3fcb0000" && $1<="3fcb9000"' | head -40
echo "=== END ==="
