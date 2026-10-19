# Flashing Guide

This guide covers both the **release bundle** and **locally built firmware**, explains what the
`.ini` files do, how to enter ISP mode, and which flashing paths exist on Linux / WSL2 / Windows.

> The dongle is a BL616 board. Flashing goes through the chip's **ROM ISP over USB**, which is a
> *different* USB device from the dongle's normal XUSB composite. On the LCTech BL616 the validated
> path is the board's own USB port: in ROM ISP it enumerates as USB CDC **`349b:6160`**
> (`COMx` on Windows, `/dev/ttyACM0` on Linux/WSL2). An external USB-TTL adapter on the UART header
> is an *alternative* transport, not a requirement.

## 1. What is in the release bundle?

`./release.sh` produces:

| File | Role |
|---|---|
| `ds4x360-bl616-xinput.bin` | Application firmware for the XInput variant (XUSB pad + UAC1 audio). |
| `ds4x360-bl616-xinput-noaudio.bin` | Application firmware for the XUSB-only variant. |
| `boot2_bl616_isp_release_v8.1.8.bin` | Second-stage bootloader, always written at `0x000000`. |
| `partition.bin` | Partition table, always written at `0xE000`; defines where the app partition lives. |
| `flash_prog_cfg-xinput.ini` | Flash layout manifest for the `xinput` variant. |
| `flash_prog_cfg-xinput-noaudio.ini` | Flash layout manifest for `xinput-noaudio`. |
| `LICENSE`, `NOTICE`, `LICENSE-bouffalo_sdk` | Licensing materials (GPL-3.0 app, third-party notices, Apache-2.0 SDK). |
| `SOURCE.txt` | GPL-3.0 Corresponding Source pointer (repository URL + commit). |
| `SHA256SUMS` | Checksums for every other file in the bundle. |
| `README.txt` | Short in-bundle notes and the validated flashing command. |

Verify a download before flashing:

```bash
cd release
sha256sum -c SHA256SUMS
# macOS:
shasum -a 256 -c SHA256SUMS
```

This 4 MB BL616 layout puts the `FW` partition at `0x10000`. The ini says `address = @partition`,
so the flashing tool resolves it by reading `partition.bin`.

The ini also carries:

- `erase = 1` — erase the programmed sections before writing (not a full-chip erase);
- `skip_mode = 0x0, 0x0` — skip nothing;
- `boot2_isp_mode = 0` — normal boot2, not the ISP boot2.

Each variant has its own ini because the application firmware is renamed in the bundle.
**If you rename a bin, update the matching `filedir = ./...` line.**

## 2. Do I need the ini?

| Method | Uses the ini? | Notes |
|---|---|---|
| `BLFlashCommand ... --config=...` | **Yes — validated path** | Gives boot2/partition/app addresses in one shot. |
| `./build.sh flash /dev/ttyACM0` (Linux) | **Yes, handled for you** | The script calls the SDK's `make flash`, which uses the repository-root `flash_prog_cfg.ini` (pointing at `build/build_out/`). |
| Bouffalo Dev Cube (GUI) | **No — it does not import this ini** | Enter the addresses manually (section 4). |
| Raw `--firmware` + `--addr` | No | Possible, but you must also flash boot2 and the partition table yourself. Not recommended. |

## 3. Preparation: enter ROM ISP

1. Unplug the board.
2. Hold **BOOT**.
3. Plug the **board's own USB port** into the PC (this is the same Type-C port used normally; in
   ROM ISP the chip re-enumerates as `349b:6160`).
4. Release **BOOT** once the port appears.

Find the port:

```bash
# Linux / WSL2
lsusb | grep -i 349b          # 349b:6160 = BL616 ROM ISP
ls /dev/ttyACM* /dev/ttyUSB*
# macOS
ls /dev/tty.usbserial* /dev/tty.usbmodem*
```

An external USB-TTL adapter (3.3V) on the UART header is an alternative if you cannot use the
board's USB port; the ISP entry sequence is the same, and the serial device will be a
`/dev/ttyUSB*` (or `COMx`) instead.

Linux permissions:

```bash
sudo usermod -aG dialout "$USER"   # re-login required
# or temporarily:
sudo chmod 666 /dev/ttyACM0
```

### BLFlashCommand is not vendored

The trimmed SDK drops `tools/bflb_tools/bouffalo_flash_cube/`, so `BLFlashCommand` is **not in this
repository**. Copy it from a full Bouffalo SDK:

```bash
mkdir -p third_party/bouffalo_sdk/tools/bflb_tools/bouffalo_flash_cube
cp -r <full-sdk>/tools/bflb_tools/bouffalo_flash_cube/* \
      third_party/bouffalo_sdk/tools/bflb_tools/bouffalo_flash_cube/
```

It contains `BLFlashCommand-ubuntu`, `BLFlashCommand-macos`, `BLFlashCommand.exe`, etc.

`./build.sh flash` checks for `BLFlashCommand-ubuntu` first and exits 1 with a
“use Dev Cube or copy BLFlashCommand from a full SDK” hint when it is missing — it does not clean,
reconfigure, or download a toolchain.

## 4. Method A — BLFlashCommand (validated path)

