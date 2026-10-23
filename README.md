# Wii Party native

Wii Party running as a native Windows program, at 60 fps and in HD, with up to 4 players and online
play with friends. No emulator: the game's PowerPC code is statically recompiled to C, and a native
runtime stands in for the Wii hardware it talks to (the same approach as N64Recomp and
XenonRecomp). Dolphin's source and docs were the reference for how the hardware behaves.

**No game data is in this repository.** No game code, no disc files, no keys. Every player builds
the game from their own Wii Party disc.

## What works

- The game itself: board games, minigames (all of them are translated), saves and Miis
- OpenGL renderer at any resolution (1080p, 1440p, 4K), 16:9 widescreen or 4:3, full screen (F11),
  and a software renderer as a fallback
- Sound (the Wii's AX audio, running natively)
- Keyboard and mouse as a Wii Remote (pointer, shake, swing, tilt, sideways grip), plus gamepads
  (Xbox, PlayStation, Switch)
- Online play: one person hosts and gets a room code, friends type it and join. Each player runs
  the game on their own PC; only controller input travels over the internet (lockstep netplay
  over UDP with hole punching, so usually no port forwarding is needed)
- A launcher for Windows: find the game, settings, key mapping, hosting and joining

## Play it

You need Windows 10 or 11, your own Wii Party disc copied to the PC, and 10-40 minutes for the
first build.

1. **Copy your disc to the PC as a folder.** With the free [Dolphin](https://dolphin-emu.org)
   emulator: add your disc image, right-click Wii Party > Properties > Filesystem, right-click the
   disc > **Extract Entire Disc**, and pick an empty folder. Dolphin decrypts it, so this project
   never needs Wii keys.
2. **Build the game.** Download this repository (green **Code** button > **Download ZIP**) and
   unzip it to a folder without spaces in its name, for example `C:\WiiParty`. Double-click
   **Build Wii Party.bat** and pick your disc folder when it asks. It downloads a C compiler, CMake
   and Python (once, into `tools\`), translates the game to C and compiles it. It needs about 6 GB
   of free space. Leave the black window open until it says Done.
3. **Play.** Open the new **Wii Party** folder, double-click **Wii Party.bat**, press
   **Find my game** and **PLAY**.

If Windows says "Windows protected your PC", press **More info** and then **Run anyway**.

**Updating.** In the launcher, Settings > **Update the game** downloads the newest version from
GitHub and builds it again with your disc. Your settings stay, and only what changed is compiled
again, so it is usually much quicker than the first build. When a new version is out, the launcher
says so on the Play page. Everyone playing online together needs the same version.

## Controls

One player per PC by default: the keyboard, the mouse and one gamepad all play player 1. For more
players on one PC, pick 2-4 under Settings > Players on this PC; gamepads are then players 2-4.
A gamepad joins when you press a button on it.

| Wii Remote | Keyboard and mouse | Gamepad |
|---|---|---|
| Pointer | Mouse | Right stick |
| A / B | Left click, Enter, Space / Right click, Backspace | A (Cross) / B (Circle), right shoulder |
| 1 / 2 | 1 / 2 | X / Y |
| + / - / HOME | = / - / H | Start / Back / Guide |
| D-pad | Arrow keys, W A S D | D-pad |
| Shake | Middle click, Left Shift | Left shoulder |
| Swing up / down | Mouse wheel, T / G | |
| Tilt forward / back, turn left / right | R / F, Q / E | Left stick |
| Upright or sideways grip | F1 | |

F11 switches full screen. Every keyboard key can be changed in the launcher.

## Online

Everyone needs the game built from their own disc, from the same version of this repository and a
disc from the same region. On the launcher's Online page the host picks how many friends join and
presses **HOST**; the room code is copied to the clipboard. Friends type it and press **JOIN**.
The host is player 1, friends are players 2-4 in the order they join, and everyone plays with the
host's Miis and save.

How the games find each other: the host's game posts its addresses (found with STUN) to a topic on
[ntfy.sh](https://ntfy.sh) named after a hash of the room code. The friend's game reads them, and
both games send packets to each other at the same time so the routers let the connection through.
If that fails, the host can forward UDP port 47123 and friends join with the host's IP address, or
everyone joins the same virtual LAN (Radmin VPN, ZeroTier).

Every PC runs the whole game deterministically: inputs are delayed by a few frames, and the games
compare hashes of memory as they play to catch a desync.

## How it works

| Piece | Where |
|---|---|
| DOL / REL parsers, Yaz0 | `recomp/formats.py`, `recomp/lz.py` |
| PowerPC to C recompiler (integer, FPU, paired singles, jump tables) | `recomp/recompile.py`, `recomp/emit.py`, `recomp/ppc.py` |
| Minigame modules (RELs): translation and linking | `recomp/rel.py`, `runtime/src/rel_link.c` |
| Functions replaced by native code (OS, threads, Remote input) | `recomp/hle_SUPP01.json`, `runtime/src/threads.c`, `runtime/src/hle_input.c` |
| Boot, memory map, SPRs, quantized loads/stores, hardware registers | `runtime/src/boot.c`, `runtime/src/cpu_services.c`, `runtime/src/hw.c` |
| GX: command decoder, OpenGL renderer (uber TEV shader), software renderer | `runtime/src/gx_fifo.c`, `runtime/src/gx_gl.c`, `runtime/src/gx_render.c` |
| DSP / AX audio | `runtime/src/dsp.c`, `runtime/src/platform_audio.c` |
| IOS: disc, NAND saves, devices | `runtime/src/ipc.c`, `runtime/src/ios_devices.c` |
| Window and input (SDL3) | `runtime/src/platform_sdl.c` |
| Deterministic mode and netplay (ENet) | `runtime/src/det.c`, `runtime/src/netplay.c` |
| Launcher and build kit | `dist/`, `kit/` |

## Build by hand

```
python recomp/recompile.py --dol <game>/sys/main.dol --hle recomp/hle_SUPP01.json --out gen
python recomp/rel.py --dol-gen gen --out gen --hle recomp/hle_SUPP01.json --rel <game>/files/rel/*.rel.lz
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DWPR_GEN_DIR=<full path>/gen
cmake --build build -j
build/wiiparty-native <game>
```

`<game>` is the extracted disc folder (the one with `sys` and `files`). Windows builds use MinGW
(w64devkit); `kit/build-kit.ps1` shows the exact steps. `gen/report.json` lists any instruction the
recompiler doesn't translate. Generated code, builds and disc files are ignored by git and must
never be committed.

## Credits

[ENet](http://enet.bespin.org) (MIT, in `third_party/enet`), [SDL3](https://libsdl.org), and the
[Dolphin](https://dolphin-emu.org) project's documentation of the Wii hardware.

Wii Party is a trademark of Nintendo. This project is not affiliated with or endorsed by Nintendo.
