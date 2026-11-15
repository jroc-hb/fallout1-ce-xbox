# AGENTS.md

Notes for AI agents working on this repo: an Original Xbox port of Fallout 1 CE, built with the
NXDK and the hardware-accelerated nxdk-sdl3 port. The current work branch is `SDL3`. PRs go to
`Xbox-Dev`.

## Environment

- Development happens in **WSL2 (Ubuntu 24.04)** on Windows 11. XEMU runs on the **Windows** side.
- Repo: `~/xbox/projects/fallout1-ce-xbox`
- NXDK: `~/xbox/nxdk` (toolchain: `~/xbox/nxdk/share/toolchain-nxdk.cmake`)
  - Branch **`pbkit-vblank-flip-fix`** (off `master`) fixes a pbkit bug: the ISR only flips when it is
    entered for the vblank, so a vblank landing while a GPU interrupt's DPC was pending retired the
    frame without showing it (the old 2-4 s "stuck in PCRTC loop" stalls). **This build no longer
    needs it**: nxdk-sdl3's renderer now flips by itself and never calls `pb_finished()` (see Video).
  - nxdk libraries are prebuilt. After switching nxdk branches, rebuild pbkit with
    `make NXDK_ONLY=y NXDK_DIR=$HOME/xbox/nxdk $HOME/xbox/nxdk/lib/libpbkit.lib` (in `~/xbox/nxdk`,
    with clang 21 on PATH), then delete `build/fallout-ce.exe` so the game relinks.
- nxdk-sdl3: `~/xbox/nxdk-sdl3` (vendors SDL under `SDL/`, with the Xbox backends in `nxdk_glue/`)
  - It's a checkout of `Ryzee119/nxdk-sdl3`. **The build uses the local branch `master-nxaudio-fallout`**:
    upstream `master` with upstream's unmerged `nxaudio` branch merged in, so audio goes through
    nxdk-audio (the MCPX APU) instead of the old AC97 `hal/audio`. It also carries a local fix to
    `nxdk_glue/audio/SDL_xboxaudio.c` (see Known issues). When updating, pull `master`, then merge
    or rebase `master-nxaudio-fallout` onto it.
  - **Dev branch `xgu-paletted-textures`** (off `master-nxaudio-fallout`, currently checked out) adds
    INDEX8 texture support with GPU palette lookup to the xgu renderer, and replaces pbkit's
    `pb_finished()` flip queue with a driver-owned double buffer. The game's Xbox renderer depends
    on it. Merge it into `master-nxaudio-fallout` once it's verified.
  - After any pull, also run `git submodule update --init --recursive`, or SDL stays on the old
    version. Then re-run `nxdk-cmake` in `build/`.
  - nxdk-audio is fetched from GitHub by nxdk-sdl3's CMake at configure time (into
    `build/_deps/nxdk_audio-src`).
  - CMake finds it automatically (`../../nxdk-sdl3` relative to the repo). You can override it
    with `-DNXDK_SDL3_DIR=...`.
- **Compiler: use clang 21.** nxdk-audio uses C23 `#embed`, so clang 18 (the system default `clang`)
  fails with `invalid preprocessing directive`. clang 19.x–20.1.2 has an optimizer bug (the
  `activate` script warns about it). clang 21/22/23 are installed under `/usr/lib/llvm-NN/bin`.
  nxdk's `nxdk-cc`/`nxdk-link` call plain `clang`/`lld` from PATH, so put llvm-21 first in PATH.
  If you switch compilers, wipe `build/`, because CMake caches the compiler version.
- **Rust:** nxdk-audio builds its DSP assembler (`dsp56300-asm`) with `cargo`. Rust is installed
  user-local via rustup (`~/.cargo`). Load it with `. ~/.cargo/env`.
- `third_party/sdl3/` is only used for **non-Xbox** builds. Xbox builds always use nxdk-sdl3.

## Building

`~/xbox/nxdk/bin/activate` **`exec`s a new shell**, so running it as a command inside a
non-interactive tool call does nothing useful. Use its `-s` flag to eval the exports instead:

```bash
cd ~/xbox/projects/fallout1-ce-xbox
export PATH=/usr/lib/llvm-21/bin:$PATH     # clang 21 (needed for nxdk-audio's #embed)
. ~/.cargo/env                             # cargo, for nxdk-audio's DSP assembler
eval "$(~/xbox/nxdk/bin/activate -s)"      # sets NXDK_DIR and puts nxdk-cmake etc. on PATH
mkdir -p build && cd build
nxdk-cmake -DCMAKE_BUILD_TYPE=Release ..   # or Debug
make -j"$(nproc)"
```

