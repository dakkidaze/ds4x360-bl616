# Phase 1 状态

> **历史记录（身份已变更）**：本轮验收使用 `0x5D01`；当前身份为 `0x1209:0xD1CE`
> （见 `docs/identity-1209-d1ce.md`）。描述符除 `idProduct` 外未改，但 `0xD1CE`
> 需要重新做枚举/绑定验收，下文记录仅对应 `0x5D01`。

日期：2026-10-19
**更新：Phase 1 已通过硬件验收**（`1209:5D01`，XUSB + UAC1 复合，
`usbccgp`/`xusb22`/`usbaudio` 均绑定成功；详见 `docs/phase1-verification.md`）。
以下保留当时（身份切换前）的暂停记录。

暂停原因：等 Oracle 评审结果；本轮不修、不刷。

## 代码

- 提交：`90f60e0`（Phase 1 复合设备：XUSB + UAC1）
- 工作区干净
- 产物：`firmware/lctech616/ds4dongle-lctech616-xinput.bin`
  - sha256 `f6820366185abeb53e5660a207943177aec6256c742be5617b5febbe1bfa0660`
- 身份：保持 `045E:028E`（不做 MS OS 兼容 ID 链）

## 已验证

- host 映射/连发单测：673 项
- 描述符自检：复合 92 项；`--audio 0` 50 项
- 三套构建 exit 0：`XINPUT=1`（复合）、`XINPUT=1 AUDIO=0`、默认 DS4
- 改动文件无新增警告；DS4 镜像除构建时间戳外字节一致
- **未刷写 Phase 1**。设备当前运行 Phase 2 的 `d9764d35`

## 待办（下次）

1. **先解决 Oracle 的 BLOCK：Windows 复合设备驱动归属**（见下节），再谈刷写
2. 按严重度派 fixer 修复其余 findings
3. 重跑：host 单测、复合/`--audio 0` 自检、三套构建、DS4 回归
4. Oracle delta 复查
5. 刷写 `firmware/lctech616/ds4dongle-lctech616-xinput.bin`
6. 真机音频验收：播放、录音、暂停/恢复、`spk_fault` 不误判；XInput 功能回归

## Oracle `ora-6` 评审（2026-10-19）— **BLOCK**

固件侧描述符装配、接口路由、FIFO 分配被认为正确；阻塞点是 **Windows 驱动归属**。

### 1. High：音频没有被绑定的既定路径
- `xusb22.inf:49,74` 用根硬件 ID `USB\VID_045E&PID_028E` 匹配并安装 `xusb22`。
  没有 `MI_00` 子项，也没有按 FF/5D/01 接口匹配的条目。
- Windows 规则：device-ID INF 匹配优先于 `usbccgp`。因此不能假设根设备会
  自动拆成复合子设备，UAC1 接口可能拿不到音频驱动（设备仍 ProblemCode 0）。
- 需要先在一台未改动的 Windows 上抓**实际的 parent/child 驱动树**，确认音频
  设备是否真的存在。
- 如果不存在，显式路径是自备父 INF（`usbccgp` + `MI_00` 绑 stock XUSB），
  并做实验验证；IAD 或只改 bcdDevice 不能绕过根匹配。
- **待决策**：如果根匹配确实挡住 `usbccgp`，另一个更干净的方案是改用自有
  VID/PID + MS OS `XUSB10` 兼容 ID（就是之前暂缓的“身份切换”）。这样
  Windows 默认用 `usbccgp` 拆复合设备，XUSB 接口靠兼容 ID 绑 xusb22，音频
  接口按类绑 `usbaudio`。代价是身份变更 + 一轮绑定验证。

### 2. Medium：复合配置变更没有递增 bcdDevice
- 从 49B/1 接口变成 233B/4 接口，`bcdDevice` 仍是 `0x0200`，违反模块自己的
  描述符版本约定。
- 建议复合版 `0x0201`（XUSB-only 诊断版可保持 `0x0200`），并在硬件记录里
  写清楚；不要把它当成 finding 1 的修复。

### 3. Low：mic FIFO 诊断仍读旧 FIFO
- `ds4_usb_audio.c:879-886,921` 的 FIBC 读仍硬编码 `0x1B4`（FIBC1）。
  XINPUT 下 FIBC1 是 XUSB IN/F1，mic 在 FIFO3（`0x1BC`）。
- 只影响诊断/`MIC_DIAG` 显示，不影响推流与恢复门控。
- 修法：`0x1B0 + 4 * ((DS4_AUDIO_EP_IN & 0x7F) - 1)`，两处共用。

