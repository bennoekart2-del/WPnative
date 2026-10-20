# Wii Party native (static recompilation)

Goal: run Wii Party as a native Windows program with 4-player local play, by statically
recompiling the game's PowerPC code to C and providing a native runtime for the Wii hardware it
talks to. Same approach as N64Recomp / XenonRecomp. Dolphin's source and docs are the reference for
hardware behaviour.

**No game data is in this repo.** You build it from your own disc.

## Status (2026-10-20)

| Piece | State |
|---|---|
| DOL / REL parsers | done (`recomp/formats.py`) |
| PowerPC → C recompiler | integer, branches, loads/stores, FPU, paired singles, psq_l/st, jump tables; tested end to end (`tests/test_recomp.py`) |
| Coverage report on the real game | needs your disc |
| Runtime: memory map, SPRs, quantized loads | done (`runtime/src/cpu_services.c`) |
| Boot: load DOL, low-memory globals | done (`runtime/src/boot.c`) |
| Window, 4 gamepads as Wii Remotes, XFB display | done (SDL3, `runtime/src/platform_sdl.c`) |
| GX command decoder (framing, vertex sizes, display lists) | done, draws are counted not rendered yet |
| GX renderer (TEV, textures, EFB copies) | **not started** — this is what puts the boot screen on screen |
| OS threads, interrupts, alarms (HLE) | not started |
| IOS: disc reads, NAND/save, WPAD Bluetooth stack | not started |
| DSP / AX audio | not started |
| REL modules (minigames) loading + relocation | parser done, linking not started |

## Build (Windows)

```
cmake -S . -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
build\Release\wiiparty-native.exe --selftest
```

`--selftest` opens the window and shows colour bars plus one square per connected controller
(moves with the right stick, lights up on any button), to check the 4-player input path.

## Build with your game

1. In Dolphin: right-click Wii Party → Properties → Filesystem → right-click the disc →
   **Extract Entire Disc** to a folder. (Dolphin decrypts it; this repo never needs Wii keys.)
2. `python recomp/recompile.py --dol <folder>/DATA/sys/main.dol --out gen`
3. `cmake -S . -B build -DWPR_GEN_DIR=%CD%/gen && cmake --build build --config Release`
4. `build\Release\wiiparty-native.exe <folder>/DATA`

`gen/report.json` lists which instructions the recompiler doesn't translate yet.

## Controls

A/Cross = A, B/Circle = B, X = 1, Y = 2, Start = +, Back = −, Guide = Home, D-pad = D-pad,
right stick = pointer, left stick = tilt. Player 1 can also use mouse + keyboard.
