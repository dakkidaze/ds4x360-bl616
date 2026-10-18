# Phase 2 硬件验收记录（DS4 → XUSB 映射 + 连发 + 震动）

日期：2026-10-18
提交：`e938d38`（Phase 2 + 两轮 Oracle 修复）
固件：`firmware/lctech616/ds4dongle-lctech616-noaudio-xinput.bin`
SHA-256：`56e8212e1547034056c28b20828e1bfa6923edf4546245eece63df587170ff8f`

## 1. 刷写

- `usbipd attach --wsl --busid 5-15`（ISP `349b:6160`）→ `/dev/ttyACM0`
- `BLFlashCommand-ubuntu --config=flash_prog_cfg.ini --reset`
- host SHA == dev SHA，`Verification succeeded` / `All programming completed successfully`
- 芯片未随工具 reset 退出 ISP，断电重插（松开 BOOT）后启动新固件

## 2. 枚举与绑定

| 项 | 值 |
|---|---|
| VID:PID | `045E:028E` |
| BusReportedDeviceDesc | `Xbox 360 Controller for Windows` |
| Class | `XnaComposite` |
| ProblemCode | 0（无 Code 10/28） |
| Service | `xusb22`，10.0.26100.9278 |
| HardwareIds | `USB\VID_045E&PID_028E&REV_0200`、无 REV 条目 |

## 3. XInput 实测

```
pad[0] ret=0 packet=13128 buttons=0x0000 LT=0 RT=0 LX=0 LY=0 RX=0 RY=0
pad[1..3] ret=1167 (ERROR_DEVICE_NOT_CONNECTED)
2s packet delta=252  rate=126 Hz
```

- 无按键、无摇杆输入时是**全中立**帧：Phase 0 的“A 常按”已移除，断连/空闲由 mapper
  发中立报告。
- 主机仍然按 ~8 ms 消费（126 Hz），与 Phase 0 观察一致，不影响功能。

## 4. 用户冒烟测试（真机）

- 按键：正常（映射到 X360 按键）。
- 震动：正常。

震动可用同时证明了 Phase 0 遗留的 OUT 链路：xusb22 → XUSB OUT 8B →
`xinput_ep_out_handler` → DS4 0x05 胶水 → BT 输出 → 手柄实际震动。

## 5. 尚未覆盖

- 触摸板连发：按住触摸板 + 长按 A/B/X/Y/LB/RB ≥400 ms 的 15 Hz 脉冲；短按不吞。
- 断连/重连：保持按键或连发状态时断开再连，确认先中立、无残留、连发需重新计时。
- 切换手柄：旧手柄的输入/震动/LED 不串到新手柄。
- `SET_INTERFACE(0,0)` / USB reset / suspend-resume 后的 IN/OUT 恢复（普通使用不触发）。
- DS4 persona 回归（音频属于 Phase 1，本轮未测）。

## 6. 评审

Oracle 三轮：初评 3 条 major → 第一轮 delta 3 条（1 major、1 major、1 minor）→
第二轮 delta **APPROVE**。最终代码即 `e938d38`。
