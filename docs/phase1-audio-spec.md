# Phase 1 规格：XINPUT + UAC1 音频复合设备

状态：**已完成（历史规格，Phase 1 已通过硬件验收）**。XUSB + UAC1 复合在
`1209:5D01` 身份下通过验收（`usbccgp` → `xusb22` + `usbaudio`，见
`docs/phase1-verification.md`）。下文保留当时的规格文本；身份一度使用
`045E:028E`（微软 VID/PID，仅本地/实验，且不做 MS OS 兼容 ID 链），之后切换为
`1209:5D01` 并加入 MS OS 兼容 ID 链。

## 1. 目标与布局

同一配置里放两个功能：

| 接口 | 内容 | 端点 |
|---|---|---|
| 0 | XUSB（FF/5D/01） | IN 0x82（idx2/F1）、OUT 0x03（idx3/F2） |
| 1 | Audio Control | 无 |
| 2 | AS OUT（speaker） | ISO OUT 0x01（idx1/F0） |
| 3 | AS IN（mic） | ISO IN **0x84**（idx4/F3） |

- 4 个数据端点 / 4 个 FIFO 正好用满，索引 1/2/3/4 互不共享。
- XUSB 仍在 interface 0，`0xC1/0x01` 握手继续用 `wIndex=0`。
- 音频 OUT 仍在 FIFO0，`g_usb_f0_guard_ep = 0x01 & 0x7F` 不变。
- 音频拓扑、采样率、MPS、alt 设置不变（32k 立体声 / 16k mic）。

## 2. UAC1 描述符参数化

`src/ds4_usb_audio_desc.h`：按 persona 定义接口号与 mic 端点：

```c
#ifdef DS4_FEATURE_XINPUT
#define DS4_AUDIO_INTF_CTRL    1
#define DS4_AUDIO_INTF_STREAM  2
#define DS4_AUDIO_INTF_MIC     3
#define DS4_AUDIO_EP_IN        0x84
#else
#define DS4_AUDIO_INTF_CTRL    0
#define DS4_AUDIO_INTF_STREAM  1
#define DS4_AUDIO_INTF_MIC     2
#define DS4_AUDIO_EP_IN        0x82
#endif
```

184 字节数组里只替换这些字面量（其余字节保持与真机 DS4 一致）：

| 偏移 | 原值 | 宏 | 说明 |
|---|---|---|---|
| 2 | 0x00 | `DS4_AUDIO_INTF_CTRL` | AC 接口号 |
| 17 | 0x01 | `DS4_AUDIO_INTF_STREAM` | AC header `baInterfaceNr[0]` |
| 18 | 0x02 | `DS4_AUDIO_INTF_MIC` | AC header `baInterfaceNr[1]` |
| 82 | 0x01 | `DS4_AUDIO_INTF_STREAM` | AS OUT alt 0 接口号 |
| 91 | 0x01 | `DS4_AUDIO_INTF_STREAM` | AS OUT alt 1 接口号 |
| 134 | 0x02 | `DS4_AUDIO_INTF_MIC` | AS IN alt 0 接口号 |
| 143 | 0x02 | `DS4_AUDIO_INTF_MIC` | AS IN alt 1 接口号 |
| 170 | 0x82 | `DS4_AUDIO_EP_IN` | mic ISO IN 端点 |

`DS4_AUDIO_EP_OUT` 保持 0x01；`DS4_AUDIO_OUT_MPS`/`IN_MPS` 不变。

## 3. `ds4_usb_audio.c`

- `usb_audio_register()` 里 `usbd_ep_open(busid, &dummy_ep3)` 是 DS4 persona 的
  诊断补丁（给未使用的 idx3/F2 配 FIFO）。XINPUT 下 idx3 是 XUSB OUT，
  **必须跳过**：`#ifndef DS4_FEATURE_XINPUT` 包住。
- 其余逻辑不动：接口注册顺序仍是 control → stream → mic，core 按注册顺序赋
  `intf_num`（XUSB 先注册，所以是 1/2/3）。
- `DS4_AUDIO_INTF_STREAM/MIC` 在 `usb_audio_set_interface()` 等处的比较会自动
  跟着宏走。

## 4. 复合配置描述符

`src/usb_xinput.c/.h`：

- 新增 `const uint8_t *usb_xinput_interface_block(uint16_t *len)`，返回
  `xinput_config_desc + 9`、长度 40（Interface 9 + 0x21 17 + EP IN 7 + EP OUT 7）。
  不复制字节，避免两份真源。
