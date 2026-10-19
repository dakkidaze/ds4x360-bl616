# Phase 1 硬件验收记录（XUSB + UAC1 复合，1209:5D01）

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

- Oracle 上轮遗留的 mic FIFO 诊断：`ds4_usb_audio.c` 仍读 FIBC1（`0x1B4`），
  XINPUT 下 mic 在 FIFO3（FIBC3，`0x1BC`）。只影响 `MIC_DIAG` 显示，不影响
  推流；建议改成按端点索引推导。
- 自检工具对复合配置仍是源码模拟（运行时拼接），可后续做 C host harness 或
  真机抓取校验。
- DS4 persona 硬件回归（接口 0/1/2、mic 0x82）尚未重新烧录验证；描述符已证明
  字节一致。
- 正式分发前在 pid.codes 申请 `0x1209:5D01`。
