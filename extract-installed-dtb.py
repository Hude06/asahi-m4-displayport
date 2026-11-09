#!/usr/bin/env python3
"""Read-only: copy the devicetree segment of the installed boot.bin to
~/Work/m4/dp-altmode/installed.dtb so it can be compared. Run with sudo."""
import hashlib, importlib.util, os, pathlib, pwd, sys
sys.dont_write_bytecode = True
HERE = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location(
    'compose', HERE.parent / 'Omarchy-M4-Native/tools/m4-boot-compose.py')
c = importlib.util.module_from_spec(spec)
spec.loader.exec_module(c)
layout = c.parse(pathlib.Path('/boot/efi/omarchy/boot.bin').read_bytes())
out = HERE / 'installed.dtb'
out.write_bytes(layout['dtb'])
user = os.environ.get('SUDO_USER')
if user:
    pw = pwd.getpwnam(user)
    os.chown(out, pw.pw_uid, pw.pw_gid)
print(f'{out}: {hashlib.sha256(layout["dtb"]).hexdigest()}')