This is the flow that was actually used for hardware acceptance
(`docs/phase1-verification.md`).

### 4.1 Release bundle

```bash
cd release
/path/to/BLFlashCommand-ubuntu \
  --interface=uart --baudrate=2000000 \
  --port=/dev/ttyACM0 --chipname=bl616 \
  --config=flash_prog_cfg-xinput.ini --reset
```

Swap `xinput` for `xinput-noaudio` as needed. On macOS use `BLFlashCommand-macos`; on Windows use
`BLFlashCommand.exe` with `--port=COMx`. Run it from the bundle directory so the ini's relative
paths resolve.

`--reset` asks the tool to reset the chip after programming. **It may leave the chip in ISP mode.**
If the device does not enumerate as `1209:d1ce` afterwards, unplug the board and plug it back in
**without** holding BOOT.

### 4.2 Local build

```bash
./build.sh
./build.sh flash /dev/ttyACM0
```

`build.sh flash` drives the SDK's `make flash` from the repository root with
`flash_prog_cfg.ini` (whose `filedir` entries point at `build/build_out/`). See section 3 when the
tool is missing.

## 5. Method B — Bouffalo Dev Cube (alternative)

Dev Cube is a GUI and **does not import BLFlashCommand's `.ini`**. Configure the layout by hand:

1. Put the board into ISP mode as in section 3.
2. Open **Bouffalo Dev Cube**, select chip **BL616**, interface **UART**, and the `349b:6160`
   port (`COMx`).
3. Choose the files and addresses manually:

   | Region | File | Address |
   |---|---|---|
   | boot2 | `boot2_bl616_isp_release_v8.1.8.bin` | `0x000000` |
   | partition | `partition.bin` | `0xE000` |
   | application | `ds4x360-bl616-<variant>.bin` | `0x10000` |

4. Start the download, then power-cycle the board if it stays in ISP mode.

Dev Cube is a supported alternative, but BLFlashCommand is the path this project validates.

## 6. Method C — WSL2 (the usual path for this repository)

When developing on Windows and building in WSL2, forward the ISP device with `usbipd-win`
(this is how the hardware acceptance was performed: `usbipd attach --wsl --busid 6-15`):

```powershell
# Windows PowerShell (administrator)
winget install usbipd
usbipd list                       # 349b:6160 -> note the BUSID
usbipd bind --busid <BUSID>
usbipd attach --wsl --busid <BUSID>
```

Confirm the device node inside WSL:

```bash
ls /dev/ttyACM0
```

Then flash as in 4.1/4.2:

```bash
cd release
/path/to/BLFlashCommand-ubuntu \
  --interface=uart --baudrate=2000000 \
  --port=/dev/ttyACM0 --chipname=bl616 \
  --config=flash_prog_cfg-xinput.ini --reset
```

Note: `usbipd attach` binds the whole device, so Windows (including Dev Cube) cannot see it until
you `usbipd detach --busid <BUSID>`.

## 7. Method D — native Windows

Prefer WSL2 (section 6). Native Windows flashing works with `BLFlashCommand.exe` (or Dev Cube),
but building natively is not supported out of the box — see the known limitation in
[BUILD.md](BUILD.md) section 2.2.

The vendored SDK does not ship the flashing tools, so `build_windows.bat flash COMx` has no
`BLFlashCommand.exe` to call. Use Dev Cube, or copy a full SDK's `bouffalo_flash_cube` into
`third_party\bouffalo_sdk\tools\bflb_tools\` and run it manually.

## 8. Which variant should I flash?

| Variant | When |
|---|---|
| `xinput` | Default. Xbox 360 wired pad (XUSB `1209:D1CE`) plus the UAC1 audio function. |
| `xinput-noaudio` | XUSB pad only; use it to rule out audio problems. |

The DS4-persona build (`XINPUT=0 ./build.sh`) is reference/regression only and is not published by
`release.sh`.

## 9. Troubleshooting

| Symptom | Check |
|---|---|
| No `349b:6160` / serial port | Board not in ISP mode, USB cable is charge-only, or the port is bound to Windows (detach usbipd). |
| `Permission denied` on Linux | Join `dialout`, or `sudo chmod 666 /dev/ttyACM0`. |
| `BLFlashCommand` not found | Copy it from a full SDK (section 3), or use Dev Cube; `./build.sh flash` fails early with that hint. |
| No `/dev/ttyACM0` in WSL | Verify bind/attach with `usbipd list`; replug the board after attaching. |
| Flash succeeds but the host does not see a pad | The tool's `--reset` may have left the chip in ISP; unplug/replug without BOOT. `lsusb` should then show `1209:d1ce`. |
| No audio device | You flashed `xinput-noaudio`; use the `xinput` variant. |
| `SHA256SUMS` mismatch | Re-download the bundle; do not flash a corrupt image. |

After flashing the host should enumerate `1209:d1ce` (XUSB). On Linux:

```bash
lsusb | grep -i 1209
dmesg | tail -n 40
```

## 10. Related documents

- Build options: [BUILD.md](BUILD.md) / [BUILD_CN.md](BUILD_CN.md)
- Variants and controller notes: [README.md](README.md) / [README_CN.md](README_CN.md)
- Hardware acceptance record: `docs/phase1-verification.md`