- `usb_xinput_config_desc()` 保持返回 49 字节（XUSB-only）。

`src/usb_gamepad.c`：

- `config_desc` 不再只在非 XINPUT 下存在；XINPUT 也要用。
- XINPUT 的尺寸/偏移：

```c
#define USB_XINPUT_BLOCK_LEN 40
#define USB_CONFIG_SIZE (9 + USB_XINPUT_BLOCK_LEN + DS4_AUDIO_DESC_LEN)
#define AUDIO_DESC_OFF  (9 + USB_XINPUT_BLOCK_LEN)   /* 49 */
```

- `usb_gamepad_init()` 的 XINPUT 分支：
  1. `memcpy(config_desc, usb_xinput_config_desc(), 9)`（配置头）
  2. `memcpy(config_desc + 9, usb_xinput_interface_block(&len), 40)`
  3. 若 `DS4_FEATURE_AUDIO`：`memcpy(config_desc + 49, usb_audio_get_desc(&alen), 184)`，
     然后 `wTotalLength = 233`、`bNumInterfaces = 4`
  4. 若非 AUDIO：头里的 `49 / 1` 保持不动
- `desc_config_cb()` 在 XINPUT 下返回 `config_desc`。
- 注册顺序：`usb_xinput_register(0)` →（有音频时）`usb_audio_register(0)`。
  XUSB 必须最先注册，保证 interface 0。
- DS4 persona 的布局（audio 0/1/2 + gamepad 3）完全不变。

## 5. 构建开关

- `CMakeLists.txt`：删除 “XINPUT=1 必须 AUDIO=0” 的 FATAL_ERROR。
  `XINPUT=1` 默认带音频；`XINPUT=1 AUDIO=0` 仍是 XUSB-only 诊断变体。
  `DS4_FEATURE_XINPUT` + `DS4_NO_GAMEPAD` 不变。
- `build_macos.sh`：删掉同样的 AUDIO=0 强制检查；`-xinput` 标签保留，
  `AUDIO=0` 时再加 `-noaudio`。
- 构建命令：
  - 复合：`BOARD_TYPE=lctech616 USB_SPEED=fs XINPUT=1 bash build_macos.sh`
  - XUSB-only：追加 `AUDIO=0`

## 6. 描述符自检工具

`tools/xinput_desc_check.py` 扩展为同时接受两种配置长度：

- 49 字节：XUSB-only，现有断言不变。
- 233 字节（`0xE9`）：XUSB + audio：
  - `bNumInterfaces == 4`，链完整
  - interface 0 = FF/5D/01，2 个端点（0x82/0x03），17B 0x21 不变
  - interface 1 = Audio Control（class 0x01/0x01/0x00），AC header
    `baInterfaceNr == [2, 3]`
  - interface 2 = AS OUT，alt 0/1，EP 0x01，MPS 132，bInterval 1
  - interface 3 = AS IN，alt 0/1，EP 0x84，MPS 34，bInterval 1
  - 端点索引 2/3/1/4 互不重复
  - `.bin` 中能搜到完整 233 字节配置

## 7. 验收

- host 描述符自检：AUDIO=0 与 AUDIO=1 两种配置都 PASS。
- 三套构建：XINPUT+AUDIO、XINPUT+AUDIO=0、默认 DS4，均 exit 0、无新增警告。
- Windows 真机：
  - 设备管理器：Xbox 360 手柄（xusb22）+ USB 音频设备，无 Code 10/28
  - XInput 按键/摇杆/扳机/连发/震动正常
  - 音频：播放、录音、暂停/恢复；不出现 `spk_fault` 误判（F0 守卫仍守 FIFO0）
- DS4 persona 回归：音频接口 0/1/2、mic 0x82，行为不变。

## 8. Lane 归属

- **Lane A**：`src/ds4_usb_audio_desc.h`、`src/ds4_usb_audio.c`（dummy 跳过）。
- **Lane B**：`src/usb_xinput.c/.h`（block accessor）、`src/usb_gamepad.c`
  （复合装配 + 注册顺序）、`CMakeLists.txt`、`build_macos.sh`。
- **Lane C**：`tools/xinput_desc_check.py` 扩展（A/B 之后）。
- Lane A 先行，B 依赖 A 的宏；C 在 B 之后。
