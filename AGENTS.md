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
  - nxdk libraries are prebuilt. **nxdk's Makefile sets no optimization level, so its libraries
    build at `-O0` unless you pass `CFLAGS=-O2 CXXFLAGS=-O2`.** At `-O0`, pdclib's `memcpy`/`memmove`/
    `memset` ran ~15 instructions per byte and made every blit and scroll ~3x slower. The libraries
    the game links are built at `-O2` (2026-11-16); keep it that way when rebuilding any of them.
    From `~/xbox/nxdk` (with clang 21 on PATH and `eval "$(~/xbox/nxdk/bin/activate -s)"`):
    `N=$HOME/xbox/nxdk; make NXDK_ONLY=y NXDK_DIR=$N CFLAGS=-O2 CXXFLAGS=-O2 $N/lib/libpdclib.lib
    $N/lib/libwinapi.lib $N/lib/libxboxrt.lib $N/lib/libnxdk_hal.lib $N/lib/libnxdk.lib
    $N/lib/nxdk_usb.lib $N/lib/libpbkit.lib $N/lib/libnxdk_automount_d.lib`. Make doesn't track
    flags, so delete the libraries' `.obj` files first when changing them. Then delete
    `build/fallout-ce.exe` so the game relinks.
  - The pdclib submodule (`lib/pdclib`) is on local branch **`fast-string-copies`**: `memcpy`,
    `memmove` and `memset` copy 4 bytes at a time (`rep movsl`/`stosl`). With `-O2` that cut a
    0.8 MB `memmove` from 3.7 ms to 0.3 ms in XEMU.
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
- **Virtual controller (analog input).** ViGEmBus is installed on the Windows host. 
  `E:\Xbox\Development\XEMU\vigem\pad.ps1` creates a virtual Xbox 360 pad (`ViGEmClient.dll` next to
  it, x64, taken from the `vgamepad` 0.2.0 PyPI sdist) and reads commands from stdin:
  `lx|ly|rx|ry <-1..1>`, `lt|rt <0..1>`, `btn <a|b|x|y|lb|rb|back|start|ls|rs|up|down|left|right> <0|1>`,
  `wait <ms>`, `ramp <axis> <from> <to> <ms>`, `reset`, `quit`. It prints `ready`, then `ok <cmd>`
  per command, and `rumble <large 0-255> <small 0-255> <ms>` whenever rumble reaches the pad
  (XEMU forwards the game's rumble to it), so rumble can be checked without feeling it. Run it as a background task fed from a file, then append commands to the file:
  `tail -n +1 -f cmds.txt | powershell.exe -NoProfile -ExecutionPolicy Bypass -File 'E:\Xbox\Development\XEMU\vigem\pad.ps1' > pad.log`.
  XEMU binds it to port 3 (`xemu.toml` `port3`, an Xbox 360 GUID), and the game sees it as
  `Original Xbox Controller #2`; it keeps working across XEMU restarts while the server runs.
  If the game logs more pads than expected (`Opening game controller` x4), every press may arrive
  twice from two pad IDs (confirmed 2026-11-17: one A press came from pads 642 and 609), so menus
  move two rows and toggles cancel out. The cause was several `pad.ps1` servers running at once
  (each makes its own ViGEm pad, mirroring the same commands): run exactly one
  (`pgrep -af pad.ps1`). With one server the game sees 2-3 pads and each press arrives once.
  **XEMU passes triggers as digital**: the game only ever receives 0 or 32767 (threshold between
  0.5 and 0.75), so analog trigger behaviour can't be tested in XEMU. Sticks are fully analog.
  To measure pointer speed exactly, log the summed `gLeftStickDeltaX/Y` per stick deflection from
  `ProcessLeftStick` (temporary debug); the 2026-11-16 sweep matched the response curve exactly.
- XEMU keyboard presses sometimes get lost (often the first A after a screen opens). For
  multi-step menu navigation the virtual pad (below) is more reliable: send `btn <name> 1`,
  `wait 120`, `btn <name> 0`. `E:\Xbox\Development\XEMU\opts.sh <log>` boots and opens XBOX OPTS.
- `keys.ps1 -Keys 'up,wait500,a,holde:300'` focuses XEMU and sends real key events (the
  `keybd_event` method above). Names: arrows, `ret`, `a`, `b`, E/S/D/F (left stick), I/J/K/L
  (right stick); `waitN` sleeps, `holdK:N` holds a key.
- `qmp_shot.ps1` does **not** work: xemu's monitor has no `screendump` command. Use `grab.ps1`.

## Code map (Xbox-specific bits)

All Xbox code is behind `#ifdef NXDK` / `XBOX`. The files with the most Xbox-specific code:
`src/plib/gnw/svga.cc` (renderer), `src/game/loadsave.cc` (UDATA save slots),
`src/platform_compat.cc` (paths/FS), `src/game/mainmenu.cc` (XBOX OPTS button),
`src/game/xboxopts.cc` (XBOX OPTS screen),
`src/plib/gnw/gamepad.cc`, `src/plib/gnw/winmain.cc` (startup/mounts), `src/audio_engine.cc`.

SDL-created threads on nxdk-sdl3 get very small stacks (about 4 KB), so keep large buffers off them.

**XBOX OPTS screen** (`xboxopts.cc`, 470x480). It is pieced together from the Preferences screen
art (`PREFSCRN.FRM`, interface fid 240) so it matches the game: the left column with all five
rotary-knob panels in place (`PRFBKNBS.FRM`, four 46x47 positions stacked; DISPLAY uses all four),
three copies of the right column's two-slider panel, the third with 12 rows of empty texture cut
out so it fits (`PRFSLDOF/ON.FRM` knobs), and the bottom button strip in place with
`LILREDUP/DN.FRM`. There is no spare room left. Knobs:
DISPLAY, PAN CENTERS CURSOR, POINTER ASSIST (OFF/ON/+RUMBLE), EDGE SCROLL, SWAP STICKS. Sliders:
POINTER/CAMERA deadzone and sensitivity (the `left_stick_*`/`right_stick_*` keys: "left" is the
pointer stick, whichever physical stick that is), SAFE AREA, RUMBLE. The hint corner says
"RESTARTS GAME" while DISPLAY or SAFE AREA is selected, plus "Y: DEV MENU" always. Coordinates are the Preferences screen's (`options.cc` `btndat`, `UpdateThing`),
shifted as noted in the file. D-pad up/down selects, left/right changes, START (sent as Escape)
applies, BACK (F11) or B (padnav back key F11) cancels; the mouse works on the knobs, sliders and buttons too.
- It edits `f1_res.ini` (`[MAIN] SCR_WIDTH/SCR_HEIGHT` = video mode, `[XBOX] UI_SCALE` 0-2 and
  `SAFE_AREA` 0-8) and `fallout.cfg` `[control]` keys. Changing DISPLAY or SAFE AREA relaunches
  `default.xbe`.
- `[control] edge_scroll` (default 1) gates `gmouse_check_scrolling` via `gmouse_set_edge_scroll`.
  `[control] swap_sticks` (default 0) remaps the stick axes at the top of the gamepad axis
  handler (`SticksSwapped()`: also always on in walk mode), so everything downstream (deadzones,
  pointer, panning) just sees "pointer stick" and "camera stick".
- **When you change the controls, update everything that shows them.** Any change to the
  mapping below (a button moved, added or removed, a hold/tap changed, a wheel entry moved)
  must also update, where affected:
  - `CONTROLS.md`, the player-facing control reference (linked from the README);
  - the LT glyph overlay on the HUD (`intface_glyph_spots` in intface.cc: which glyph sits on
    which bar button, and whether a button gets one at all);
  - the glyphs in our own UI: the walk-mode label (`gmouse_near_label` passes `PAD_GLYPH_A`),
    the wheels' "A ..." / "B Close" hints (skillwheel.cc `wheel_draw`), the inventory and
    loot screens' hint strip (inventry.cc `ipad_hint_show`), and what those screens do with
    each button (`ipad_frame`);
  - `padglyph.cc` if a button needs a glyph that doesn't exist yet (add it to `PadGlyph` and its
    style table, plus any letters the 5x7 font lacks).
