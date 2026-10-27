#!/usr/bin/env python3
"""Audit and package a current XBE without distributing a cartridge or game art."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import struct
import zipfile

ROOT = Path(__file__).resolve().parents[2]


def digest(data):
    return hashlib.sha256(data).hexdigest()


def source_digest(path):
    return digest(path.read_text(encoding='utf-8-sig').replace('\r\n', '\n').encode('utf-8'))


def check_plan(plan):
    if not plan:
        return
    for name, expected in plan['source_sha256'].items():
        path = (ROOT / name).resolve()
        if ROOT not in path.parents or source_digest(path) != expected:
            raise RuntimeError('Source differs from the release commit: ' + name)


def audit_xbe(path):
    data = path.read_bytes()
    if data[:4] != b'XBEH':
        raise RuntimeError('Not an Xbox executable: ' + str(path))
    inputs = [p for p in (ROOT / 'src').rglob('*') if p.suffix in ('.c', '.h')]
    inputs += [ROOT / n for n in ('setup.py', 'tools/xbox/make_icons.py',
               'tools/xbox/gen_manifest.py', 'tools/xbox/xbconfig.py',
               'Platform/xbox/titleimage.rdf', 'Platform/xbox/saveimage.rdf')]
    if path.stat().st_mtime_ns < max(p.stat().st_mtime_ns for p in inputs):
        raise RuntimeError('The XBE predates current source or artwork. Run a fresh build first.')
    if 'Xontra'.encode('utf-16-le') not in data:
        raise RuntimeError('The XBE does not contain the current Xontra dashboard title.')
    header = (ROOT / 'src/xbox/netplay_build.h').read_text(encoding='utf-8')
    match = re.search(r'CNX_NET_BUILD\s+(0x[0-9a-fA-F]+)', header)
    if not match or struct.pack('<I', int(match[1], 16)) not in data:
        raise RuntimeError('The XBE does not match the current netplay source fingerprint.')
    spec = importlib.util.spec_from_file_location('xontra_icons', ROOT / 'tools/xbox/make_icons.py')
    icons = importlib.util.module_from_spec(spec); spec.loader.exec_module(icons)
    resources = {}
    for stem, size in [('titleimage', 128), ('saveimage', 64)]:
        bmp = ROOT / 'Platform/xbox/gen' / (stem + '.bmp')
        resource = bmp.with_suffix('.xbx')
        if bmp.read_bytes() != icons.bitmap(size):
            raise RuntimeError('Dashboard artwork is not the original Xontra icon: ' + stem)
        if resource.stat().st_mtime_ns < bmp.stat().st_mtime_ns:
            raise RuntimeError('Dashboard resource is stale: ' + stem)
        packed = resource.read_bytes()
        if not packed or packed not in data:
            raise RuntimeError('The XBE does not embed the audited dashboard resource: ' + stem)
        resources[stem] = digest(packed)
    # Compare against local cartridges without copying them into any release.
    checked = set()
    for rom in [ROOT / 'baserom.nes', ROOT / 'build/Contra.nes', ROOT / 'data/Contra.nes']:
        if not rom.is_file():
            continue
        cartridge = rom.read_bytes(); identity = digest(cartridge)
        if identity in checked:
            continue
        checked.add(identity)
        if cartridge in data:
            raise RuntimeError('Cartridge bytes were embedded in the XBE.')
        for offset in range(16, len(cartridge) - 4095, 4096):
            block = cartridge[offset:offset + 4096]
            if len(set(block)) > 16 and block in data:
                raise RuntimeError('Cartridge asset block was embedded in the XBE.')
    return data, resources


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--version', required=True)
    parser.add_argument('--configuration', choices=['Release', 'Debug'], default='Release')
    parser.add_argument('--out', type=Path)
    parser.add_argument('--plan', type=Path, help='Optional pinned source manifest for an official release')
    parser.add_argument('--check-source', action='store_true', help='Validate the pinned source without building or packaging')
    args = parser.parse_args()
    if not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9.-]+)?', args.version):
        parser.error('Version must be a semantic version without the leading v')
    plan = json.loads(args.plan.read_text(encoding='utf-8-sig')) if args.plan else None
    if plan and args.version != plan['version']:
        raise RuntimeError('Version differs from the release plan')
    check_plan(plan)
    if args.check_source:
        print('Pinned release source verified. No build or package created.'); return
    project = json.loads((ROOT / 'rxdk.project.json').read_text(encoding='utf-8'))['name']
    executable = ROOT / 'out' / args.configuration / 'Build' / project / 'default.xbe'
    data, resources = audit_xbe(executable)
    out = (args.out or ROOT / 'out/public-release' / args.version).resolve()
    out.mkdir(parents=True, exist_ok=True)
    name = 'Xontra-v' + args.version + '-Xbox.zip'
    readme = '''Xontra - Original Xbox

This download contains the executable and notices. No game ROM or cartridge
artwork is included. Dashboard artwork is original to this port.

Extract all files into one Xbox game folder. Add a compatible Contra (US) NES
cartridge dump separately as Contra.nes beside default.xbe, or data/Contra.nes.
Start default.xbe. A missing cartridge shows an installation screen.

Online Play offers public Open and four-digit Passcode rooms. The host presses
A to start after both players connect, then selects 2 PLAYERS at the NES title.
Both consoles need the same client build and the compatible protocol-v3 service.

Source, controls, build instructions and license notices:
https://github.com/sirdankz/Xontra
'''
    entries = {'default.xbe': data, 'README.txt': readme.encode('utf-8'),
               'LICENSE': (ROOT / 'LICENSE').read_bytes(), 'NOTICE': (ROOT / 'NOTICE').read_bytes()}
    pending = out / (name + '.new')
    with zipfile.ZipFile(pending, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for member, value in entries.items():
            archive.writestr(member, value)
    with zipfile.ZipFile(pending) as archive:
        if archive.testzip() is not None or set(archive.namelist()) != set(entries):
            raise RuntimeError('Release archive validation failed')
        if any(archive.read(member) != value for member, value in entries.items()):
            raise RuntimeError('Release archive content mismatch')
    pending.replace(out / name)
    (out / 'default.xbe').write_bytes(data)
    sums = {name: digest((out / name).read_bytes()), 'default.xbe': digest(data)}
    (out / 'SHA256SUMS.txt').write_text(''.join(value + '  ' + member + '\n' for member, value in sums.items()), encoding='ascii')
    print('ROM-free release package: ' + str(out / name))
    print('Audited original dashboard resources: ' + json.dumps(resources, sort_keys=True))


if __name__ == '__main__':
    main()
