#!/usr/bin/env python3
"""Install only Contra's service; preserve other title services and data."""
import ast
import hashlib
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
DEST = Path('/opt/contra-netplay')
UNIT = Path('/etc/systemd/system/contra-netplay.service')
NAME = UNIT.name

def run(*args, check=True):
    return subprocess.run(args, check=check, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

def active():
    return run('systemctl', 'is-active', '--quiet', NAME, check=False).returncode == 0

def write_atomic(path, data):
    temporary = path.with_name(path.name + '.new')
    if temporary.is_symlink() or path.is_symlink(): raise RuntimeError('Refusing symlink: ' + str(path))
    temporary.write_bytes(data);temporary.chmod(0o644);os.replace(temporary,path)

def health_check():
    packet = bytearray(96);packet[:4]=b'CDR3';packet[4]=4;packet[5]=3
    struct.pack_into('<I',packet,8,12345)
    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as sock:
        sock.settimeout(.5)
        for _ in range(12):
            sock.sendto(packet,('127.0.0.1',6475))
            try: data,_=sock.recvfrom(256)
            except (socket.timeout,ConnectionRefusedError): continue
            if len(data)==96 and data[:6]==b'CDR3\x01\x03':return
    raise RuntimeError('Contra UDP health check did not respond')

def main():
    if os.geteuid()!=0:raise SystemExit('Run with sudo python3 install_server.py')
    for name in ('contra_server.py','test_server.py','install_server.py'):
        ast.parse((HERE/name).read_text(encoding='utf-8-sig'))
    # Test before touching the running service. No title/client builds occur.
    tests=run(sys.executable,'-B',str(HERE/'test_server.py'),check=False)
    print(tests.stdout+tests.stderr)
    if tests.returncode:raise SystemExit('Server tests failed; nothing installed')
    desired_unit=(HERE/NAME).read_text(encoding='utf-8-sig')
    expected='Description=Contra NES-X matchmaking and relay'
    if expected not in desired_unit:raise SystemExit('Unexpected service file')
    if UNIT.exists() and expected not in UNIT.read_text():raise SystemExit('Existing service does not belong to Contra')
    if DEST.is_symlink() or UNIT.is_symlink():raise SystemExit('Refusing symlink destination')
    if DEST.exists() and DEST.resolve()!=DEST:raise SystemExit('Unexpected destination path')
    # Reject a port occupied by another service, rather than stopping it.
    bound=run('ss','-H','-lunp','sport = :6475').stdout.strip()
    pid=run('systemctl','show','-p','MainPID','--value',NAME,check=False).stdout.strip()
    if bound and (not pid.isdigit() or pid=='0' or ('pid='+pid+',') not in bound):
        raise SystemExit('UDP 6475 is already used by another process; nothing installed')
    was_active=active()
    was_enabled=run('systemctl','is-enabled','--quiet',NAME,check=False).returncode==0
    old_source=(DEST/'contra_server.py').read_bytes() if (DEST/'contra_server.py').exists() else None
    old_unit=UNIT.read_bytes() if UNIT.exists() else None
    stamp=time.strftime('%Y%m%d-%H%M%S')+'-'+str(os.getpid())
    backup=Path('/var/backups')/('contra-netplay-'+stamp)
    backup.mkdir(mode=0o700)
    if old_source is not None:(backup/'contra_server.py').write_bytes(old_source)
    if old_unit is not None:(backup/NAME).write_bytes(old_unit)
    DEST.mkdir(mode=0o755,exist_ok=True)
    try:
        source=(HERE/'contra_server.py').read_text(encoding='utf-8-sig').encode('utf-8')
        write_atomic(DEST/'contra_server.py',source)
        write_atomic(UNIT,desired_unit.encode('utf-8'))
        run('systemctl','daemon-reload')
        run('systemctl','enable',NAME)
        run('systemctl','restart',NAME)
        health_check()
        if not active():raise RuntimeError('Contra service did not stay active')
        if shutil.which('ufw'):
            # Add only Contra's UDP rule; never enable/reset the firewall.
            run('ufw','allow','6475/udp','comment','Contra netplay')
        print('CONTRA SERVER READY: UDP 6475. Backups: '+str(backup))
        print('If a Linode Cloud Firewall is attached, allow inbound UDP 6475 there too.')
        print('Installed SHA256: '+hashlib.sha256(source).hexdigest())
    except Exception:
        run('systemctl','stop',NAME,check=False)
        if old_source is not None:write_atomic(DEST/'contra_server.py',old_source)
        if old_unit is not None:write_atomic(UNIT,old_unit)
        elif UNIT.exists():UNIT.unlink() # exact file created above, no directory removal
        run('systemctl','daemon-reload',check=False)
        if not was_enabled:run('systemctl','disable',NAME,check=False)
        if was_active:run('systemctl','start',NAME,check=False)
        print('Installation failed; previous Contra service restored where present. Backup: '+str(backup))
        raise

if __name__=='__main__':main()
