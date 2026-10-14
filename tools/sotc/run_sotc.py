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


def kill_with_this_process(process):
    """Windows: the game ends with this script (killed by timeout, console closed), through a
    job object that kills its processes when its last handle closes."""
    if os.name != 'nt':
        return
    import ctypes
    from ctypes import wintypes
    kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel32.CreateJobObjectW.restype = wintypes.HANDLE
    kernel32.OpenProcess.restype = wintypes.HANDLE

    class Limits(ctypes.Structure):
        _fields_ = [('PerProcessUserTimeLimit', ctypes.c_int64), ('PerJobUserTimeLimit', ctypes.c_int64),
                    ('LimitFlags', wintypes.DWORD), ('MinimumWorkingSetSize', ctypes.c_size_t),
                    ('MaximumWorkingSetSize', ctypes.c_size_t), ('ActiveProcessLimit', wintypes.DWORD),
                    ('Affinity', ctypes.c_size_t), ('PriorityClass', wintypes.DWORD),
                    ('SchedulingClass', wintypes.DWORD)]

    class ExtendedLimits(ctypes.Structure):
        _fields_ = [('Basic', Limits), ('Io', ctypes.c_uint64 * 6), ('ProcessMemoryLimit', ctypes.c_size_t),
                    ('JobMemoryLimit', ctypes.c_size_t), ('PeakProcessMemoryUsed', ctypes.c_size_t),
                    ('PeakJobMemoryUsed', ctypes.c_size_t)]
    job = kernel32.CreateJobObjectW(None, None)
    limits = ExtendedLimits()
    limits.Basic.LimitFlags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    kernel32.SetInformationJobObject(job, 9, ctypes.byref(limits), ctypes.sizeof(limits))
    handle = kernel32.OpenProcess(0x1F0FFF, False, process.pid)
    if not handle or not kernel32.AssignProcessToJobObject(job, handle):
        print('run_sotc: the game is not tied to this script (job object failed)')
    kill_with_this_process.job = job  # kept open for the life of this script


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
    # The renderer's Bloodborne features hook that game's shaders and render targets
    # (temporal upscalers, object motion through instrumented vertex shaders, live render
    # resolution); CUSA08809 starts with all of them off.
    config = Path(env['BB_CONFIG'])
    if not config.exists():
        config.write_text('# sotcport settings\nupscaler=off\nobject_motion=0\nlive_resolution=0\n'
                          'show_fps=1\n', encoding='utf-8')
    env.setdefault('BB_DLSS', '0')
    env.setdefault('BB_LIVE_RES', '0')
    # Shaders read guest memory directly when an offset is only known as they run: the
    # culling compute shaders index their tables in loops, and the flattened copy they read
    # otherwise culled the scenery away. Pipeline caches do not record this setting, so each
    # value keeps its own GPU folder.
    env.setdefault('BB_DIRECT_MEMORY_ACCESS', '1')
    env.setdefault('BB_GPU_USER_DIR', str(user / f"gpu-dma{env['BB_DIRECT_MEMORY_ACCESS']}"))
    # The game decides on the CPU which scenery to draw from results its compute shaders
    # wrote (visibility): without precise readbacks the CPU sees stale memory and the main
    # view loses all terrain while the shadow maps keep it.
    env.setdefault('BB_READBACKS', '2')
    env.setdefault('BB_VBLANK_HZ', '60')
    env.setdefault('BB_HEARTBEAT', '10')
    # Guest memory copies run on the GPU command thread, the one thread whose protection
    # faults are handled inline. On copy threads (BB_COPY_THREADS) or the Vulkan recording
    # thread, a copy whose source the GPU wrote waited for the GPU thread to download it while
    # that thread waited for the copy: CUSA08809 hung about one run in four. Toggle bits:
    # 16 threaded recording, 256 and 16384 copies on the recording thread, 131072 and 524288
    # copy threads (see src/runtime_memory.c).
    env.setdefault('BB_COPY_THREADS', '0')
    if 'BB_TOGGLE_FILE' not in env:
        toggles = out / 'toggles.txt'
        toggles.write_text(str(16 | 256 | 16384 | 131072 | 524288) + '\n')
        env['BB_TOGGLE_FILE'] = str(toggles)
    command = [str(probe), str(out / 'boot-linked.bin'), '--content-profile', str(out / 'content.bin'),
               '--app0', str(game), '--user', str(user), '--timeout', env.get('SOTC_TIMEOUT', '0'),
               *sys.argv[1:]]
    # The output also goes to <user>/last_run.log, for runs started by hand.
    log_path = user / 'last_run.log'
    with open(log_path, 'wb') as log:
        process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT)
        kill_with_this_process(process)
        for line in process.stdout:
            log.write(line)
            log.flush()
            sys.stdout.buffer.write(line)
            sys.stdout.buffer.flush()
        code = process.wait()
        message = f'run_sotc: game exited with code {code} (0x{code & 0xffffffff:08x})\n'.encode()
        log.write(message)
        sys.stdout.buffer.write(message)
        return code


if __name__ == '__main__':
    sys.exit(main())
