# Phase 1 硬件验收记录（XUSB + UAC1 复合，1209:5D01）

> **历史记录（身份已变更）**：第 1–6 节是在 `0x5D01` 上完成的。当前身份为
> `0x1209:0xD1CE`（见 `docs/identity-1209-d1ce.md`）；`0xD1CE` 的枚举/绑定/音频
> 复核见第 7 节。XUSB/音频描述符除 `idProduct` 之外没有改动。

日期：2026-10-19
提交：`8787b3a`（身份 `eefcd55` + MS OS 防御修复）
固件：`firmware/lctech616/ds4dongle-lctech616-xinput.bin`
SHA-256：`b93307d895224de5e9dfde9a878f92e9f6665fd029147a4ffe14de2a49aee491`

## 1. 刷写

- `usbipd attach --wsl --busid 6-15`（ISP `349b:6160`）→ `/dev/ttyACM0`
- `BLFlashCommand-ubuntu --config=flash_prog_cfg.ini --reset`
- host SHA == dev SHA，`Verification succeeded` / `All programming completed successfully`
- 芯片未随工具 reset 退出 ISP，断电重插（松开 BOOT）后启动

## 2. Windows 拓扑（主机侧实测）

| 设备 | Class | Service | ProblemCode | InstanceId |
|---|---|---|---|---|
| USB Composite Device | USB | **usbccgp** | 0 | `USB\VID_1209&PID_5D01\...` |
| Xbox 360 手柄 | XnaComposite | **xusb22** | 0 | `...&MI_00\...` |
| Xbox 360 Controller for Windows（音频功能） | MEDIA | **usbaudio** | 0 | `...&MI_01\...` |
| HID-compliant game controller | HIDClass | HidUsb | 0 | `...&IG_00\...` |

- 根设备由 `usbccgp` 接管，XUSB 子设备通过 MS OS `XUSB10` 兼容 ID 绑定
  `xusb22`；音频功能按类绑定 `usbaudio`。
- 无 Code 10/28。

## 3. XInput

```
pad[0] ret=0 packet=40388 buttons=0x0000 LT=0 RT=0 LX=0 LY=0 RX=0 RY=0
2s packet delta=252  rate=126 Hz
```

空闲全中立；手柄功能正常（按键/摇杆/扳机/连发/震动，用户实测）。

## 4. 用户音频实测

- 麦克风监听开启后，插耳机能从耳机听到麦克风声音（mic → USB capture → 主机监听 → USB playback → 耳机）。
- 拔掉耳机后扬声器正常出声，L/R 都在。
- 插耳机听音乐正常。
- 麦克风/扬声器均正常，未出现 `spk_fault` 误判。

## 5. 结论

Phase 1 通过：XUSB + UAC1 复合在 `1209:5D01` 身份下由 `usbccgp` 正常拆分，
`xusb22` 与 `usbaudio` 均绑定成功，XInput 与双向音频同时工作。

## 6. 后续（不阻塞）

- ~~Oracle 上轮遗留的 mic FIFO 诊断~~ **已修复**：`ds4_usb_audio.c` 现在按 mic
  端点推导 `FIBCn` 偏移（`DS4_USB_FIFO_BC_OFF(ep)`）：DS4 mic `0x82` → idx2 →
  `0x1B4`，XINPUT mic `0x84` → idx4 → `0x1BC`，并有编译期 `_Static_assert`
  校验两个 persona 的取值。该诊断只影响 `MIC_DIAG` 显示，不影响推流。
- 自检工具对复合配置仍是源码模拟（运行时拼接），可后续做 C host harness 或
  真机抓取校验。
- DS4 persona 在本仓库只是参考副本，不是出货目标；原 `ds4dongle-bl618` 与 SDK
  未改动。共享改动的证据是 DS4 描述符字节一致 + DS4 构建通过，不需要再做
  DS4 硬件回归。
- pid.codes 申请：`0x1209:0xD1CE`（DICE）已提交 PR
  https://github.com/pidcodes/pidcodes.github.com/pull/1324 。

## 7. D1CE 复核（2026-10-19）

- 固件：`release/ds4x360-bl616-xinput.bin`
  SHA-256 `287eb278432490541e20fe3e1b07b5dccd75889c116c2bf2b89e88fd59286ad7`
  （host SHA == dev SHA，刷写校验通过）
- 断电重插后枚举：

| 设备 | Class | Service | ProblemCode | 实例 |
|---|---|---|---|---|
| USB Composite Device | USB | **usbccgp** | 0 | `USB\VID_1209&PID_D1CE\...` |
| MI_00 | XnaComposite | **xusb22** | 0 | `...&MI_00` |
| MI_01 | MEDIA | **usbaudio** | 0 | `...&MI_01` |
| HID 子设备 | HIDClass | HidUsb | 0 | `...&IG_01` |

- 父设备硬件 ID：`USB\VID_1209&PID_D1CE&REV_0201`、`USB\VID_1209&PID_D1CE`
- `XInputGetState(0)` ret=0，空闲 `buttons=0x0000`，2 s 251 包（125.5 Hz）
- 音频端点出现 “(2- Xbox 360 Controller for Windows)” 的播放/录音设备
- 结论：`0xD1CE` 与 `0x5D01` 行为一致，`usbccgp` 拆分、`xusb22`/`usbaudio`
  绑定、XInput 与音频同时工作均正常。
