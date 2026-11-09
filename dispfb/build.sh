#!/bin/bash
# Rebuild dispfb: overlay -> dtbo -> embedded C header (8-byte aligned: libfdt's fdt_check_header rejects unaligned blobs) -> module.
set -e
cd "$(dirname "$(realpath "$0")")"
dtc -I dts -O dtb -W no-reg_format -W no-avoid_default_addr_size -o dispfb.dtbo dispfb.dts 2>&1 | grep -v -E 'Warning|Failed prerequisite' || true
dtc -I dtb -O dts dispfb.dtbo 2>/dev/null | grep -q '__local_fixups__' || { echo "overlay lost its __local_fixups__"; exit 1; }
xxd -i dispfb.dtbo | sed 's/^unsigned char dispfb_dtbo\[\]/static const unsigned char dispfb_dtbo[] __aligned(8)/; s/^unsigned int dispfb_dtbo_len/static const unsigned int dispfb_dtbo_len/' > dispfb_dtbo.h
grep -q '__aligned(8)' dispfb_dtbo.h
make $(cat ../KMAKE) M=$PWD modules 2>&1 | grep -E 'error|warning: [^t]|LD \[M\]'
ls -la --time-style=+%H:%M:%S j713_dispfb.ko
