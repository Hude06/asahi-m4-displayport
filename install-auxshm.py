#!/usr/bin/env python3
"""Install (or roll back) the J713 AUX_SHM-tunable stage2. Run as root.

  sudo python3 -I install-auxshm.py            # inspect only, writes nothing
  sudo python3 -I install-auxshm.py --apply    # install
  sudo python3 -I install-auxshm.py --rollback # restore boot.bin.pre-auxshm

Replaces exactly one boot.bin segment, the loader (m1n1 stage2), and only when
the installed loader is the notch1-dcpext1 stage2 and the DTB is dcpext0 v2.
The new stage2 differs only in kboot_atc.c: it also copies the ADT
tunable_AUX_SHM of atc-phy,t8132 nodes into "apple,tunable-aux-shm" (offset
core+0x16400), non-fatally. Bootargs, kernel, DTB and initramfs are copied
byte-for-byte. The current boot.bin is kept as boot.bin.pre-auxshm and also
becomes boot.bin.previous (the file the documented macOS recovery restores).
"""
import hashlib
import sys as _sys
_sys.dont_write_bytecode = True
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ART = HERE / 'auxshm'

BOOT = Path('/boot/efi/omarchy/boot.bin')
BACKUP = BOOT.with_name('boot.bin.pre-auxshm')
PREVIOUS = BOOT.with_name('boot.bin.previous')
# installed published backlight stage2 -> dcpext build of the same notch variant
LOADERS = {'0432c3825b6843e3ee0992458d1244b8d45d4638af68036c5b5600ea21a55dec'}  # notch1-dcpext1
DTBS = {'18bb6c6e9c1b3b83cc700f8530e2813ca9f4c16dac0cb296fecd42ddeb335723'}  # dcpext0 v2
NEW_LOADER = 'stage2-backlight1-notch1-dcpext1-auxshm1.bin'


def sha(b):
    return hashlib.sha256(b).hexdigest()


def load_compose():
    import importlib.util
    path = HERE.parent / 'Omarchy-M4-Native/tools/m4-boot-compose.py'
    spec = importlib.util.spec_from_file_location('compose', path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def pinned(name):
    sums = dict(reversed(l.split()) for l in (ART / 'SHA256SUMS').read_text().splitlines() if l)
    data = (ART / name).read_bytes()
    if sha(data) != sums[name]:
        sys.exit(f'refusing: {name} does not match SHA256SUMS')
    return data


def write_atomic(path, data):
    tmp = path.with_name(path.name + '.new')
    with open(tmp, 'wb') as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)
    fd = os.open(path.parent, os.O_RDONLY)
    os.fsync(fd)
    os.close(fd)


def main():
    if os.geteuid() != 0:
        sys.exit('run with sudo')
    compose = load_compose()
    mode = sys.argv[1] if len(sys.argv) > 1 else '--inspect'

    if mode == '--rollback':
        if not BACKUP.exists():
            sys.exit(f'no {BACKUP}')
        data = BACKUP.read_bytes()
        compose.parse(data)
        write_atomic(BOOT, data)
        print(f'restored {BOOT} from {BACKUP} ({sha(data)})')
        return

    current = BOOT.read_bytes()
    layout = compose.parse(current)
    print(f'boot.bin   {sha(current)} ({len(current)} bytes)')
    for k in ('loader', 'bootargs', 'kernel', 'dtb', 'initramfs'):
        print(f'  {k:<9} {sha(layout[k])} {len(layout[k])} bytes')
    if sha(layout['loader']) not in LOADERS or sha(layout['dtb']) not in DTBS:
        sys.exit('refusing: installed loader/devicetree are not the notch1-dcpext1 stage2 + '
                 'dcpext0 v2 DTB this build is based on')

    loader = pinned(NEW_LOADER)
    new = dict(layout, loader=loader)
    data = compose.join(new)
    check = compose.parse(data)
    for k in ('bootargs', 'kernel', 'dtb', 'initramfs'):
        if check[k] != layout[k]:
            sys.exit(f'refusing: {k} would change')
    if check['loader'] != loader:
        sys.exit('refusing: composed object does not round-trip')
    print(f'new        {sha(data)} ({len(data)} bytes): loader replaced, '
          'bootargs/kernel/dtb/initramfs identical')
    if mode != '--apply':
        print('inspection only; rerun with --apply to install')
        return

    free = os.statvfs(BOOT.parent)
    if free.f_bavail * free.f_frsize < 3 * len(data):
        sys.exit('refusing: not enough space on the EFI partition')
    write_atomic(BACKUP, current)
    write_atomic(PREVIOUS, current)
    write_atomic(BOOT, data)
    if sha(BOOT.read_bytes()) != sha(data):
        write_atomic(BOOT, current)
        sys.exit('write verification failed; original restored')
    print(f'installed. backup: {BACKUP} and {PREVIOUS}')


if __name__ == '__main__':
    main()
