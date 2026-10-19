# ds4x360-bl616 — DualShock 4 → Xbox 360 (XUSB) adapter

[中文文档 (Chinese)](README_CN.md) · [Build guide](BUILD.md) · [Flashing guide](FLASHING.md)

A BL616/BL618 adapter that connects a **DualShock 4 (DS4)** over Bluetooth Classic (BR/EDR) HID
and presents it to the host as an **Xbox 360 wired controller** (XUSB, VID:PID `1209:D1CE`) with a
**UAC1 audio composite** function. Windows binds it through `usbccgp` → `xusb22` (XInput) +
`usbaudio`; no driver install is needed.

The patched Bouffalo SDK is vendored in `third_party/bouffalo_sdk`, so a clone builds without an
external SDK checkout. See [SDK_VENDORING_CN.md](docs/SDK_VENDORING_CN.md) for how the vendor was
pruned, and `SDK_CHANGES.md` for every local patch.

> **Unofficial project.** Not affiliated with or endorsed by Sony or Microsoft. "DualShock" and
> "Xbox" are trademarks of their respective owners. `1209:D1CE` is a pid.codes-style development
> VID/PID used locally; apply for a proper allocation before public distribution.

## Variants / Build / Flash

The product is the XInput persona. Two variants are published; the DS4 persona is **reference-only**
(regression target) and is not shipped.

| Variant | Build | Host sees |
|---|---|---|
| `xinput` (**default**) | `./build.sh` | XUSB pad `1209:D1CE` + UAC1 audio |
| `xinput-noaudio` | `XINPUT=1 AUDIO=0 ./build.sh` | XUSB pad only, no audio function |

- **Build:** [BUILD.md](BUILD.md) / [BUILD_CN.md](BUILD_CN.md) — vendored SDK, automatic Xuantie
  toolchain download (`./setup_toolchain.sh`), Linux/WSL2 commands, native-Windows limitation.
- **Flash:** [FLASHING.md](FLASHING.md) / [FLASHING_CN.md](FLASHING_CN.md) — ROM ISP `349b:6160`,
  `BLFlashCommand --config=flash_prog_cfg-<variant>.ini`, Dev Cube alternative.
- `./release.sh` builds both variants into `release/` (firmware, flash configs, licenses,
  `SOURCE.txt`, `SHA256SUMS`).
- DS4 reference build: `XINPUT=0 ./build.sh` — builds, but is not a product target.

## Supported boards

| Board | Chip | Status |
|---|---|---|
| **LCTech BL616** (default) | BL616 QFN32 | **Hardware-validated** (XUSB + UAC1 composite) |
| Ai-M61-32S-Kit | BL618 QFN56 | Builds; hardware untested |
| Sipeed M0S Dock | BL616 QFN32 | Builds; hardware untested |

Only the LCTech BL616 has been validated on hardware. The other boards compile, but their USB
wiring, LED behaviour and audio timing have not been verified.

### USB wiring (Ai-M61-32S-Kit only)

The on-board Type-C port is a UART bridge (debug/flash). Native USB must be wired from header pins:

```
Pin 37 (USB_DM) → USB D- (white)
Pin 38 (USB_DP) → USB D+ (green)
GND             → USB GND (black)
```

LCTech BL616 / M0S Dock use the on-board Type-C for native USB; no wiring is needed.

## Getting started

### 1. Dependencies

```bash
# Linux / WSL2 (Ubuntu/Debian)
sudo apt install -y build-essential cmake python3 curl xz-utils git

# macOS
brew install cmake make xz   # GNU Make >= 4.0 (use gmake); xz unpacks the toolchain
```

CMake >= 3.15 and GNU Make >= 4.0 are required.

### 2. RISC-V toolchain

The SDK needs the T-Head extensions (`xtheade`, `zpsfoperand`); distro and Homebrew
`riscv64-elf-gcc` cannot compile it. `build.sh` finds an existing Xuantie toolchain or downloads it
once via `./setup_toolchain.sh` (~95 MB, SHA-256 verified) into `third_party/toolchain/`
(git-ignored). To do it manually:

```bash
./setup_toolchain.sh          # idempotent
./setup_toolchain.sh --force  # reinstall
```

Or point at your own toolchain: `CROSS_COMPILE=/path/to/riscv64-unknown-elf- ./build.sh`.

### 3. Build

