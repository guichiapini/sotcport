# Shadow of the Colossus (CUSA08809) on the Bloodborne port

This branch (`sotc`) runs the PS4 remake of Shadow of the Colossus (EU, CUSA08809, 01.01)
natively on Windows with the bbport runtime: the game's x86-64 code runs as is, its system
library calls are answered by `src/runtime_*.c`, and its GPU command buffers go through the
vendored shadPS4 renderer (Vulkan) in `gpu/`.

## Running

```
bash build.sh                     # MSYS2 CLANG64 (see msys2-env.sh)
python tools/sotc/run_sotc.py     # prepares out/sotc from the dump on first use, then starts
```

`run_sotc.py` reads the dump from `D:\Shadow of the Colossus PC\CUSA08809` (`SOTC_GAME_DIR` to
change it), keeps saves and caches in `out/sotc-user`, and sets the defaults the game needs:

| Variable | Default | Why |
|---|---|---|
| `BB_READBACKS` | `2` (precise) | The game culls scenery on the CPU from visibility its compute shaders write; without reading GPU-written memory back, the main view loses all terrain. |
| `BB_DIRECT_MEMORY_ACCESS` | `1` | Culling shaders index guest tables at offsets only known as they run. |
| `BB_GPU_USER_DIR` | `out/sotc-user/gpu-dma1` | Pipeline cache per direct-memory setting. |
| `BB_COPY_THREADS` | `0` | Guest copies on the GPU thread. |
| `BB_HEARTBEAT` | `10` | Frames and read rate every 10 s; a thread dump when frames stop. |

Bloodborne-specific renderer features (upscaler, object motion, live resolution) are off in
the generated `sotcport.ini`.

## Tools (`tools/sotc/`)

| Tool | What it does |
|---|---|
| `capture.sh [s...]` | Runs the game, dumps the presented frame at the given seconds. `SOTC_PAD="50:tap=options 53:tap=up 55:tap=cross"` scripts input (menus need short taps); `"90:frame"` records the frame analyzer. |
| `smoke.sh` | Boot → skip the intro (pause menu) → title → main menu, with a one-page report (`smoke_report.py`): FPS, GPU time per pass, readbacks, Ngs2 parameters, audio level per port, how the run ended. |
| `sheet.py <dir>` | Contact sheet of a capture. |
| `raw2png.py` | Converts frame/target dumps (`BB_DUMP_IMAGES=<hex address>,...`). |
| `hang_report.py <log>` | Names the frames of a heartbeat thread dump. |
| `prof_report.py <log>` | Names a `BB_PROFILE=<start>:<seconds>` sampling profile (draw recording and GPU command threads). |
| `psarc.py list/extract` | Reads the game's `pak*.psarc` archives. |

Diagnostics switches: `BB_FRAME_STATS=1`, `BB_GPU_PROFILE=1`, `BB_READ_FAULT_STATS=1|2`,
`BB_INDIRECT_PROBE=N`, `BB_BUFFER_STATS=1`, `BB_NGS2_TRACE=N`, `BB_AUDIO_DUMP=<prefix>`,
`BB_LOOP_GUARD=N` (SPIR-V loop bound, default 65536; clear the GPU cache after changing),
`BB_UNBOUNDED_CAP_MB=N` (unbounded read-only buffer bindings, default 64).

## What it took (by area)

- **Loading**: the dump's import names are spelled per game (`NID#lib#mod`); `link_modules.py`
  respells them for the runtime. The image is mapped below 2 GiB (the game stores 31-bit
  pointers to static data); the game maps fixed memory at 0x7f0000000.
- **Threads**: SndX and the job system need event flags with hand-off semantics; fibers run on
  host stacks (guest stacks among GPU-tracked pages died silently).
- **GPU memory**: precise readbacks, made workable by stepping reads over page protection when
  the GPU did not write the bytes read (single-step trap), by hashing textures through the
  backing view, and by never waiting on the GPU from a speculative read.
- **Resource tables**: pointers keep 40 address bits and dword alignment, as the GPU decodes
  them; a walk through unfilled tables yields null descriptors instead of garbage ones.
- **GPU robustness**: translated loops are bounded (a loop that never exits reset the device);
  `VK_EXT_device_fault` is reported on device loss; unbounded buffers are capped.
- **Audio**: SndX renders through Ngs2 (`src/runtime_sotc_ngs2.c`): sampler voices (PS ADPCM —
  the banks are `.hd/.bd` — PCM, ATRAC9), submixers and mastering into the buffers the game
  hands to AudioOut. `tests/test_ngs2.c` checks the graph; `NGS2_TEST_VAG=<.bd>` decodes a bank.

## State and open items

- Boots, plays the opening cinematic with full scenery and menus with text, reaches the title
  screen and the main menu.
- Main menu ran at ~0.5 FPS: readbacks with full GPU waits (step-over added, unmeasured) and
  GPU submissions taking seconds (a loop at the guard is the suspect: see `BB_GPU_PROFILE`).
- Audio: Ngs2 graph written and unit-tested; the parameter IDs it assumes are checked against
  the first 200 the game sends (`Ngs2 param` lines).
- Gameplay (New Game) not reached yet.
