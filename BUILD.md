# Build Guide (Linux / WSL2 / Windows)

This is a from-scratch guide: install the dependencies, clone the repository, build a flashable
XInput firmware. The patched Bouffalo SDK is vendored at `third_party/bouffalo_sdk`; no separate
SDK checkout is needed.

This repository ships the **XInput (XUSB) persona only**: the default build is an Xbox 360 wired
pad plus the UAC1 audio function. The DS4 persona is kept for reference/regression.
**Linux / WSL2 is the supported build path**; native Windows has a known limitation (section 2.2).

---

## 0. Requirements

| Item | Notes |
|---|---|
| Disk | ~1.5 GB with the toolchain (the extracted toolchain is ~700 MB and can be deleted) |
| Network | the first build downloads the official RISC-V toolchain (~95 MB); later builds are offline |
| Hardware | LCTech BL616 (the only board validated on real hardware) |

System packages (Debian / Ubuntu / WSL2):

```bash
sudo apt update
sudo apt install -y build-essential cmake python3 curl xz-utils git
```

- `build-essential`: gcc and GNU Make (**>= 4.0**; Ubuntu 20.04+ satisfies this)
- `cmake`: **>= 3.15**
- `python3`: used by the SDK image-generation scripts
- `curl` + `xz-utils`: download and extract the toolchain
- `git`: clone the repository

Fedora equivalent:

```bash
sudo dnf install -y gcc gcc-c++ make cmake python3 curl xz git
```

macOS prerequisites (Homebrew):

```bash
brew install cmake make xz
# curl and git ship with macOS; xz is needed to extract the toolchain
```

> **Why not the distro `gcc-riscv64-unknown-elf`?**
> The SDK compiles with `-march=rv32imafcpzpsfoperand_xtheade`. `xtheade` and `zpsfoperand` are
> T-Head extensions that upstream GCC does not support. The official Xuantie toolchain is
> required; `build.sh` downloads it automatically.

---

## 1. Linux / WSL2 (recommended)

### 1.1 Clone

```bash
git clone <repo-url> ds4x360-bl616
cd ds4x360-bl616
```

### 1.2 First build (downloads the toolchain)

```bash
./build.sh
```

On the first run it:

1. looks for `riscv64-unknown-elf-gcc` (or `riscv64-elf-gcc`) under
   `third_party/toolchain/*/bin`, `~/riscv-toolchain/toolchain/bin`, and a few system paths;
2. if no compiler is found and `third_party/toolchain/` does not exist, runs
   `./setup_toolchain.sh` once, which downloads
   `xuantie-gcc-riscv-v2.6.1-linux-x86_64.tar.xz` (~95 MB, SHA-256 verified) from the official
   release into `third_party/toolchain/` (git-ignored, never committed);
3. builds with the system `cmake` and that toolchain.

Outputs:

```
firmware/lctech616/ds4dongle-lctech616-xinput.bin          # default: XUSB pad + UAC1 audio
firmware/lctech616/ds4dongle-lctech616-noaudio-xinput.bin  # XUSB only (AUDIO=0)
firmware/lctech616/flash_prog_cfg.ini                      # flash config for this build
firmware/lctech616/boot2_*.bin, partition.bin              # flash payload
```

### 1.3 Build commands

```bash
./build.sh                        # default: XInput + audio composite (recommended)
XINPUT=1 AUDIO=0 ./build.sh       # XUSB only (no UAC1 audio), diagnostic
BOARD_TYPE=lctech616 USB_SPEED=fs ./build.sh  # explicit board/speed; fs is the only supported speed
XINPUT=0 ./build.sh               # reference: DS4 persona (not published by this repo)
```

- `USB_SPEED` only accepts `fs` (Full-Speed 12 Mbps); `hs` is rejected.
- The default board is `BOARD_TYPE=lctech616`. `aim61` / `m0sdock` still build but are **not
  validated on real hardware**.
- `DEBUG_CONSOLE=1` and `MIC_DIAG=1` change **behaviour**, not just the file name:
  `DEBUG_CONSOLE` adds the UART log mirror / HID 0xB4 log ring, and `MIC_DIAG` enables mic
  diagnostic behaviour (it changes the audio pacing). Both also add a `-dbg` / `-diag` suffix so
  they never overwrite the release image. See the usage text from `./build.sh`.

Other actions:

```bash
./release.sh          # build both XInput variants into release/ (firmware + flash configs)
./build.sh clean      # clean
./build.sh rebuild    # clean + rebuild
./build.sh strip      # drop intermediates, keep the firmware
```

### 1.4 Manual / offline toolchain

```bash
./setup_toolchain.sh          # download only (returns immediately if installed)
./setup_toolchain.sh --force  # force reinstall
SKIP_TOOLCHAIN_SETUP=1 ./build.sh   # disable the automatic download
```