- A clean build takes about 20 s wall time with `-j20` (including SDL3 and nxdk-audio's cargo
  step). An incremental build takes a few seconds.
- `build/` already exists and is configured for Release. Reuse it.
- **Outputs are written into the source tree, not into the build dir**, regardless of which build
  dir you use:
  - `build/fallout-ce.exe`: the PE linked by nxdk
  - `fallout-ce/default.xbe`: converted by `cxbe` (the `cxbe_convert` target)
  - `fallout-ce.iso`: packed from the whole `fallout-ce/` folder by `extract-xiso` (the `xbe_iso` target)
- The ISO is about 575 MB because `fallout-ce/` holds the user-supplied game data (`MASTER.DAT`,
  `CRITTER.DAT`, `DATA/`). These are gitignored and must never be committed.
  `fallout-ce/SDL3.xbe` is a stale leftover and not part of the build.
- Expected warnings that are not regressions:
  - many `-Wdeprecated-ofast` (clang 21 deprecates the game's `-Ofast`)
  - `-Wkeyword-macro`
  - `NV097_SET_COLOR_MATERIAL macro redefined` (from nxdk-sdl3 xgu.h)
  - `-Wwritable-strings` in `debug.cc` and `-Wswitch` in `mouse.cc`
  - from the nxdk-audio DSP assembler: `InstructionInInterruptVector`
  - from cargo: `failed to connect to jobserver`
  - from lld: `ignoring unknown argument '-Ofast'` and `.edata=.rdata already merged`

## Running in XEMU

XEMU lives at `E:\Xbox\Development\XEMU\xemu.exe` (`/mnt/e/Xbox/Development/XEMU/xemu.exe` from
WSL). Its config is `C:\Users\justi\AppData\Roaming\xemu\xemu\xemu.toml`. That config already
sets the BIOS/MCPX/HDD paths, `mem_limit = 128`, keyboard on controller port 1, and a default
`dvd_path` pointing at this repo's ISO.

The user's normal command, in a Windows shell:

```
E:\Xbox\Development\XEMU\./xemu.exe -device lpc47m157 -serial stdio -dvd_path <iso>
```

The command that works for agents, run from WSL as a **background** task. It captures the serial
debug output to a file and opens a QMP socket:

```bash
cd /mnt/e/Xbox/Development/XEMU
./xemu.exe -device lpc47m157 -serial stdio \
  -qmp tcp:127.0.0.1:4444,server,nowait \
  -dvd_path "$(wslpath -w ~/xbox/projects/fallout1-ce-xbox/fallout-ce.iso)" \
  > /path/to/serial.log 2>&1
```

- `-device lpc47m157 -serial stdio` routes the game's `DbgPrint` and debug output to stdout, so it
  ends up in the log file.
- The ISO is read over `\\wsl.localhost\Ubuntu-24.04\...`, so there's no need to copy it to Windows.
- The XEMU window is visible to the user, and they may close it themselves. Tell them when you're
  still testing and when you're finished. If XEMU exits with code 0 and the log has no crash, the
  user most likely closed it.
- **Close XEMU before rebuilding.** The build rewrites the ISO that XEMU is reading. (Whether
  Windows' file lock makes `xbe_iso` fail hasn't been verified.) To kill XEMU from WSL:
  `taskkill.exe /IM xemu.exe /F`.
- The boot to the main menu takes about 15–20 s. A healthy boot log contains this sequence:
  `##### Starting Fallout #####` → `Mounted D:/E: success` → save-slot sync →
  `game_init_databases` → `>gsound_init` … `>init_options_menu` → `game_init done` →
  `main_selfrun_init` → `Opening game controller for joystick N` (once per pad) →
  `Added joystick ... Original Xbox Controller #1`. If `Opening game controller` is missing, pads
  plugged in at boot won't work (see the gamepad note under Code map).
- **The serial log can contain NUL bytes.** Plain `grep` then treats it as binary and prints
  nothing, or just "Binary file matches". Always use `grep -a`.
- `xemu.toml` binds a host gamepad to port 3, so a second `Original Xbox Controller #2` is normal.
- Messages in the log that are normal: `FONT4-9.FON` / `FONT5-15.AAF` "not found" errors, an
  `OPTIDN.FRM` fopen ALERT from the patches dir, and the XEMU host lines `FBO incomplete` / `win32_dxgi
  present failed`.
- Save data and the live `fallout.cfg`/`f1_res.ini` are on the emulated HDD at `E:\UDATA\FALLOUT1`
  (inside `xbox_hdd.qcow2`). After the first run, the copy in `fallout-ce/` only seeds a fresh HDD.
  The README says FMVs used to crash XEMU and the workaround was `[debug] disable_fmv=1`. The
  current build boots with `disable_fmv=0`.

### Screenshots and input (helper scripts in `E:\Xbox\Development\XEMU\`)

Run them from WSL with `powershell.exe -NoProfile -ExecutionPolicy Bypass -File '<win path>' ...`:

- `grab.ps1 -Out 'E:\Xbox\Development\XEMU\grab.png'` captures the XEMU window's client area
  (1280x960). Then view `/mnt/e/Xbox/Development/XEMU/grab.png` with the Read tool. It prints
  `bad rect` when the window has no usable client area, for example while it is closing or
  minimized (it uses `SW_SHOW`, not `SW_RESTORE`). First check that XEMU is still running, then ask
  the user to restore the window.
- `sendkeys.ps1 -Keys 'down,down,ret'` sends QEMU qcodes over QMP (port 4444, so XEMU must have
  been started with `-qmp`). Port 1 is bound to the keyboard in `xemu.toml`, so keys map to the
  emulated controller according to XEMU's keyboard bindings. **In practice these presses don't reach
  the game**: with logging on `SDL_EVENT_GAMEPAD_BUTTON_*`, no events arrived, focused window or not.
- What does work: real Windows key events to the focused XEMU window. Focus it (run `grab.ps1`), then
  from PowerShell call `user32!keybd_event(vk, MapVirtualKey(vk,0), flags, 0)` (down: flags `1` for
  extended keys such as arrows, up: `3`). XEMU's keyboard mapping turns them into pad input (down
  arrow = d-pad down), including holds and the game's key repeat.
