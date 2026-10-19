# 身份切换：VID 0x1209 / PID 0xD1CE + MS OS XUSB10

日期：2026-10-19
依据：`docs/phase1-status.md` 的驱动归属结论、`docs/pid-codes-1209.md` 的
未分配 PID 清单。

## 决策

- **VID**：`0x1209`（pid.codes 共享 VID）
- **PID**：`0xD1CE`（hexspeak "DICE"；pid.codes `0x1209:0xD1CE`）
- **bcdDevice**：`0x0201`（复合配置变更，从 0x0200 递增）
- 正式分发前需在 pid.codes 申请 `0x1209:0xD1CE`；在此之前按本地/实验用途使用。
- DS4 persona（`054C:09CC`）不变。
- 更早选择过 `0x5D01`（XUSB 子类 0x5D + 协议 0x01 的联想），已被 `0xD1CE` 取代。

## 为什么需要

`xusb22.inf` 用根硬件 ID `USB\VID_045E&PID_028E` 匹配，优先于 `usbccgp`；
UAC1 复合功能拿不到音频驱动。换成无根 INF 匹配的 VID/PID 后：

1. Windows 默认用 `usbccgp` 拆复合设备；
2. XUSB 接口（MI_00）通过 MS OS 1.0 兼容 ID `XUSB10` 绑 `xusb22`；
3. 音频接口（MI_01/02/03）按类绑 `usbaudio`。

## MS OS 1.0 描述符

- vendor code：`0x20`（**不能是 0x01**，与 XUSB 握手冲突）
- String 0xEE（18 字节）：
  ```
  0x12, 0x03,
  'M',0, 'S',0, 'F',0, 'T',0, '1',0, '0',0,
  0x20, 0x00
  ```
- compatible-ID blob（40 字节，bFirstInterfaceNumber = 0）：
  ```
  28 00 00 00        // dwLength = 40
  00 01              // bcdVersion = 0x0100
  04 00              // wIndex = 0x0004
  01                 // bCount = 1
  00 00 00 00 00 00 00
  00                 // bFirstInterfaceNumber = 0 (XUSB)
  01                 // reserved
  'X','U','S','B','1','0',0,0
  00 00 00 00 00 00 00 00
  00 00 00 00 00 00  // reserved2
  ```
- `struct usb_msosv1_descriptor`：`string` / `vendor_code` / `compat_id` /
  `comp_id_property = NULL`，挂到 `usb_desc.msosv1_descriptor`。
- CONFIG_USBDEV_ADVANCE_DESC 下 core 在 string index 0xEE 返回该 string，
  并在 `bRequest == vendor_code && wIndex == 4` 时返回 compat_id；这两条路径
  先于 class vendor handler，不会和 `0xC1/0x01` 冲突。

## 改动范围

- `src/usb_xinput.h`：VID/PID/BCD 宏；MS OS vendor code。
- `src/usb_xinput.c`：msosv1 string/compat blob/descriptor；挂到 `usb_desc`。
- `tools/xinput_desc_check.py`：期望的 VID/PID 改为 `1209:D1CE`、bcdDevice
  `0x0201`；增加 msosv1 string 长度/vendor code/compat ID 的基本校验
  （至少：40 字节、`XUSB10`、bFirstInterfaceNumber=0）。
- 其他 Phase 1 复合逻辑不动。

## 验收

1. 描述符自检：复合 92+ 项、XUSB-only 50+ 项，新 VID/PID/BCD 断言通过。
2. 三套构建 exit 0（复合、XUSB-only、DS4），无新增警告。
3. 真机（下次）：Windows 上应出现
   - 父设备：USB Composite Device（usbccgp）
   - 子设备：Xbox 360 Controller for Windows（xusb22，靠 `USB\MS_COMP_XUSB10`）
   - 子设备：USB Audio Device（usbaudio）
   - 硬件 ID：`USB\VID_1209&PID_D1CE&REV_0201`
4. 若 Windows 仍不拆复合或 XUSB 子设备不绑，回退检查 msosv1 描述符与
   `wIndex=4` 请求（UsbTreeView / USB 抓包）。