- **Button map** (gamepad.cc `HandleControllerButtonDown/Up`, 2026-11-17 redesign: triggers and
  stick clicks are what can be pressed with both thumbs on the sticks, so they carry the actions
  done while moving; face buttons the ones you stop for):
  - **A / RT**: left click (in walk mode: use the highlighted object; hold = action menu).
    **B**: right click; in walk mode with the pointer hidden, Look at the highlighted object
    (`stick_walk_request_look` -> `gmouse_near_target_look`). **LT** held: precision pointer
    (the existing brake) and every usable object on screen outlined (`gmouse_show_all_update`,
    from `ProcessTriggers`; hands the outline over to the walk highlight / d-pad selection if
    they're on the same object). LT + d-pad still sends 1-8 (skill hotkeys).
  - **X**: inventory (I); in combat (with the interface enabled) a tap opens the inventory
    too (I on release, user request: consistent with the INV glyph) and a hold (400 ms,
    `ProcessHeldButtons`) reloads (key R -> `intface_quick_reload`, the active weapon whatever
    its mode, reload AP charged). **Y**: swap hands (B key).
    **Black**: weapon mode (N). **White**: tap = end turn (Space, on release), hold = end combat
    (Enter, combat only); outside combat a tap starts combat (`stick_walk_request_combat` ->
    `stick_walk_run_pending`, as the keyboard's A; ignored if the map loop doesn't take it
    within 500 ms). With a walk-mode highlight on a living critter, `gmouse_combat_aim_prepare`
    remembers it and `combat_input` calls `gmouse_combat_aim_apply` at the player's turn: pointer
    shown, crosshair, target selected (verified on Slummer). **Start**: Esc. **Back**: tap = menus
    wheel (F11, sent on release, so other screens reading F11 still get it), hold (400 ms,
    `ProcessHeldButtons`) = quick wheel (F8; F12 is the screenshot key).
  - The **quick wheel** (`quick_wheel_select`, skillwheel.cc; game.cc KEY_F8): quick save (F6)
    and quick load (F7, after a YES/NO), heal (`quick_heal`: stimpak, healing powder PID 273, or
    a super stimpak at 50+ HP missing; used as the inventory's Use does, for the inventory's
    4 - Quick Pockets AP in combat; "x N" carried under the button), reload (R), sneak
    (`action_skill_use(SKILL_SNEAK)`), walk mode (`GamepadToggleStickWalk`), rest
    (`pipboy(PIPBOY_OPEN_INTENT_REST)`, not in combat), center view (Home). Opens on the last
    used entry (`quick_last`). **`display_print` edits its text in place**: passing it a
    string literal wrote into .rdata and crashed (fatal 0xC0000005); `quick_print` copies first.
    Verified in XEMU: tap/hold split, heal 16 -> 26 HP (x4 -> x3), "already at full health",
    sneak on, quick save's slot picker, rest -> alarm clock, walk mode toggle. Not exercised:
    quick load's dialog, reload, center view, heal in combat.
  - Stick clicks go by the physical stick, whatever the swap: **left** toggles walk mode
    (`GamepadToggleStickWalk`: saves `[control] stick_walk` with `gconfig_save`, prints
    "Controls: walk with the left stick." / "Controls: pointer.", plays the haptic tick); in
    combat it centers the camera (Home). **Right** is S, which on NXDK always opens the skill
    wheel (below); its release sends the same key it pressed.
  - The **menus wheel** (`menus_wheel_select` in skillwheel.cc, titled MENUS, same radial code
    as the skill wheel): Inventory, Pip-Boy, Character, Map, Skilldex, Options, Load, Save;
    picking one feeds its hotkey back into `game_handle_input`. Names that don't fit the button
    in font 103 use font 101. Both wheels are steered with the **physical left stick**
    (`GamepadGetPhysicalLeftStick`, whatever the swap; user request) or the d-pad, and while one
    is open `GamepadSetUiCapture` stops both sticks from moving the pointer, panning or walking.
  - Other screens that read the old keys: XBOX OPTS cancels on F11 (Back) now (it was `b`, which
    Y sends now); in dialogue `b` (Y) is Barter; character selection still backs out on Esc
    (Start).
  - **Button glyphs** (`game/padglyph.cc`, `pad_glyph_draw`): Duke-style discs drawn from
    scratch into 8-bit buffers (no art needed): green A, red B, blue X, yellow Y, black/white
    discs for Black/White, grey "RS"/"LS" and dark "START"/"BACK" pills. Colours go through
    `colorTable` (never index 0, which is transparent in sprites); letters use a built-in 5x7
    bitmap font because font 101's R reads as an A at that size. Used where the UI is ours: the
    walk label (`text_object_create_label`'s `glyph` widens the text object and puts it in
    front) and the wheels' "A ..." / "B Close" hints.
  - **The HUD is not drawn over permanently.** Glyphs stuck on Fallout's frame looked janky, and
    tinting the buttons the pad colours (yellow swap-hands dome, blue INV, white TURN/CMBT)
    looked bad too; both were tried and removed (user feedback). Instead, **hold LT: glyph
    overlay** (`intface_show_pad_glyphs`, called every frame from `ProcessTriggers`): one small
    `WINDOW_MOVE_ON_TOP` window per spot holding a copy of the bar under it plus the glyph (GNW
    has no transparent windows here), so button redraws can't erase it and deleting the windows
    restores the bar. Each glyph is centred on its button (`intface_glyph_spots`, centres in bar
    coordinates): Y swap hands, X INV, START options, Black item box, RS Skilldex, BACK on the
    MAP/CHA/PIP column (they're in the menus wheel), White on END TURN / END CMBT (only while the
    combat panel is open) or on the closed panel (start combat), X on the item box in combat
    (hold reloads; spots have `when`: always / combat / no combat, and `held`).
  - **HOLD tags** (user request): a glyph whose action needs the button held gets a dark
    "HOLD" pill under it (`PAD_GLYPH_HOLD`, `pad_glyph_draw_held`; font has H/O/D now). Used on
    X (reload) and White on END CMBT. Any new tap/hold split must mark the hold spot `held`.
  - **Other screens' LT glyphs** (`pad_glyph_spots_set(win, spots, n)` / `_clear` /
    `_update`, padglyph.cc; same fade, same copy-under windows, the window's buttons' images
    included): dialogue Y on BARTER, barter Y on OFFER and B on TALK (`padModeOnly`: B is a right
    click with the pointer). Registered in gdialog.cc's create/destroy of the dialogue and
    barter windows. Shown while the first spot is on the top window. Verified in XEMU. Only shown while the bar is the top window (checked at the AP lights,
    which no glyph covers). Verified in XEMU in and out of combat, bar restored on release.
    The glyphs **fade with the trigger**: strength = (LT - 0.1) / 0.75, eased toward that at
    most a full fade per 150 ms (so XEMU's on/off triggers still fade), drawn in 8 steps by
    blending each glyph pixel with the saved bar pixel in RGB (`cmap`) and mapping back through
    `colorTable`. Verified in XEMU (frame capture: red -> orange -> yellow in and back out); the
    analog part needs real hardware.
  - XEMU note: the ViGEm pad's LB/RB arrive as Black/White (SDL buttons 9/10).
  - Verified in XEMU: menus wheel -> Character, and left-stick steering with walk mode on (no
    walking behind the wheel, the right stick ignored); LT outlines (doors, NPCs) and clears; B Look on
    Zark ("You see a thug"); in combat X reloaded an empty pistol, Black cycled SINGLE/RELOAD, Y
    swapped hands, White ended the turn (and, with no hostiles, combat). Not exercised: White
    hold, LS centering in combat, X hold.

**On-screen keyboard** (`game/padkeys.cc`, NXDK). The text prompts call
`padkeys_begin(fieldX, fieldY, firstCapital)` (or `padkeys_begin_at(top, ...)`), wrap
`get_input()` in `padkeys_frame()`, and `padkeys_end()` when done: `get_input_str` (editor.cc,
the name), `get_input_str2` (loadsave.cc, save description), `about_loop` (gdialog.cc, Tell Me
About: placed over the talking head so the reply box stays visible), `save_file_dialog`
(bmpdlog.cc, the character editor's Save / Print file name; its list then needs the pointer).
`padkeys_frame` passes real
key codes through and otherwise turns pad presses into characters / KEY_BACKSPACE /
KEY_RETURN / KEY_ESCAPE, so the prompts' own code does the editing. Layout: 4 rows of 10
characters (digits, qwerty, `-.'?`) and SHIFT / SPACE / DEL / DONE; hint strip with glyphs.
- Input goes through `GamepadSetTextCapture` (gamepad.cc): every button but the stick clicks is
  captured (only counted for `GamepadButtonPresses`), and it sets `gUiCapture` so A never clicks
  and the sticks do nothing. The A press that opened the prompt (save screen DONE) used to type
  a letter: A only counts after it has been seen up (`pk_a_released`). A captured A-up also
  clears `gAButtonHeld`.
- Verified in XEMU: save description "Test 5" (shift, space, digit) saved and shown in the
  load list; Tell Me About "M" asked (answer visible); character creator name "Max" (B held
  deleted the old default name with repeat). BACK cancel verified in Tell Me About.
- XEMU testing note: the virtual pad's command file is a queue: a screenshot taken a fixed time
  after queuing a long sequence can show the middle of it.
- Dialogue BARTER / TELL ME ABOUT have plate highlights now (the dot alone was invisible).

**Character editor without the pointer** (editor.cc, the `epad_*` block before
`editor_design`, NXDK). The editor window is `WINDOW_PAD_SELF_NAV` with the gamepad's screen
capture on (`epad_start` / `epad_end` from `CharEditStart` / `CharEditEnd`); `epad_frame(keyCode)`
runs after `get_input` and returns the key code for the loop. Everything in the editor
revolves around `info_line` (the selected item: stats 0-6, level rows 7-9, folder rows 10-29,
tabs 40-42, conditions 43-50, derived stats 51-60, skills 61-78, 79/80/81 headers, traits
82-97), which the screen already draws in its highlight colour and describes on the card.
- `epad_build` lists the items each frame (window coordinates, from the draw code's layout;
  the folder rows depend on its contents). Moving onto one sets `info_line` (and `skill_cursor`,
  or `folder` for a tab) and redraws as `InfoButton` does, without reading the mouse. Items
  with no look of their own (Name/Age/Sex, headers, character points, tabs, the bottom plates)
  get `padnav_show_area`. The d-pad picks the nearest item that way (45 degree cone, then
  anything past the item's edge).
- A (on release) returns the item's event code: buttons (517/519/520, 500/501/502), tag
  skill (536+), trait (555+). Left/right adjust instead of moving on a SPECIAL stat in
  creation (`epad_stat_step`, one step of `StatButton`) and on a skill when levelling up if
  there are points to spend or take back (the arrow goes to `SliderBtn`'s keyboard path);
  otherwise they move. B returns Escape (cancel). Button baselines are re-synced after any
  pad action (`epad_acted`), so presses inside the windows it opens don't leak back.
- Pointer mode: the mouse works as before; a d-pad press goes back to pad mode on the item
  under the pointer.
- Age / Sex windows: DONE has the focus and a plate highlight, with
  `padnav_set_adjust(doneBtn, proc, pressable = true)` (new parameter: A still presses it), so
  left/right change the age or the gender (radio buttons updated) and A closes.
- Perk window (`perks_dialog`, shared by the Tag!/Mutate lists): `padnav_set_filter` leaves only
  DONE/CANCEL (plate rects) for the d-pad, so up/down reach `InputPDLoop`'s list.
- Creating a character, only changeable items are on the cursor's path (name/age/sex, stats,
  skills, traits, bottom buttons; user request: character points, derived stats and headers
  looked selectable but did nothing). The character screen keeps every line (all reading).
- The name window starts from the current name. The old NXDK default name "Vault Dweller"
  (critter.cc `critter_pc_reset_name`, proto.cc) is gone: a new character is "None" as on PC.
- Verified in XEMU: creation (stats +/-, char points, trait pick/unpick, tag skill via
  DONE -> up, Age 25 -> 26, Sex -> Female, name "zx" typed, Options -> Save with the keyboard,
  BACK / B cancels, DONE's "use all points" message dismissed with B without cancelling the
  editor); level-up (temporary XP hook: perk window list + pick, Big Guns +3/-1 with the
  points counter, tabs and karma / perk-folder trait rows, B cancel). Name field empty on a
  new character. Not exercised: Kills
  rows, Print, Load, starting the game from DONE.

**World map / town maps** (worldmap.cc): plain padnav. The town list's red dots and the
TOWN/WORLD switch get plate highlights; `padnav_set_filter(world_win, wm_pad_filter)` skips town
slots not visited yet (`first_visit_flag`). Town map entrances (`TMSelBttns`, with hover art)
are padnav buttons already. Verified: Cathedral town map -> Vault 15 town map -> Desert
Entrance -> travel -> random encounter.
- Free travel: the world map loop sets `GamepadSetPanHandler(wm_pad_pan)` (new, gamepad.cc: while
  set, `ProcessRightStick` hands the camera stick's deflection and dt to the screen instead of
  walking / panning the town map, which it used to scroll behind the world map). `wm_pad_pan`
  accumulates pixels (0.6 px/ms at full deflection) that the loop adds to `viewport_x/y`
  (clamped, autofollow off). When the pointer stick ends pad mode, the loop moves the pointer
  onto the player's marker (padnav had parked it on a town button); A then clicks a spot as on
  PC. Y (key `b`) while stopped (`dropbtn`) enters the area, as clicking your marker.
  Verified: pan, pointer on the marker, travel into unexplored desert (random encounter), Y
  entered the desert next to the Cathedral. No glyph for Y (no button to put it on).

**Pip-Boy** (pipboy.cc). Plain padnav plus fixes:
- Plate highlights for STATUS / AUTOMAPS / ARCHIVES / CLOSE (`47, y - 4, 122x23`).
- Rows that hold two things, where the click's x decided (padnav presses the centre, which
  matched neither): the status page (location left of x 429, holodisk right;
  `AddStatusHotLines`) and the holodisk reader's bottom line (BACK left of 395, MORE/DONE right
  of 459; `SplitHoloBackLine`) are two buttons each, same event code (`HotLinesRight`,
  `holo_more_btn`, deleted in `NixHotLines`).
- `hot_back_line` was never set (decompilation slip), so back lines were never deleted and
  piled up; `AddHotLines` sets it now. B (Escape in pad mode) with a back line calls the page's
  back (1025; 1027 = page back in a holodisk) instead of closing.
- The alarm clock is off the d-pad path (`padnav_set_filter`: from the lists the d-pad landed
  on it and couldn't get down to the menu) and on Y (key `b` -> 504) with an LT glyph spot.
- Verified: menu highlights, status -> Vault 13 quests -> B back -> B close, automaps -> Vault
  13, alarm via Y and rest via A, LT glyph. Not exercised: holodisks and archives (none in the
  test save).

**D-pad menu navigation without the pointer** (`plib/gnw/padnav.cc`). Generic, so every GNW
screen gets it without per-screen code:
- Each frame (`padnav_update`, from `process_bk`) it collects the eligible buttons of the
  windows a click could reach: top down, skipping hidden windows and its own overlays, stopping
  at a modal window or one flagged `WINDOW_NO_PAD_NAV` (map view, HUD, inventory/loot/barter
  `i_wid`: they have their own pad code, below) or `WINDOW_PAD_SELF_NAV` (XBOX OPTS, which
  navigates itself; padnav only hides the pointer). Eligible: enabled, does something on a
  left click, not tiny, not a hotspot covering most of a big window, not a position-reading
  area taller than 150 px (the save slot list: the screen's own arrow keys handle it), and not
  a button that only sends an arrow / page key (dialogue reply halves, list arrows).
- **Pad mode** (pointer hidden, focused button highlighted) vs **pointer mode**. The highlight
  (a gold rectangle at first, replaced as too ugly; user feedback): GNW hover works off the
  pointer position whether it's visible or not, so in pad mode the hidden pointer is parked on
  the focused button and the game draws its own hover look. Buttons count as having one if they
  have a hover image, a mouse-enter proc, or distinct enter/exit event codes (dialogue options:
  1200+n / 1300+n, recoloured yellow by the dialogue loop; Preferences' sliders share 526 for
  both, no visual). Others (main menu / DONE red buttons, Preferences knobs and sliders) get a
  brightened copy of their window pixels (`cmap` RGB x1.4 + 1, via `colorTable`) in one
  `WINDOW_PAD_OVERLAY` window, refreshed every frame, table rebuilt per new highlight in case the
  palette changed.
  - `padnav_set_highlight_rect(btn, x, y, w, h)`: a screen can make the highlight cover a bigger
    area than the button (window coordinates; dropped when the window is deleted). Used where the
    button is just a red dot on a plate with its label in the background art: main menu (plate
    x-12..x+179, y-3..y+29 from the button), Load/Save DONE/CANCEL (x-11, y-5, 98x24),
    Preferences DEFAULT/DONE/CANCEL (x-8, y-5, 101/98 x 23). Measured from screenshots.
  - The overlay only refreshes when `process_bk` runs, so it outlived its menu during transitions
    that don't (Exit Game's fade, the barter panel sliding in; user report). It is now created
    with its source window's `WINDOW_MOVE_ON_TOP`, so any window opened later stacks above it
    (no need to delete it on `win_add`: doing that repainted the spot from the next screen's
    not-yet-drawn window, a black rectangle while the main menu faded; user report), and
    `win_delete` of the source window calls `padnav_window_changed`, which drops it. Verified:
    Exit Game -> YES, Barter, and main menu -> Load Game transitions.
  - The copy layers the source window's buttons' `currentImage` over its buffer, as GNW does
    when drawing: a button state change (`button_draw(..., draw=true)`) never reaches the window
    buffer, so the pressed image only showed this way.
  - A/RT press the button on A down (`win_button_press`: pressed image + left-down event) and
    release it on A up (`padnav_release` -> `win_button_release`: normal image + left-up event,
    the click), so the press shows as with a mouse (user request). Verified on the main menu.
- `ProcessLeftStick` calls `padnav_pointer_moved` when a
  pointer stick moves: pointer back on the focused button, frame gone. A d-pad press with a nav
  window returns to pad mode. Default follows walk mode (`padnav_set_pad_by_default`): with walk
  mode on a newly opened menu starts in pad mode, with it off (PC-like) in pointer mode.
- The gamepad's d-pad handlers call `padnav_dpad`: nearest candidate within 45 degrees of the
  direction (score along + 3 * across, edge overlap counts as 0 across). Nothing that way ->
  the arrow key is sent as before, so screens' own wrap-around/list handling still works.
  A/RT (`ProcessTriggers`) call `padnav_activate`: the hidden pointer is put on the button's
  centre (some screens read the click position, e.g. the Preferences knobs), then the button is
  pressed until A is released (above); the press never becomes a mouse click.
- **B is back** while padnav is active (pad mode with a nav window, or a `WINDOW_PAD_SELF_NAV`
  window in pad mode): `padnav_back_key()` gives the SDL scancode B sends instead of the right
  click, Escape unless the window registered another with `padnav_set_back_key(win, scancode)`
  (dropped on `win_delete`): main menu 0 = nothing (Escape there is Exit), XBOX OPTS F11 (Escape
  applies). Escape cancels/closes almost every GNW screen (dialog boxes, Pip-Boy, editor,
  Preferences, options, load/save, Skilldex, automap, elevator, character select); the dialogue
  reply loop ignores it. Its key-up is sent on B up. Pointer mode and the map (NO_PAD_NAV)
  keep B as right click / walk-mode Look. Verified: main menu (nothing), character select,
  in-game options, Preferences -> options, Pip-Boy. XBOX OPTS not exercised.
- `padnav_set_adjust(btn, proc)`: a slider; left/right call `proc(btn, dir)` instead of moving
  (Preferences' range sliders, `PrefAdjustSlider`, a twentieth of the range per press;
  unregistered in `PrefEnd` because button ids are reused).
- Focus: kept on the same button id; else a pending request (`warp_mouse_to_button` /
  `warp_mouse_to_dialogue_option` call `padnav_set_focus`, and in pad mode only move the hidden
  pointer, hiding it first so no cursor image is left behind) taken when that button appears
  (within 1 s: dialogue asks before creating its options); else the button nearest where the
  focus was (same menu); else top left.
- Back on the map with pad mode on in walk mode, `stick_walk_update` re-hides the pointer.
- Verified in XEMU: main menu (highlight, d-pad, right stick shows pointer, d-pad hides it), Load
  Game (DONE/CANCEL, list via arrows, loaded a save), Start menu, Preferences (knobs and toggles
  with A, slider with left/right), dialogue (options, a lone [Done] option, closing), XBOX OPTS.
  Not checked: character creation/editor, Pip-Boy, world/town maps, Skilldex window, combat
  options, quantity/pickers, walk mode off. Highlights verified: main menu and DONE buttons lit,
  dialogue options yellow, Preferences knob panel and slider row lit.

**Inventory, use-item-on, loot and barter without the pointer** (inventry.cc, the `ipad_*`
block before `inven_set_dude`, NXDK only). These screens are all mouse-driven: a slot is an
invisible button whose key code (1000+row your list, 1006/1007/1008 right hand/left hand/armor,
2000+row the other list, 2300+/2400+ the barter tables) reaches the loop with the mouse button
state, and every drag (`inven_pickup`, `move_inventory`, `barter_move_inventory`,
`barter_move_from_table_inventory`) shows the item as the cursor, waits for the button to come
up, then hit-tests the pointer. The pad code drives exactly that:
- `ipad_begin(type)` / `ipad_end()` around each loop; `ipad_frame(keyCode)` right after
  `get_input`. In pad mode (padnav's, `padnav_menu_opened` at open) it hides the pointer, parks
  it on the focused slot, shows the highlight with `padnav_show_area` (padnav's brightened
  overlay, drawn while no padnav window is above), and returns the key code to handle: a slot's
  key for a pad "click" (`ipad_dispatch`), Escape/`t`/`m`/`A`/2500 for B/X/Y, or -1. The loops
  read `inven_left_down()` / `inven_right_down()` / `inven_menu_mode()` instead of the raw mouse
  state, so a pad click goes down the mouse path.
- A drag the pad started drops where `ipad_drop` says (`ipad_drag`, in place of the wait loop):
  a slot (the hidden pointer is parked on it, so the screen's own hit test decides), or
  `IPAD_DROP_CARRY`: the item is shown as the pointer and moved slot to slot with the d-pad
  until A (drop) or B (back on its own slot). No list scrolling while carrying (the drag code
  keeps the list offset it started with).
- Buttons are read with the gamepad's **screen capture** (`GamepadSetScreenCapture`): while on,
  in pad mode and with no padnav window above (`GamepadScreenCaptureActive`), A/RT don't click
  and B/X/Y/Black/White send no keys; the screen polls `GamepadButtonPresses` (press counters,
  so a tap shorter than a frame isn't lost) and `GamepadClickHeld`. A key-up is only swallowed
  if its key-down was (`gButtonCaptured`), so X's "I" opening the inventory still gets released.
  The d-pad keeps sending arrows (key repeat for free).
- Mapping: inventory A tap carry, A hold (350 ms) the item action menu (`inven_action_cursor`
  with `ipad_menu` 1: no click-or-hold wait, menu at the slot, d-pad selection while A is held,
  B = Cancel), X Use (`ipad_menu` 2 runs `GAME_MOUSE_ACTION_MENU_ITEM_USE` directly if the menu
  would offer it), Y equip (armor slot or the active hand, `intface_is_item_right_hand`) /
  take off (`ipad_unequip_target`: a free row, never onto a container, which would swallow
  it), B close (out of a bag first). Loot: A moves to the other list, X Take All (`A`), B Done.
  Barter: A inventory <-> own offer table, Y Offer (`m`), B Talk (`t`). Black/White page the
  focused list. Up/down past a list's visible rows scrolls it.
- Focus: the d-pad goes along a list, else to the nearest slot within ~50 degrees, else the
  nearest on that side (the hands are far below the list's top rows). Reachable slots: rows
  with items (at least row 0 of each list) plus hands/armor.
- The focused item is described: inventory info panel (`inven_obj_examine_func`, stats on an
  empty slot), barter `obj_look_at_func` into the dialogue box, loot nothing (it would fill the
  message log). `ipad_described_key` is reset after any pad action and on returning to pad
  mode, since those may have redrawn the panel.
- Hint strip (`ipad_hint_show`): its own MOVE_ON_TOP window with the clean background art and
  glyphs: inventory bottom of the info panel (only over a description: over the stats it would
  cover the weight), loot the middle plate. None in barter (not placed yet).
- The pointer stick switches to pointer mode as everywhere (pointer on the focused slot, mouse
  behaviour as before, B right-clicks, X closes the inventory); the d-pad switches back, onto
  the slot under the pointer.
- Verified in XEMU: inventory highlight/description, Y equip, carry knife to ITEM2, X Use
  (stimpak), A hold menu -> Use (lit a flare), B close, pointer round trip, hints; loot (opened
  with a temporary hook on a living guard): A both ways, X Take All, hint, B; the quantity
  window over the inventory (padnav; it opens on DONE, `padnav_set_focus` in
  `setup_move_timer_win`, with plate highlights for DONE/CANCEL; A on ALL gave 004, B and
  CANCEL cancelled without closing the inventory).
- Presses made in a window a pad action opened (quantity window, message box) are that
  window's: `ipad_frame` resets its button baselines after every pad action, or the B that
  cancelled the quantity window also closed the inventory (found in testing). **Barter not
  tested** (no merchant reachable from the saves); use-item-on not tested.

**Stick walking** (prototype; `game/stickwalk.cc`): walk mode, toggled in-game with the left stick
click (`[control] stick_walk`, default 0; it used to be SWAP STICKS = WALK / `swap_sticks=2`,
which `GamepadInit` migrates to `swap_sticks=1 stick_walk=1`). Like the Beamdog console ports'
Drive/Tactics modes. The sticks are swapped (pointer on the right) and, outside combat on the map
view, the camera stick walks the player directly, console-Diablo style. `stick_walk_update` returns what the stick
should do otherwise: in combat on the map view it pans as usual (`STICK_WALK_PAN`); in menus,
dialogue or any window over the map center it moves the pointer too (`STICK_WALK_POINTER`:
`gCameraStickPointer`, and `ProcessLeftStick` uses whichever stick is pushed further), so it
never pans the map behind a menu (user request). Verified: no map movement behind the
inventory while the left stick moved the pointer.
- No path planning. The engine walk (`object_move`, anim.cc) moves a critter to the next hex when
  its pixel offset passes the step, then reads the next direction from `rotations[]`. There
  `anim_set_dude_steer`'s hook asks `sw_steer` for the next hex and rewrites the path to that one
  step, so the walk cycle never restarts. Unextended, a walk ends at the hex being entered, which
  is how releasing the stick stops (and a blocked next hex).
- The next hex: of the free neighbours within ~87 degrees of the stick, the one nearest a point
  64 px ahead on a line through the hex where the stick was last turned (>20 degrees). Hexes have
  no up/down neighbour, so straight up alternates NE/NW and stays on the line. 45 degrees walks
  plain NE (37 degrees); anything steeper than ~55 mixes in NW. Slanted walls are slid along;
  perpendicular ones stop the walk. Closed doors block: the engine never auto-opens doors for
  the dude (`anim_can_use_door`), so they're opened with the pointer as before.
- Deflection >= 0.85 runs, less walks; it switches at hex boundaries (art changed in the hook,
  frame scaled to the same share of the new cycle so the stride carries on). Sneaking or a
  crippled leg always walks.
- **Free movement** (walk mode's default style, `[control] stick_walk_free` 1; the quick wheel's
  WALKING entry cycles OFF -> FREE -> HEX; `sw_free_update`). Outside combat the player moves
  continuously: position = hex + pixel offset (`obj->x/y`, via `obj_offset`, which also moves
  the egg), kept with sub-pixel floats; the hex is always the one whose centre is nearest in
  world space (screen x / 32, y / 13.86: hexes are regular in the world), so the offset stays
  within half a hex. Crossing a hex calls `obj_move_to_tile` (exit grids, roofs, egg, seen) and
  `scr_chk_spatials_in`, as a finished engine step. Pace from the art (`sw_free_measure`,
  `sw_free_play`): the art plays at its fps (times 0.45-1 for a light push, and a 0.12 s ramp
  from a standstill, 30% -> 100%), and during frame k the player covers the art's own step into
  frame k+1 (its per-frame shift, as world hexes), spread smoothly: the player is where the
  engine would put it each time the image changes, so planted feet don't drift as they did with
  a constant speed (user feedback: "sliding"). Same speed every world direction (walk ~2.6, run
  ~7.9 hexes/s for the vault suit; straight up on screen is slow because the view squashes it,
  as slow as the engine's zig-zag). Letting go: it walks on in the same stride to the hex centre
  nearest half a hex ahead (`sw_free_stop_tile`, facing the way it goes) and then stands,
  instead of gliding back with the walk frames playing; stopped against a wall, letting go walks
  the few pixels back onto the hex. Facing: the nearest of the 6 to the stick (angles via atan2), kept until
  another is nearer by 5 degrees (only so straight up, halfway between NE and NW, doesn't
  flicker; a 0.15-cosine margin made right -> down-right keep facing right until ~30 degrees,
  user report). A critter's fid is the same for every facing, so a facing change must be detected by
  comparing `obj->rotation` (it once compared only the fid: the facing never updated after the
  walk started, so a gradual stick turn ran the player backwards; user report). Blocked: the part of the move toward the blocked hex's centre is removed (twice, for
  corners), so it slides along the border; a closed door stops it instead so the push opens it
  (`sw_door_ahead`, after `sw_free_snap`). `sw_free_snap` (offset 0, stand) runs before A use / menu / look / combat, in
  `stick_walk_pause` (wheels) and whenever walking isn't allowed (dialogue, menus, combat). An
  engine animation on the player (clicked walk, door, script) is waited for; anything else
  moving the player is picked up by re-reading tile/offset each frame. Verified in XEMU (logs):
  speeds, straight-up run to the Cathedral door, slide along the wall next to it, door opened,
  ran inside (roof hidden). Not tested: exit grids, spatial scripts, combat start mid-offset,
  saving mid-offset (the offset is saved with the object; harmless).
- **Dev menu** (XBOX OPTS, Y or the hint corner; `xo_dev_menu` in xboxopts.cc): a small window
  over XBOX OPTS built from the same art (title band, three copies of the PAN CENTERS CURSOR
  knob panel, the DONE plate). WALK STYLE HEX/FREE (`stick_walk_free`), FREE WALK PACE
  GLIDE/STRIDE (`[control] free_walk_pace`: 0 = constant speed at the cycle's average step,
  the first free version; 1 = the art's per-frame steps), FREE WALK STOP SLIDE/STEP
  (`free_walk_stop`: 0 = full speed at once and glide back onto the current hex on release;
  1 = the 0.12 s ramp and walking on to a hex ahead). D-pad up/down/left/right, A turns the
  selected knob, START/DONE applies (saved with `gconfig_save`), B/BACK cancels. A and B are
  read with the screen capture: walk mode's hidden-pointer A handling otherwise ate A here.
  Verified in XEMU: open, change, apply, reopen shows it, B cancels.
- Pace (hex style): `anim_set_dude_step_speed(sw_step_speed)` (anim.cc, applied as each player step starts)
  scales a step's frame time by the cosine between step and stick in world space (screen y /
  13.86, x / 32: every hex step is the same world length), floor 0.8, so zig-zagging straight up
  (30 degrees off each step) isn't 13% slower. A drawn-pose layer (steady facing + drawing the
  sprite on a straight line over the zig-zag) was tried and removed: correcting the drawn
  position every frame against a position that moves in per-anim-frame jumps made the sprite
  visibly vibrate (user feedback).
- Turning back mid-step: `anim_dude_turn` (`sw_turn`) is asked every frame of the step. If the
  stick points within ~60 degrees of straight back, the step is less than half done and the hex
  behind is free, the step is turned around in place (rotation flipped, path set to the reverse).
  The player is still on the step's start hex, so it is a normal step to the hex behind and no
  offsets need fixing. Only exact reversals: other angles would leave a sideways pixel offset.
  Past halfway it waits for the boundary, so a new direction takes at most half a step.
- Walking into a closed door (within ~55 degrees of the stick, blocking the next hex) runs
  `action_use_an_object` on it, once per stick push (a locked door's message doesn't repeat).
  Verified: walking into the Cathedral door opened it and walked through to the interior.
- Pushing the stick during a clicked walk takes it over at the next hex; during another
  animation it waits for it to end. A walk with no stick update for 200 ms stops.
- The camera follows with `map_scroll_pixels`, closing 8x the distance per second, until the
  player's feet are within 2 px of a point one hex (24 px, `kFollowDropY`) below the map view
  center (user request; or the map edge stops it). A look-ahead
  in the walking direction was tried and removed: the user found it distracting.
- Verified in XEMU (2026-11-17, TMPWALK logs): one register per push, steady steering; down and up
  zig-zag SE/SW and NE/NW; walk -> run -> walk without a stop; stops at the building wall; no
  walking with the inventory open. Not done: combat movement, feel on real hardware.
- **Hidden pointer and A to use** (gmouse.cc `gmouse_walk_*`, `gmouse_near_target_*`). Walking
  with the stick hides the pointer (`gmouse_walk_hide_pointer`): `gmouse_bk_process` returns early
  (no cursor, hex cursor, hover outline, action popup or edge scroll) and `gmouse_handle_event`
  ignores clicks. Any pointer-stick movement brings it back on the hex in front of the player; so
  do the d-pad target jump, a disallowed state (dialog, combat, window over the map, wait cursor;
  `stick_walk_update` shows it) and turning walk mode off.
- While hidden, the nearest pickable object within 2 hexes (`gmouse_pointer_assist_wants` in
  ARROW mode, on screen, not under a roof; score distance*4 + turns away from the player's facing
  *3) gets the gold item outline and a floating "<verb>: <name>" label (Take / Talk / Search /
  Open / Close / Use / Look, English only; `text_object_create_label` in textobj.cc: placed just
  above the sprite instead of 60 px up, fades on the usual float timer). An object has only one
  floating text and creating one replaces it, so labels are flagged (`TEXT_OBJECT_LABEL`), only
  `text_object_remove_label` removes them, and no label is made while the object shows other
  text: otherwise the label came back on an NPC right after you talked to it and wiped its reply
  after a split second (user report; verified fixed with a test float).
  Reachability: at distance 2 a free hex must be next to both the player and the object, so
  nothing is picked through a wall.
- The d-pad steps the highlight to the nearest candidate in that direction (`gmouse_jump_to_target`
  routes to `gmouse_near_target_step` while hidden; the pointer jump is unchanged when visible).
  A pick stays until the player moves to another hex or it leaves reach.
- A or RT (`ProcessTriggers`, which then sends no click): a tap (released before 350 ms) uses the
  target with `gmouse_default_action`, the same switch as an arrow click (pick up, talk/loot,
  use, look). Holding opens the action menu on it (`gmouse_action_menu`, the hold-click menu
  factored out of `gmouse_handle_event`; it takes a `held` callback, here `GamepadClickHeld`, and
  the d-pad now moves its selection too; it also clamps the index, which could run off the list
  before). The menu is drawn by the flat hex cursor object, which renders with the floor, so
  walls, roofs and sprites lower on screen covered it (an NPC indoors); while it's open
  `obj_set_mouse_flat_on_top` makes `obj_render_post_roof` draw it again last, fully lit (mouse
  and pad alike). The target's label stays up during the menu (text removal is a bk process,
  which the menu pauses) and goes when it closes. Both run from `game_handle_input` (`stick_walk_run_pending`), where clicks are handled;
  `sw_in_menu` keeps the menu's nested input loop from re-entering stick walking. The outline is
  removed before acting (dialogue snapshots the map view when it opens, outline included) and
  isn't shown again until the player is idle or the stick moves. Objects flagged
  `OBJECT_NO_HIGHLIGHT` can be targeted but not outlined. Decorative scenery (e.g. the trash
  bins outside the Cathedral) has no use action and is skipped. The previous target is only
  touched if it's still in the 8-hex object list; `gmouse_forget_target` and `gmouse_reset` clear
  it too.
- **Skill wheel** (`game/skillwheel.cc`; on NXDK `game.cc` KEY_S always calls
  `skill_wheel_select` / `skill_wheel_use`, in any mode and in combat; with the pointer shown the
  target is the object under it). The eight Skilldex
  skills on an ellipse around the player, clockwise from the top in Skilldex order, drawn with
  the Skilldex button art (interface fids 120/119) and names from `skilldex.msg`, each with its
  level; the selected one gets a gold frame. The center shows what A will do ("A: on Slummer",
  "on yourself", "pick a target", "toggle"). It runs its own modal loop: the pointer stick
  (the physical left stick past 0.5) or the d-pad picks; A uses;
  B (`GamepadBackHeld`, read from the pad because the mouse layer ignores buttons while the
  pointer is hidden), START or another right click closes. `GamepadSetUiCapture` keeps A from
  clicking and the stick from moving the pointer meanwhile, and `stick_walk_pause` stops
  walking. Using a skill: on the highlighted object if that skill's cursor mode accepts it
  (`gmouse_near_target_for_mode`), First Aid / Doctor on the player otherwise, Sneak toggles,
  and with no target it falls back to the normal skill cursor (pointer shown in front of the
  player). GNW draws transparent windows only when back-buffering, which isn't on, so the
  window is filled with a dimmed copy of the map under it (`intensityColorTable[..][80]`).
  Verified in XEMU: First Aid from the wheel on Slummer ("looks healthy already"), d-pad and
  stick selection, B and right click close, Lockpick with nothing near gave the skill cursor.
  Sneak wasn't exercised.
- Verified in XEMU: guard outlined, A opened dialogue without the outline in its view, outline
  gone after; Cathedral door outlined, A went through it; pointer hidden while walking and back
  in front of the player on a right-stick nudge; A with the pointer visible is a normal click.
  Later (with a temporary key dropping a pistol and ammo next to the player): labels "Take: 10mm
  JHP" / "Take: 10mm Pistol" just above the items, d-pad right/left switched between them, taps
  of A picked both up; holding A on a guard opened the menu, d-pad down + release did Look ("You
  see a zombie"). Reversal fired at offset 14/32 and was skipped past halfway. Not tested: the
  reachability check against a real wall.

**Display scaling and safe area** (`svgaComputeLayout`, svga.cc). The window is the video mode
(480p/720p); the game draws at a smaller "game size" that every window, the mouse and `scr_size`
use, and `renderPresent` draws the texture into `gPresentRect` (centered) instead of full screen
(SDL logical presentation is off on NXDK). Area = mode minus SAFE_AREA% per edge; game size =
area / UI scale (1, 1.33, 1.5), at least 640x480, width a multiple of 8. At 480p the game can't go
below 640x480, so a safe area scales the image down. Texture filtering is NEAREST at exactly 1:1,
LINEAR otherwise (works on the INDEX8 texture in XEMU; untested on hardware). E.g. 720p 1.5x with
4% safe area = 848x480 scaled 1.37x. The log line `svga_init: mode ..., game ..., scale ...` shows
the result. Pointer speed needs no change: its sqrt(area) scaling keeps screen-crossing time the
same at every game size.

**Video mode / resolution.**
- `f1_res.ini` lives at `E:\UDATA\FALLOUT1` and is written by the XBOX OPTS screen (`xboxopts.cc`). It
  sets `SCR_WIDTH`/`SCR_HEIGHT` (the video mode) and `[XBOX] UI_SCALE`/`SAFE_AREA`.
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
  queued at most, so a frame taking longer than ~16 ms drops to 30 fps. Because present already
  paces to vblank, `FpsLimiter::throttle()` is a no-op on NXDK (its extra 16 ms sleep could make a
  present miss the next vblank). Every loop that throttles also presents each pass, so none spins.
- Because every present costs a vblank, code that presents in a tight loop gets slow. Map loading
  registers `gameMouseRefreshImmediately` as the `db` read callback (every 8 KB), which presented on
  each call and capped loading at ~0.5 MB/s (about 20 s to load a save). On NXDK it now presents
  at most every 100 ms, which cut that load to about 2.4 s in XEMU. For the same reason
  `palette_init` no longer times a 60-step test fade on NXDK (always 1 s); it uses 42 steps.

**Load and boot times (measured in XEMU, 2026-11-15).**
- Save load (Cathedral Entrance slot): 2.4 s, of which about 1 s is raw stdio I/O for 5 MB.
  `game_init`: 5.7 s, of which about 1.2 s is I/O for 1.8 MB. The rest is USB enumeration, the
  video mode set (`svga_init`, ~1 s) and game data setup.
- nxdk's stdio has a 1 KB buffer, every `fseek` drops it, and `db.cc` seeks before every read of a
  file stored uncompressed in a `.DAT`. Skipping redundant seeks and/or a bigger `setvbuf` buffer
  were measured and **didn't help**: reads are scattered, so time scales with bytes read, and a
  64 KB buffer made the save load 3x slower. Don't retry without measuring on real hardware.
- The nxdk-sdl3 joystick init waited for USB enumeration with 500 iterations of `SDL_Delay(1)`,
  which took ~1.5 s in XEMU. It is now time-based (500 ms). USB enumeration itself (OHCI reset,
  port reset/debounce per device) still takes ~1.5 s and is needed for pads present at boot.
- To profile init without code changes, pipe XEMU's stdout through a timestamper (a few lines of
  Python stamping each line with host time) and look for the largest gaps between log lines.
- The game renders at the native mode size, so frames are shown 1:1. The texture uses
  `SDL_SCALEMODE_NEAREST`, which the xgu renderer honours, so any scaling would also be unfiltered.
- Shortly after the main menu appears, SDL sends a spurious `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED`
  (1280×720 → 1280×720). Recreating the renderer for it caused a black screen at 720p, so
  `handleWindowSizeChanged()` does nothing on NXDK. Don't tear down or recreate the xgu renderer
  mid-session.
- Many nxdk libm functions are `assert(0)` stubs that hang the game silently: `exp2`, `log2`, `cbrt`,
  `scalbn`, `lround`, `lrint`, `fma`, `expm1`, `log1p`, `erf`, the hyperbolic inverses and more
  (`grep -l "Not implemented" ~/xbox/nxdk/lib/pdclib/platform/xbox/functions/math/*.c`). `exp`,
  `log`, `pow` and `sqrt` work.
- `debugPrint` (nxdk `hal/debug.h`) draws on screen, not to serial. Use `DbgPrint` or `debug_printf`
  for the serial log.

**Smooth right-stick panning.** The right stick pans the map pixel by pixel instead of in the
engine's 32x24 px scroll steps.
- `tile.cc` keeps a sub-step camera offset (`tile_fine_x`/`tile_fine_y`, 0..31 and 0..23) that
  `tile_set_center` folds into `tile_offx`/`tile_offy`. Every screen<->map conversion (objects,
  floor/roof, text, mouse picking, bounds) goes through those, so they all follow automatically.
- `tile_scroll_fine` moves the camera N pixels, taking whole scroll steps as the offset passes them
  (net screen shift of a step is zero). It only shows part of the next step if `tile_set_center`
  would allow that step, so it reaches exactly the area stepped scrolling can.
- `map_scroll_pixels` (map.cc) shifts the map buffer by exactly that many pixels and redraws the
  strips that come into view, sharing `map_shift_display` with `map_scroll`. It isn't limited to
  one call per 33 ms like `map_scroll`.
- `ProcessRightStick` (gamepad.cc) turns stick deflection into a pixel velocity, keeping the old
  speed: one 32x24 step per `gRightStickInternalDivisor` ms at full deflection.
- Cost in XEMU (2026-11-16, -O2 nxdk + fast pdclib copies): 2.5-4.5 ms average (up to ~9 ms) per
  frame at 720p, mostly the strip redraw. (480p measured ~5 ms before those library fixes.) 720p holds 56-60 fps while panning, so an occasional frame still drops.
- The real hex cursor is hidden while panning (`map_pixel_scrolling()`, checked in
  `gmouse_bk_process`) and comes back 100 ms after the map stops. It can only sit on whole hexes,
  so showing it while the map glides under a still mouse made it slide and jump back a hex. In its
  place `gmouse_show_pan_cursor` sets the mouse shape to a screen-space copy of it (the hex art
  plus the outline `obj_render_outline` would draw, in solid dark red since a mouse shape can't
  blend), built from `obj_mouse_flat`'s current fid. So the hex stays fixed at the center while the
  map moves. Interface art #1 is blank; the visible hex comes from `obj_mouse_flat->fid`.
- `gmouse_3d_off` refreshes the hex's art bounds (plus 1 px for the outline), not just the rect
  `obj_turn_off` returns: that is the object's light area and needn't cover the hex, which left
  hex fragments in the map buffer for the scroll to carry along.
- Strip redraws were dominated by `obj_render_pre_roof`: it visits every object within 320x240 px
  of the strip and `obj_render_object` locked each one's art just to find it doesn't overlap.
  `Object` now caches its last drawn bounds relative to its tile (`renderBounds*`, keyed on fid,
  frame, rotation and x/y offset, runtime only) for an early reject. That cut object drawing
  per strip from ~1.5 ms to ~0.2 ms at 480p. Any new code that allocates an `Object` without
  zeroing it must set `renderBoundsValid = false`.
- While the right stick is deflected, `map_center_mouse()` locks the pointer to the center of the map
  view (PS4 Planescape-style), so the hex cursor lands on the center hex when the camera stops. It
  skips this while map scrolling is disabled (dialogue, enemy turns, loading) or when another
  window covers the screen center (inventory, Pip-Boy). The left stick still moves the pointer.
  The lock is optional: XBOX OPTS "PAN CENTERS CURSOR" sets `[control]
  right_stick_center_cursor` in `fallout.cfg` (default 1). With it off the pan cursor still shows,
  wherever the pointer is.
- `GamepadInit()` (which reads the `[control]` stick settings) used to run only on APPLY in XBOX
  OPTS, so saved deadzone/sensitivity were ignored after a reboot. `game_init` now calls it right
  after `gconfig_init`. Its first `debug_printf` line is garbled because nxdk's `debug_printf` here
  mishandles `%%` and `%.2f`.
- XEMU's default keyboard map puts the right stick on I/J/K/L, which is how it was tested.

**Left stick pointer** (`ProcessLeftStick`, gamepad.cc). Full deflection moves the pointer at
500 px/s at 640x480 at sensitivity 50 (x2 per 25 steps: 1 is ~1/4 speed, 100 is 4x; the right stick uses the same curve around one 32x24 step per 50 ms), scaled by sqrt(screen area / 640x480) (1.73x at
720p) so it feels the same in both modes. Deflection goes through a response curve
(`0.2m + 0.8m^2`) for fine control near the center; LT still brakes to 25%. Timing uses
`SDL_GetTicksNS` with fractional pixels carried between frames.
- `GNW95_process_message` runs the stick/trigger processing **after** draining SDL events, so the
  sticks use this frame's axis values (it used to lag one frame).
- `ScaleJoystickAxes` used to return raw ±32767 values when the deadzone was 0%, which made a 0%
  deadzone unusable; it now always normalizes.
- XEMU's default keyboard map puts the left stick on E/S/D/F (full deflection only, so the curve
  can't be tested from the keyboard).
- **Pointer assist** (`gmouse_pointer_assist`, gmouse.cc; XBOX OPTS "POINTER ASSIST",
  `[control] pointer_assist`, default on). `ProcessLeftStick` passes each frame's movement through
  it. Only in the map view in ARROW, CROSSHAIR and skill/use modes (not MOVE, not over UI windows,
  not while panning). It lists objects within 8 hexes of the pointer (`obj_list_near_tile`,
  object.cc) and keeps the ones the mode can act on (CROSSHAIR: living critters; otherwise
  critters, ground items, usable scenery; roof-hidden ones skipped). Over one (pixel-exact
  `obj_intersects_with`) the pointer moves at 45% speed (friction). Within 40 px of one's bounds
  and moving toward it, the path bends toward its center without changing speed (magnetism);
  moving away is never resisted. Tuning constants are at the top of the function.
- **UI pointer assist** (`win_pointer_assist`, button.cc), same setting. Runs first; the map assist
  only runs when it finds no button. It looks at the top window under the pointer and its buttons
  (skipping disabled ones, ones with no events/procs, ones over 460x120 or covering half the
  window). Pull radius 24 px; friction (50%) only on buttons 40 px or less in one dimension, and
  nothing at all while inside a bigger one (e.g. the active item slot). The steering maths for both
  assists is in `plib/gnw/pointer_assist.h`.
- **Pointer haptics** (`[control] pointer_haptics`, default 0; the POINTER ASSIST knob's +RUMBLE
  position, which also means assist on). Both assists report the button/object the pointer is on;
  `ProcessLeftStick` plays a 45 ms small-motor tick when that changes to a new one.
- **D-pad target jumping** (`gmouse_jump_to_target`, gmouse.cc; called from the arrow-key cases in
  `game.cc` `game_handle_input`, so only in the map view). Jumps the pointer to the nearest
  pickable object within ~50 degrees of the pressed direction (same filter as the map assist, on
  screen, not under a roof), switching walk mode to the arrow, or in combat to the crosshair when
  the active item allows it. With nothing in that direction it does nothing (the d-pad no
  longer scrolls the map on NXDK; the right stick pans).
  Dialogs/menus with their own d-pad navigation run their own input loops and are unaffected.
- **D-pad selection** (gmouse.cc `gmouse_select_target` / `gmouse_track_target`, called at the start
  of `gmouse_bk_process`). The jumped-to object gets the gold item outline (`OUTLINE_TYPE_ITEM`)
  unless it already has one (combat outlines every critter; only an outline we added is removed).
  The pointer aims at a solid pixel of the sprite (nearest to the bounds' middle), and follows the
  object when it moves more than 8 px or the pointer is no longer on it, so walking NPCs stay
  selected. The selection drops when anything else moves the pointer (left stick, centering lock,
  warps), on walk mode, map panning, dialogue, when it leaves the screen or stops being a valid
  target. Each frame it checks the object still exists by looking for its pointer among nearby
  objects (never dereferencing first); `obj_erase_object` and `obj_disconnect` (pickups) call
  `gmouse_forget_target`, and `gmouse_reset` clears it without touching the object.
- The Preferences "Mouse Sensitivity" slider has no effect on Xbox (the scaling in `mouse.cc` is
  in the non-NXDK branch).

**Rumble** (`game/rumble.cc`, `GamepadRumble` in gamepad.cc). `[control] rumble` 0-100 scales
everything (0 = off, default 100); XBOX OPTS previews it while the slider moves. Rumble goes to
**every** open pad, and an overlapping weaker rumble never cuts a stronger one short. It used to
follow the pad that last sent input, but XEMU can deliver one host pad's input on two emulated
ports (the second one last, with no motor behind it), so shots never rumbled for the user. Events:
- Player hit (scaled by damage / max HP), knocked down/out, killed; the player's critical hits; an
  enemy's critical hit on the player is stronger; explosions (fading out to 16 hexes); poison ticks;
  recoil when the player fires a gun (`rumble_cue_fire`: by weapon skill, longer for bursts).
- Recoil is queued right after the attacker's attack animation in `action_ranged`; the next step
  registered there waits for the animation's action frame (projectile shown, target dodges, or the
  target's hit), so it lands on the shot. Pending cues merge (stronger motor, longer duration), so
  recoil plus a critical-hit kick on the same step both count. Use `register_rumble_clear` to drop
  a cue nobody took.
- **The game stops the motors itself** (`GamepadRumbleUpdate`, called from `process_bk`). SDL only
  expires a rumble while events are pumped, and `combat_turn_run` runs whole enemy turns calling
  only `process_bk`: a knockdown rumble ran until the player's next turn.
- Combat damage is only *applied* after the attack animation ends, so hit rumble isn't fired from
  `apply_damage`. `show_damage_to_object` calls `register_rumble_next(cue)`, and the next animation
  step it registers carries the cue (`AnimationDescription::rumble`, attached in `check_registry`)
  and plays it when `anim_set_check` starts that step: the frame the hit shows. It takes no extra
  step: a sequence holds only 40 and overflowing cancels the whole attack. Explosions mark their
  sound step the same way. Paths without animation (`action_explode`/`action_dmg` unanimated)
  play the cue directly.
- Verified in XEMU via the virtual pad's rumble log (menu preview, pointer ticks, an explosion next
  to the player plus its hit, pistol recoil on a hit and a miss ~450 ms after the attack is queued,
  a stop during a non-pumping loop). Not felt on real hardware yet; enemy hits, criticals and death
  weren't triggered in a test.

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
