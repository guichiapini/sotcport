#!/usr/bin/env python3
"""Prepare Shadow of the Colossus (CUSA08809) and start it in bb-probe.

The Bloodborne launcher (run.py) also applies 1.09 patches, mods and online settings; none of
that applies here. Steps: prepare.py -> link_modules.py -> content_profile.py -> bb-probe.exe.
Environment: SOTC_GAME_DIR (folder with eboot.bin), SOTC_OUT (generated files, default
out/sotc), SOTC_USER (saves and caches, default out/sotc-user). Extra arguments go to bb-probe.
"""
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = ROOT / 'scripts'


def step(script, *args):
    result = subprocess.run([sys.executable, str(SCRIPTS / script), *map(str, args)],
                            stdin=subprocess.DEVNULL)
    if result.returncode:
        sys.exit(f'{script} failed ({result.returncode})')


def main():
    env = os.environ
    game = Path(env.get('SOTC_GAME_DIR', r'D:\Shadow of the Colossus PC\CUSA08809'))
    out = Path(env.get('SOTC_OUT', ROOT / 'out' / 'sotc'))
    user = Path(env.get('SOTC_USER', ROOT / 'out' / 'sotc-user'))
    if not (game / 'eboot.bin').is_file():
        sys.exit(f'No eboot.bin in {game} (set SOTC_GAME_DIR).')
    out.mkdir(parents=True, exist_ok=True)
    user.mkdir(parents=True, exist_ok=True)
    if env.get('SOTC_SKIP_PREPARE') != '1':
        step('prepare.py', game, '--out', out)
        step('link_modules.py', game, '--out', out)
        step('content_profile.py', game, '--out', out)
    probe = ROOT / 'out' / 'bb-probe.exe'
    clang64 = Path(env.get('MSYS2_ROOT', r'C:\msys64')) / 'clang64' / 'bin'
    env['PATH'] = f'{clang64}{os.pathsep}{env.get("PATH", "")}'
    env.setdefault('BB_CONFIG', str(user / 'sotcport.ini'))
    env.setdefault('BB_GPU_USER_DIR', str(user))
    env.setdefault('BB_VBLANK_HZ', '60')
    command = [str(probe), str(out / 'boot-linked.bin'), '--content-profile', str(out / 'content.bin'),
               '--app0', str(game), '--user', str(user), '--timeout', env.get('SOTC_TIMEOUT', '0'),
               *sys.argv[1:]]
    return subprocess.run(command, stdin=subprocess.DEVNULL).returncode


if __name__ == '__main__':
    sys.exit(main())