```bash
./build.sh                    # default: XUSB + UAC1 audio composite
XINPUT=1 AUDIO=0 ./build.sh   # XUSB only (diagnostic)
XINPUT=0 ./build.sh           # reference: DS4 persona (not shipped)

./build.sh rebuild            # clean + rebuild
./build.sh clean              # clean only
./build.sh strip              # delete intermediates, keep firmware
```

- Default board: `BOARD_TYPE=lctech616`; overrides are `aim61` / `m0sdock`.
- **USB speed:** Full-Speed only (12 Mbps). `USB_SPEED=hs` is rejected.
- Output: `firmware/lctech616/ds4dongle-lctech616-xinput.bin` (composite) and
  `firmware/lctech616/ds4dongle-lctech616-noaudio-xinput.bin`, plus `boot2_*.bin`,
  `partition.bin` and `flash_prog_cfg.ini`. Build outputs are git-ignored.
- `DEBUG_CONSOLE=1` / `MIC_DIAG=1` change behaviour: `DEBUG_CONSOLE` adds the UART log mirror /
  HID 0xB4 log ring, `MIC_DIAG` enables mic diagnostics (and changes audio pacing). They also
  produce `-dbg` / `-diag` variants that never overwrite the release names.
  `DEBUG_CONSOLE` counts as enabled only when non-empty and not `0`.

### 4. Native Windows

Native Windows is **not supported out of the box**: the trimmed SDK no longer ships `tools/make`,
`tools/cmake` or `tools/ninja`, and `project.build` hardcodes `tools/cmake/bin/cmake.exe` on
Windows. Use **WSL2** (recommended), or supply system make/CMake and pass `CMAKE=cmake`, or point
`BL_SDK_BASE` at a full SDK. Details: [BUILD.md](BUILD.md) §2.2.

### 5. Flash

Short version (validated path): unplug, hold **BOOT**, plug the board's USB port, wait for the
`349b:6160` CDC port, then run `BLFlashCommand` with the variant's ini. Full instructions,
including the WSL2/usbipd flow and the Dev Cube alternative, are in
[FLASHING.md](FLASHING.md) / [FLASHING_CN.md](FLASHING_CN.md).

## Usage

1. Connect the board's Type-C port to the host (power + data).
2. Put the DS4 in pairing mode: hold **PS + Share/Create** until the light bar flashes.
3. The host enumerates an "Xbox 360 Controller for Windows" plus an audio device
   (`1209:D1CE`, composite).
4. The on-board LED shows the connection state (table below).

### LED status (LCTech BL616, single blue LED on GPIO27)

| State | Pattern |
|---|---|
| Idle / waiting to pair | Slow blink (~1 Hz) |
| Scanning | Fast blink (~3 Hz) |
| Connected | Solid |
| Just disconnected | Blink (~1 Hz) for ~3 s, then idle |
| Battery ≤20% (discharging) | Medium blink (~1.7 Hz) |
| Battery ≤10% (discharging) | Fast blink (~3 Hz) |
| Auto-off | Off after 1 min |
| Event acknowledge | Single flash |
| Bonds cleared | Triple flash |

### BOOT button gestures

| Gesture | Action |
|---|---|
| Single click | Switch to the next remembered controller (up to 8) |
| Double click | Disconnect and scan for a new controller (bonds kept) |
| Long press (3 s) | Clear all bonds and start scanning (triple flash) |

## Features

### Implemented (XInput persona)

- BT Classic HID host: inquiry, SDP, L2CAP, SSP pairing, reconnection watchdog.
- DS4 input → XUSB 20-byte report: buttons, D-pad, sticks (radial deadzone, Y-axis inversion),
  analog triggers, Guide (= PS button).
- XUSB output → DS4: rumble (strong/weak) and LED command forwarded over BT `0x05`.
- Touchpad-as-modifier turbo: hold touchpad + target button (A/B/X/Y/LB/RB) for ≥400 ms →
  15 Hz pulses; short presses pass through untouched.
- Neutral report on disconnect (no stuck buttons); 4 ms pacing (host observed ~126 Hz).
- Bidirectional UAC1 audio in the composite build: 32 kHz stereo speaker out, 16 kHz microphone in.
- Multi-controller memory (up to 8 pairings) and BOOT-gesture switching.
- Board selection at compile time (LCTech BL616 / Ai-M61-32S-Kit / M0S Dock).

### Known limitations