- To catch single-frame glitches, capture a small strip of the window in a tight PowerShell loop
  (`Graphics.CopyFromScreen` into bitmaps in a list, saved after the loop): about 100-120 captures/s,
  so every 60 Hz frame is sampled. Then classify the frames offline (Pillow is installed; numpy isn't).
- Timing-sensitive bugs can vanish under instrumentation. A debug thread polling every 1 ms hid the
  frame-bounce bug completely. Prefer in-memory event rings checked by the existing 1 s watchdog.
- `qmp_shot.ps1` does **not** work: xemu's monitor has no `screendump` command. Use `grab.ps1`.

## Code map (Xbox-specific bits)

All Xbox code is behind `#ifdef NXDK` / `XBOX`. The files with the most Xbox-specific code:
`src/plib/gnw/svga.cc` (renderer), `src/game/loadsave.cc` (UDATA save slots),
`src/platform_compat.cc` (paths/FS), `src/game/mainmenu.cc` (XBOX OPTS menu),
`src/plib/gnw/gamepad.cc`, `src/plib/gnw/winmain.cc` (startup/mounts), `src/audio_engine.cc`.

SDL-created threads on nxdk-sdl3 get very small stacks (about 4 KB), so keep large buffers off them.

**Video mode / resolution.**
- `f1_res.ini` lives at `E:\UDATA\FALLOUT1` and is written by the XBOX OPTS menu (`main.cc`). It
  sets `SCR_WIDTH`/`SCR_HEIGHT`.
- `svga_init` creates the SDL window at that size **without `SDL_WINDOW_FULLSCREEN`**. With that
  flag, SDL3 swaps in the display bounds, and the nxdk driver's primary display is 640×480, so 720p
  silently becomes 480p.
- The nxdk-sdl3 driver (`nxdk_glue/video/SDL_xboxvideo.c`) maps requests below 1280×720 to 480p and
  anything else to 720p. It never sets 1080i, although nxdk lists 1920×1080.
- The screen texture is **8-bit paletted** (`SDL_PIXELFORMAT_INDEX8`, NV2A `SZ_I8_A8R8G8B8`), sharing
  `gSdlSurface`'s palette, so the GPU does the palette lookup and palette changes (color cycling
  every 33 ms in-game, fades) cost no CPU. `GNW95_ShowRect` copies into `gSdlSurface` and grows a
  dirty rect; `renderPresent` uploads that rect (aligned to 8x4) once per frame. Anything that writes
  to `gSdlSurface` directly must call `svgaMarkDirty`.
