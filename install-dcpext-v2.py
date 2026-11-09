#!/usr/bin/env python3
"""Install (or roll back) the J713 dcpext0 v2 devicetree. Run as root.

  sudo python3 -I install-dcpext-v2.py            # inspect only, writes nothing
  sudo python3 -I install-dcpext-v2.py --apply    # install
  sudo python3 -I install-dcpext-v2.py --rollback # restore boot.bin.pre-dcpext-v2

Replaces exactly one boot.bin segment, the devicetree, and only when the
installed loader is a dcpext stage2 and the installed DTB is dcpext0 v1.
Loader, bootargs, kernel and initramfs are copied byte-for-byte. The current
(v1, known-good) boot.bin is kept as boot.bin.pre-dcpext-v2 and also becomes
boot.bin.previous (the file the documented macOS recovery restores).
"""
import hashlib
import sys as _sys
_sys.dont_write_bytecode = True
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent

BOOT = Path('/boot/efi/omarchy/boot.bin')
BACKUP = BOOT.with_name('boot.bin.pre-dcpext-v2')
PREVIOUS = BOOT.with_name('boot.bin.previous')
# installed published backlight stage2 -> dcpext build of the same notch variant
LOADERS = {
    '701253576f1c22bd4d9e7af87e7164db7743ca05359fd47baed3701552f8f66f',  # dcpext1
    '0432c3825b6843e3ee0992458d1244b8d45d4638af68036c5b5600ea21a55dec',  # notch1-dcpext1
}
OLD_DTBS = {'9ca9c91303b2438adde97550d6eaad8f14569ea44a156568079a24c046bad782',  # dcpext0 v1 (release copy)
            'bb26c478a1ce659709896b6f140b3a1cef0d7ec3dacd3a25d32f5d5f0974b4d1'}  # dcpext0 v1 as installed
NEW_DTB = 'j713-dcpext0-v2.dtb'


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
    sums = dict(reversed(l.split()) for l in (HERE / 'SHA256SUMS').read_text().splitlines() if l)
    data = (HERE / name).read_bytes()
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
    if sha(layout['loader']) not in LOADERS or sha(layout['dtb']) not in OLD_DTBS:
        sys.exit('refusing: installed loader/devicetree are not the dcpext stage2 + '
                 'dcpext0 v1 DTB this build is based on')

    dtb = pinned(NEW_DTB)
    new = dict(layout, dtb=dtb)
    data = compose.join(new)
    check = compose.parse(data)
    for k in ('loader', 'bootargs', 'kernel', 'initramfs'):
        if check[k] != layout[k]:
            sys.exit(f'refusing: {k} would change')
    if check['dtb'] != dtb:
        sys.exit('refusing: composed object does not round-trip')
    print(f'new        {sha(data)} ({len(data)} bytes): dtb replaced, '
          'loader/bootargs/kernel/initramfs identical')
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