| Item | Description |
|---|---|
| Single active controller | One DS4 connected at a time; up to 8 remembered pairings. |
| XUSB report set | No touchpad coordinates, gyro or battery reporting in the XUSB report; the touchpad is only a turbo modifier. |
| Audio | UAC1 composite only; no HD haptics or adaptive-trigger audio path. |
| `1209:D1CE` | Development VID/PID; apply for a proper allocation before distribution. |

### DS4 persona (reference only)

This repository also builds the original DS4 HID persona (`XINPUT=0`) for regression comparison.
It is **not** the product: it is not built by `release.sh`, not hardware-targeted here, and its
descriptor/protocol details are kept only so shared USB/BT changes can be validated. The DS4
persona's HID report layout, feature reports and DualSense-era material from the upstream project
do not apply to the XInput builds.

## Technical notes

### XUSB input report (20 bytes)

| Offset | Content |
|---|---|
| 0 | `0x00` |
| 1 | `0x14` (report length) |
| 2–3 | Buttons, `uint16` LE (`& 0xF7FF`) |
| 4 | LT (0–255) |
| 5 | RT (0–255) |
| 6–7 | LX `int16` LE |
| 8–9 | LY `int16` LE (upward positive) |
| 10–11 | RX `int16` LE |
| 12–13 | RY `int16` LE |
| 14–19 | Reserved (`0`) |

Button bits: D-pad `0x0001–0x0008`, Start `0x0010`, Back `0x0020`, L3/R3 `0x0040/0x0080`,
LB/RB `0x0100/0x0200`, Guide `0x0400`, A/B/X/Y `0x1000/0x2000/0x4000/0x8000`.

### Audio path

The composite build exposes interface 0 as XUSB and interfaces 1–3 as UAC1 (audio control,
speaker AS OUT, mic AS IN). Speaker audio is 32 kHz stereo; the microphone is 16 kHz mono. The
mic EP is `0x84` (FIFO index 4 → `FIBC3`, `0x1BC`); the speaker is ISO OUT `0x01` (FIFO0).
See `docs/phase1-audio-spec.md` for the endpoint map.

### Why the mic FIFO diagnostic is endpoint-derived

`ds4_usb_audio.c` reads the mic FIFO occupancy via `FIBCn` derived from the mic endpoint:
`0x1B0 + 4 * ((ep & 0x7F) - 1)` → DS4 `0x1B4`, XInput `0x1BC`. A hardcoded `FIBC1` would read the
XUSB IN FIFO in the composite build.

## Project structure

```
src/                        application (BT host, USB, XInput mapping, audio)
tools/                      host-side tools (hidlog, descriptor checks, unit tests)
docs/                       design/verification notes (Chinese)
third_party/bouffalo_sdk/   vendored patched SDK (Apache-2.0)
build.sh                    canonical build entry point
build_macos.sh              Linux/macOS/WSL build implementation
release.sh                  assemble release/
setup_toolchain.sh          download the Xuantie toolchain
SDK_CHANGES.md              SDK patch notes
misc/                       local notes/logs (git-ignored)
```

## Documentation

| Document | Content |
|---|---|
| [BUILD.md](BUILD.md) / [BUILD_CN.md](BUILD_CN.md) | Toolchain, build commands, Windows limitation |
| [FLASHING.md](FLASHING.md) / [FLASHING_CN.md](FLASHING_CN.md) | ISP entry, ini layout, BLFlashCommand, Dev Cube |
| [docs/SDK_VENDORING_CN.md](docs/SDK_VENDORING_CN.md) | How the SDK was trimmed and vendored |
| `SDK_CHANGES.md` | Every local SDK patch (base `42c20811` → local `ec8c6e9d`) |
| `docs/phase1-verification.md` | Hardware acceptance record for the composite build |
| `docs/phase2-gamepad-spec.md` | DS4 → XUSB mapping and turbo semantics |

## Acknowledgements

- [awalol/DS5Dongle](https://github.com/awalol/DS5Dongle) — the upstream adapter this project's
  BT/USB plumbing derives from (MIT).
- [bouffalolab/bouffalo_sdk](https://github.com/bouffalolab/bouffalo_sdk) — BL616 SDK, vendored
  with local patches.
- [CherryUSB](https://github.com/cherry-embedded/CherryUSB) — USB device stack.
- `dakkidaze/ds4dongle-bl618` — the DS4-persona reference project.

## License

GPL-3.0; see [LICENSE](LICENSE). Third-party notices are in [NOTICE](NOTICE); the vendored SDK is
Apache-2.0 (`third_party/bouffalo_sdk/LICENSE`).
