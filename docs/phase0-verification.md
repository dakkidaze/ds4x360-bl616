# Phase 0 硬件验收记录（XUSB 可枚举）

日期：2026-10-18
固件：`firmware/lctech616/ds4dongle-lctech616-noaudio-xinput.bin`
SHA-256：`e42d0030d8db617d93a497d9b4dfe958f6aa918a706f209128cf030bc454eb2a`

## 1. 刷写

- `usbipd attach --wsl --busid 5-15`（`349b:6160 Bouffalo CDC DEMO`）→ WSL `/dev/ttyACM0`
- `BLFlashCommand-ubuntu --interface=uart --baudrate=2000000 --port=/dev/ttyACM0
  --chipname=bl616 --config=flash_prog_cfg.ini`
  - `boot2` → 0x000000
  - `partition.bin` → 0xE000
  - 固件 → 0x10000（@partition）
  - host SHA == device SHA，`Verification succeeded` / `Flash writing succeeded`
- 工具 `--reset` 没有让芯片退出 ISP（USB 无 disconnect），断电重插并松开 BOOT 后启动。

## 2. Windows 枚举

| 项 | 值 |
|---|---|
| InstanceId | `USB\VID_045E&PID_028E\8&3F75BC3&0&15` |
| BusReportedDeviceDesc | `Xbox 360 Controller for Windows` |
| Class | `XnaComposite` |
| Status / Problem | `OK` / `CM_PROB_NONE`（0，无 Code 10/28） |
| Service | `xusb22` |
| Driver | `10.0.26100.9278`，Microsoft |
| HardwareIds | `USB\VID_045E&PID_028E&REV_0200`、`USB\VID_045E&PID_028E` |
| CompatibleIds | `USB\COMPAT_VID_045E&Class_FF&SubClass_5D&Prot_01` 等（系统按类生成） |
| HID 子设备 | `HID-compliant game controller`（joy.cpl 可见） |

REV_0200 说明总线上确实读到了 `bcdDevice = 0x0200`；无 REV 的硬件 ID 也匹配
`xusb22.inf`，绑定不依赖兼容 ID 链。

## 3. XInput 实测（`XInputGetState` / `XInputSetState`）

```
pad[0] ret=0 packet=7969 buttons=0x1000 LT=0 RT=0 LX=0 LY=0 RX=0 RY=0
pad[1..3] ret=1167 (ERROR_DEVICE_NOT_CONNECTED)
2s packet delta=251  rate=125.5 Hz  (implied period 8 ms)
rumble ret=0 stop ret=0
```

- `buttons=0x1000` = A 常按，摇杆 0、扳机 0，与 Phase 0 固定报告一致。
- 报告在持续流动（packet 递增），不是一次性状态。
- 实测 125.5 Hz（8 ms），而端点 `bInterval=4`（理论 250 Hz）。Phase 0 不影响判定；
  Phase 2 若在意输入延迟，需要查是 xusb22 侧的轮询节奏还是固件“完成→重新 arm”
  错过了一次 4 ms 轮询（NAK）。

## 4. 结论

Phase 0 通过：

- XUSB 描述符被 Windows 接受，xusb22 绑定成功，无 Code 10/28；
- 20B 固定输入报告被 XInput 正确解析（A 键、摇杆、扳机）；
- OUT 端点已 arm，`XInputSetState` 被驱动接受（返回 0）。

**未直接证明**：8B rumble / 3B LED 是否真的到达固件 OUT 回调。release 构建没有
可从主机侧读取的计数器；Phase 2 把 rumble 转发给 DS4 后可用物理震动验证，或临时
加一个诊断输出。

## 5. 解锁

Phase 1（音频回归，接口 1/2/3、端点 0x01/0x84、F0 守卫保持 FIFO0）可以开始；
Phase 2（映射 + 触摸板连发）仍排在 Phase 1 之后。
