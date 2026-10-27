# Xontra

An original Xbox port of Contra (NES, US) with local co-op, public netplay
rooms, optional passcodes, and rollback networking.

Based on [Team-Resurgent/Contra-NES-X](https://github.com/Team-Resurgent/Contra-NES-X).
The game runs on a dedicated NES core; video, audio, controllers, and storage use RXDK.

## Downloads and installation

Download the Xbox package from [Releases](https://github.com/sirdankz/Xontra/releases/latest).
The ZIP contains `default.xbe`, installation instructions, the license, and notices.
ROMs, game screenshots, music, and cartridge-derived artwork are excluded.
Dashboard artwork is original to Xontra.

Extract the ZIP into an Xbox game folder and add a compatible Contra (US)
cartridge dump separately:

```text
default.xbe
Contra.nes
```

`data/Contra.nes` is also supported. Additional accepted names beside the XBE
are `baserom.nes`, `Contra (USA).nes`, and `Contra (U).nes`.

Launch `default.xbe` to open the front menu. A missing or unsupported cartridge
displays a cartridge screen. **A** retries and **B** returns to the dashboard.

## Features

- Single-player and two-player local co-op.
- LAN play and public internet rooms with Open or four-digit Passcode access.
- Automatic input delay or a fixed delay of 0-12 frames.
- Configurable rollback limit of 0-3 frames.
- Rapid fire, local save states, image smoothing, and optional sprite-limit removal.
- Dashboard-selected 480i, 480p, or 720p output with integer scaling.
- PAL50 output with the NES simulation running at 60 frames per second.

## Controls

| Xbox control | Function |
| --- | --- |
| D-pad / left stick | Move and aim |
| A | Jump |
| B / X | Fire |
| Y | Rapid fire |
| Back | NES Select; choose 1 or 2 players at the title |
| Start | Start / pause |
| White | Save state while paused, local play only |
| Black | Load state while paused or at the title, local play only |
| Left-stick click | Toggle picture smoothing |
| Right-stick click | Toggle sprite limit, local play only |
| Back + Start | End the game and return to the front menu |
| Back + Start + both triggers | Return to the dashboard |

The first two connected controllers control local P1 and P2. Network sessions
use the first connected controller on each console. Saving replaces the previous
state in the title's Xbox save directory.

## Multiplayer

Both consoles require the same client build and compatible cartridge. The host
controls P1; the joining console controls P2.

1. **Netplay Settings** selects Auto or 0-12 delay frames and a 0-3 rollback
   limit. Left/Right changes values and **B** returns. The host's settings apply
   to both players.
2. **Online Play > Host Room** opens the room-creation menu. Choose Open or
   Passcode access, then select **Create Public Room**. Both room types appear
   in the public browser.
3. **Set Passcode** opens the four-digit editor. Left/Right selects a digit,
   Up/Down changes it, and **A** confirms. Leading zeros are supported.
4. **Online Play > Browse Rooms** lists room IDs, `OPEN` or `CODE` access, and
   occupancy. Up/Down selects a room and **A** joins. Protected rooms prompt for
   the code; **B** returns to the browser.
5. After both players connect, the host presses **A** on **Start Game**. At the
   NES title, the host selects **2 PLAYERS** with **Back** and presses **Start**.

LAN sessions use **LAN Play > Host LAN Game / Join LAN Game** on the same subnet.
**B** cancels a lobby; **Back + Start** ends the session. Starting from the front
menu begins a fresh game.

### Delay and rollback

Auto adjusts delay to connection latency, jitter, and the selected rollback
limit. Fixed delay stays at the selected value. Zero rollback waits for real
remote input; lower delay can cause more waiting on slow connections. Local
play has no added netplay delay.

Prediction holds the last known remote input. Corrections restore a complete
machine snapshot and replay within the selected limit, capped at three frames
beyond the oldest unconfirmed input. Audio is released only for confirmed
frames, without duplicate playback during correction.

The port draws no HUD text during netplay. Room messages and errors appear in
the menus. Save/load and sprite-limit changes are disabled during network play;
picture smoothing is local to each console.

## Building from source

Requirements:

- Windows 10 or 11.
- RXDK toolchain and SDK, normally in `C:\ProgramData\RXDK`; `RXDK_ROOT` selects
  a different installation.
- Python 3.8+, using the standard library.
- A host C compiler: RXDK clang with its Windows host sysroot, Visual Studio C,
  or GCC.
- A compatible Contra (US) ROM named `baserom.nes` in the repository root.

| Cartridge | Size | MD5 | CRC32 |
| --- | --- | --- | --- |
| Contra (US) | 131,088 bytes | `7BDAD8B4A7A56A634C9649D20BD3011B` | `C50A8304` |

Unused iNES header-byte differences are accepted. PAL Probotector is unsupported.

```powershell
.\build.bat --no-assemble
```

The build validates the cartridge, runs the native regression suite, generates
original dashboard icons and project metadata, and creates the executable and ISO.

Outputs:

- Game folder: `out\Release\Build\Contra-NES-X\`
- Disc image: `out\Release\XISO\Contra-NES-X.iso`

Additional options:

```powershell
.\build.bat --check
.\build.bat --no-assemble --config Debug
.\build.bat --no-assemble --skip-build
```

`--check` lists prerequisites. `--skip-build` prepares host tools, tests, icons,
and project files before the Xbox compilation step. The prepared
`Contra-NES-X.sln` supports Visual Studio with RXDK.

The optional `build.bat` command without `--no-assemble` fetches pinned revisions
of [nes-contra-us](https://github.com/vermiceli/nes-contra-us) and
[cc65](https://github.com/cc65/cc65). It requires Git and an initial internet connection.

### Release packaging

After building:

```powershell
python -B tools/xbox/package_release.py --version 0.1.0
```

Output under `out/public-release/0.1.0/` includes the ROM-free ZIP, a separate
`default.xbe`, and SHA-256 sums. The packager checks executable freshness, the
netplay fingerprint, original dashboard resources, and embedded cartridge data.
The ZIP includes only `default.xbe`, `README.txt`, `LICENSE`, and `NOTICE`.

### Development checks

```powershell
python -B tools/xbox/release_check.py
```

The game build also runs `hosttest build/Contra.nes --netplay-test` against the
NES core. The source-release check excludes game data, build outputs, archives,
backups, credentials, and machine-specific paths.

## Credits and license

- Original Xbox port: [Team Resurgent](https://github.com/Team-Resurgent/Contra-NES-X).
- Contra: Konami, 1988.
- [nes-contra-us](https://github.com/vermiceli/nes-contra-us): annotated Contra
  disassembly, building on Trax's work.
- [cc65](https://github.com/cc65/cc65): optional disassembly-build assembler and linker.
- RXDK: [Team Resurgent](https://github.com/Team-Resurgent).
- Undead Neighbor Eaters / ZAMN netplay: transport and frame-history design reference.
- Klaus Dormann's 6502 functional test: supported by the CPU test harness;
  test binaries are excluded.

Project code is licensed under the [MIT License](LICENSE). External components
retain their own terms; see [NOTICE](NOTICE). Xontra is not affiliated with or
endorsed by Konami.