### 4. Low：自检工具验证的是“原料”，不是实际装配结果
- 复合配置由 Python 自己拼，然后断言自己写入的 233/4；没有执行
  `usb_gamepad_init()`/`desc_config_cb()`/注册顺序。
- 端点收集丢掉了 interface/alt 归属，重复地址会被静默覆盖。
- 建议用 stub 掉 USB 注册的 C host harness 验证实际回调输出，或在真机上抓
  原始配置；保留端点归属、检查每个 alt 的端点数量与重复；删掉
  `tools/xinput_desc_check.py` 里恒真的 `or True` 断言。

### Oracle 认为正确的部分
- 8 处音频替换正确，AC `baInterfaceNr=[2,3]`、AC 长度 71、AS alt/端点一致；
  连续 UAC1 集合不需要 IAD。
- 注册顺序 0/1/2/3 与 CherryUSB 的 `wIndex` 路由一致；XUSB interface 0 的
  class 拒绝不会拦截音频请求。
- 端点/FIFO：dummy EP3 跳过，F0/F1/F2/F3 = speaker/XUSB IN/XUSB OUT/mic，
  无冲突。
- 装配在 `usbd_desc_register`/初始化之前完成；233/3 的静态头不会被主机看到。
- DS4 persona 无行为回归；`0xA0/250`（500mA）有效。

## 身份路径（`xusb22.inf` 实测，2026-10-19）

`/mnt/c/Windows/INF/xusb22.inf` 的 Models 段只有：

```
[MSFT.NTamd64]
%XUSB22.DeviceName.Wired%=CC_Install, USB\Vid_045E&Pid_028E
%XUSB22.DeviceName%=CC_Install,      USB\Vid_045E&Pid_0719
%XUSB22.DeviceName.Wired%=CC_Install, USB\MS_COMP_XUSB10
%XUSB22.DeviceName%=CC_Install,      USB\MS_COMP_XUSB20
%XUSB22.DeviceName.Jump%=CC_Install,  USB\Vid_045E&Pid_028F
```

没有 `MI_00` 条目，也没有 `usbccgp`。

- **045E:028E + usbccgp 不会自动生效**：根硬件 ID 先匹配 `xusb22`。要改用
  `usbccgp` 必须宿主侧自备父 INF 或手动改驱动；而 MI_00 子设备还需要
  `USB\MS_COMP_XUSB10`（MS OS 描述符）或自备子 INF 才能绑 `xusb22`。
  这不是即插即用。
- **自定义 VID/PID + MS OS `XUSB10`**：没有根 INF 匹配，Windows 默认用
  `usbccgp` 拆复合；XUSB 子设备靠 `USB\MS_COMP_XUSB10` 绑 `xusb22`，音频
  接口按类绑 `usbaudio`。这是第三方 XInput 复合设备的标准即插即用路径。
- **VID 不要随机生成**：VID 是全局分配资源。用 pid.codes 的 `0x1209` +
  申请/分配的 PID（开源免费），或自有 VID。在 `0x1209` 下随手挑一个未分配
  PID 本地能跑，但不规范；正式使用应申请。
- 切换身份后，**XUSB-only 变体也必须带 `XUSB10`**（否则 `xusb22` 不绑）；
  DS4 persona（054C:09CC）不受影响。


### 首次刷写前的前三项硬件检查
1. 枚举与驱动归属：抓原始 233B 配置 + 完整 parent/child service 树，确认
   render/capture 设备真的存在，而不只是 xusb22 ProblemCode 0。
2. 并发双工：32k 立体声播放 + 16k mono 录音，同时跑 XInput/连发/震动；
   验证 CUR/mute/volume 路由、SET_INTERFACE 2/3、0x01/0x84 的真实 ISO 流量。
3. 生命周期隔离：speaker/mic 独立停开、两种顺序、suspend/resume、断连重连；
   XUSB IN/OUT 不挂、音频恢复、无持续 mic fault/误判 F0/整设备恢复抖动。


## 关键实现点（复查时用）

- 接口：0 XUSB、1 AC、2 AS OUT、3 AS IN
- 端点：0x82/idx2/F1、0x03/idx3/F2、0x01/idx1/F0、0x84/idx4/F3
- 复合配置 233B：header 9 + XUSB block 40 + audio 184（offset 49），
  `wTotalLength=233`、`bNumInterfaces=4`
- 音频描述符只在 8 个偏移按 persona 改写（界面号、AC `baInterfaceNr`、mic EP）
- `dummy_ep3` 在 XINPUT 下跳过（idx3/F2 归 XUSB OUT）
- F0 守卫仍守音频 OUT（FIFO0）
