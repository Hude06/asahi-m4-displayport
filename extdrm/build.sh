#!/bin/bash
# Rebuild j713_extdrm.ko (needs ../dcpext2/Module.symvers and ../dispfb/Module.symvers for the exported APIs).
set -e
cd "$(dirname "$(realpath "$0")")"
make $(cat ../KMAKE) M=$PWD KBUILD_EXTRA_SYMBOLS="$PWD/../dcpext2/Module.symvers $PWD/../dispfb/Module.symvers" modules 2>&1 | grep -vE "compiler differs|kernel was built by|You are using|^make" || true
ls -la --time-style=+%H:%M:%S j713_extdrm.ko