Offline: download the matching package from
<https://github.com/bouffalolab/toolchain/releases/tag/default>, extract it into
`third_party/toolchain/`, and make sure
`third_party/toolchain/<dir>/bin/riscv64-unknown-elf-gcc` exists.

### 1.5 Release bundle

```bash
./release.sh
```

Produces `release/`:

```
release/ds4x360-bl616-xinput.bin
release/ds4x360-bl616-xinput-noaudio.bin
release/boot2_bl616_isp_release_v8.1.8.bin
release/partition.bin
release/flash_prog_cfg-xinput.ini
release/flash_prog_cfg-xinput-noaudio.ini
release/README.txt
```

### 1.6 Flashing

> Full guide: **[FLASHING.md](FLASHING.md)** — ISP entry, what the `.ini` does, Dev Cube vs
> BLFlashCommand, and the WSL2/usbipd serial path.

---

## 2. Windows

### 2.1 Option A: WSL2 (recommended)

WSL2 is plain Linux; follow section 1:

```powershell
wsl --install -d Ubuntu
```

Then install the packages, clone, and run `./build.sh` inside Ubuntu.

**Flashing:** see **[FLASHING.md](FLASHING.md)**. The validated transport is the board's own USB
port in ROM ISP (`349b:6160`), and that is the device to pass into WSL with `usbipd-win` (an
external USB-TTL adapter is an alternative):

```powershell
winget install usbipd
usbipd list
usbipd bind --busid <BUSID>
usbipd attach --wsl --busid <BUSID>
```

Once `/dev/ttyACM0` appears inside WSL you can flash with `BLFlashCommand`. Note that
`usbipd attach` binds the whole device: Windows will not see it until it is detached.

### 2.2 Option B: native Windows (known limitation)

**Native Windows does not work out of the box.** The vendored SDK is conservatively trimmed and
**no longer ships** `tools/make`, `tools/cmake` or `tools/ninja`. Worse, the SDK's `project.build`
hardcodes CMake as `$(BL_SDK_BASE)/tools/cmake/bin/cmake.exe` in its Windows branch, and
`build_windows.bat` puts those directories on `PATH`. Native Windows therefore needs:

- **GNU Make** and **CMake** installed system-wide (on `PATH`);
- `CMAKE=cmake` passed to make, to override the hardcoded path in `project.build`;
- or a full, untrimmed **Bouffalo SDK** (point `BL_SDK_BASE` at it).

`build_windows.bat` only sets `BL_SDK_BASE` to `third_party\bouffalo_sdk`; it does not download a
toolchain. Get the Windows Xuantie toolchain manually from
<https://github.com/bouffalolab/toolchain/releases/tag/default> and point `TOOLCHAIN_PATH` at the
directory containing `bin`.

Bottom line: **build under WSL2**; native Windows only if you can supply make/CMake yourself.

---

## 3. Troubleshooting

| Symptom | Fix |
|---|---|
| `ERROR: RISC-V toolchain not found` | Run `./setup_toolchain.sh`; check `third_party/toolchain/*/bin/riscv64-unknown-elf-gcc` |
| Toolchain download fails/times out | Retry, or install manually (section 1.4) |
| `cmake` too old | Upgrade to >= 3.15 (Ubuntu 20.04+ is fine) |
| `GNU Make >= 4.0 required` | macOS: `brew install make`, then use `gmake`; Ubuntu 20.04+ ships a suitable make |
| `USB_SPEED=hs is not supported` | Only `fs` is supported; drop the variable or use `USB_SPEED=fs` |
| Weird build after switching variant/board | `./build.sh clean` or `./build.sh rebuild` (the script also auto-cleans on a config-key change) |
| `BLFlashCommand` not found when flashing | Use Dev Cube, or copy it from a full SDK — see [FLASHING.md](FLASHING.md) |
| Want to reproduce with the official SDK | The SDK here is patched; you must use `third_party/bouffalo_sdk`, otherwise the P0 fixes are lost (see `SDK_CHANGES.md`) |

---

## 4. Repository layout (build-related)

```
ds4x360-bl616/
  src/                        application sources
  tools/                      host tool sources
  third_party/bouffalo_sdk/   vendored patched SDK (Apache-2.0)
  build.sh                    build entry point (delegates to build_macos.sh)
  build_macos.sh              Linux/macOS/WSL build implementation
  release.sh                  assemble the release/ bundle
  setup_toolchain.sh          download the official Xuantie toolchain
  build_windows.bat           native Windows build (manual toolchain; see 2.2)
  SDK_CHANGES.md              SDK patch notes
  misc/                       local logs/old artifacts (git-ignored, not in a clone)
```
