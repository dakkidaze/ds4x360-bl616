# Phase 1 状态（暂停点）

日期：2026-10-19
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

1. 收 Oracle `ora-6`（`ses_eb04a2decffe2BDuxsm8e6uTjf`）的 findings
2. 按严重度派 fixer 修复；不改身份方案
3. 重跑：host 单测、复合/`--audio 0` 自检、三套构建、DS4 回归
4. Oracle delta 复查
5. 刷写 `firmware/lctech616/ds4dongle-lctech616-xinput.bin`
6. 真机音频验收：播放、录音、暂停/恢复、`spk_fault` 不误判；XInput 功能回归

## 关键实现点（复查时用）

- 接口：0 XUSB、1 AC、2 AS OUT、3 AS IN
- 端点：0x82/idx2/F1、0x03/idx3/F2、0x01/idx1/F0、0x84/idx4/F3
- 复合配置 233B：header 9 + XUSB block 40 + audio 184（offset 49），
  `wTotalLength=233`、`bNumInterfaces=4`
- 音频描述符只在 8 个偏移按 persona 改写（界面号、AC `baInterfaceNr`、mic EP）
- `dummy_ep3` 在 XINPUT 下跳过（idx3/F2 归 XUSB OUT）
- F0 守卫仍守音频 OUT（FIFO0）