- Presentation (`XBOX_RenderPresent` in nxdk-sdl3) is a driver-owned double buffer: wait for the GPU
  to idle, write `PCRTC_START` right after a vblank, render the next frame into the other buffer. It
  replaced pbkit's interrupt-driven `pb_finished()` flip queue, which under XEMU load (paletted
  re-uploads from color cycling) sometimes rendered into or showed the wrong buffer. An older frame
  then flashed for one frame (sprites "bouncing back"). It's synchronous with vblank: one frame
  queued at most, so a frame taking longer than ~16 ms drops to 30 fps.
- The game renders at the native mode size, so frames are shown 1:1. The texture uses
  `SDL_SCALEMODE_NEAREST`, which the xgu renderer honours, so any scaling would also be unfiltered.
- Shortly after the main menu appears, SDL sends a spurious `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED`
  (1280×720 → 1280×720). Recreating the renderer for it caused a black screen at 720p, so
  `handleWindowSizeChanged()` does nothing on NXDK. Don't tear down or recreate the xgu renderer
  mid-session.
- `debugPrint` (nxdk `hal/debug.h`) draws on screen, not to serial. Use `DbgPrint` or `debug_printf`
  for the serial log.

**Gamepad mapping must be registered before `SDL_INIT_JOYSTICK`.** The Original Xbox pad needs the
game's `default` mapping (`GamepadRegisterDefaultMapping` in `gamepad.cc`). The mapping is set via
`SDL_HINT_GAMECONTROLLERCONFIG` before `SDL_InitSubSystem` in `dxinput_init`.
- Calling `SDL_AddGamepadMapping` after init doesn't work. In SDL 3.4, the re-check of already
  attached joysticks runs before `s_pDefaultMapping` is assigned. The pad stays cached as "not a
  gamepad" and only works after a re-plug.
- Every attached pad is opened, and input is handled per event. A pad present at boot gets
  `SDL_EVENT_GAMEPAD_ADDED` twice, so the handler skips pads that `SDL_GetGamepadFromID` reports as
  already open.

## Audio pipeline (fixed 2026-10-18)

The pipeline is game mixer `src/audio_engine.cc` → SDL3 → `nxdk_glue/audio/SDL_xboxaudio.c` (in
nxdk-sdl3) → nxdk-audio APU streaming voice. The device runs at 44100 Hz stereo. Because
`audio_engine.cc` sets `SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES=4096`, each buffer is about 93 ms, and
nxdk-audio's streaming queue holds 2 of them.

History, so nobody repeats it:
- The old driver (AC97 `hal/audio`) produced crackle in XEMU, a fast or looping-sounding menu
  track, and `XBOXAUDIO_WaitDevice: Timeout` log spam.
  - That nxdk HAL only fires its callback once the analog **and** S/PDIF DMA have both finished a
    buffer. xemu never runs S/PDIF DMA (`SO_INDEX` in `hw/audio/ac97.c`), so the callback never
    fired in XEMU.
  - The driver also had a hardcoded 50 ms wait timeout, shorter than one buffer.
- Upstream's `nxaudio` driver fixes the callback, because APU callbacks work in XEMU. But it kept
  the 50 ms timeout, so half of all mixed buffers were rejected by the full 2-deep queue and the
  music ran at 2× and skipped.
- The local fix in `master-nxaudio-fallout`:
  - The wait timeout is 4× the buffer duration, a safety net only.
  - Nothing is zeroed on timeout.
  - The semaphore starts at 0, because SDL fills and plays a buffer before its first wait.
  - Verified: 11 callbacks/s, 11 buffers/s, 0 timeouts, 0 rejected buffers. Music is consumed at
    88.26 KB/s against 88.20 expected. The user confirmed it sounds right in XEMU **and on real
    Xbox hardware** (2026-10-18).
- The comment in `audio_engine.cc` claiming 1024 frames "starves" was based on the broken old
  driver. A smaller buffer could now be tried for lower latency, but it hasn't been tested yet.

How to measure audio health without hearing it:
- Add temporary counters in the driver (callbacks, timeouts, failed `nxAudioBufferQueue` calls,
  buffers played per second) and print them with `DbgPrint`.
- Log `numBytesRead` over time in `refreshSoundBuffers` (`src/int/sound.cc`). Menu music is 22050 Hz
  16-bit stereo, so it should advance about 88.2 KB/s.
- XEMU can't record its audio: `-audiodev wav` disables xemu's default audio, and the machine has
  no `audiodev` property.
