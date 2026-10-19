# SDK 补丁说明（bouffalo_sdk，基线 42c20811 → 本地 ec8c6e9d）

> **本仓库（`ds4x360-bl616`）说明**：本仓库复用**同一份**已打补丁的 SDK
> （base `42c20811` → local `ec8c6e9d`，确切状态见
> `third_party/bouffalo_sdk/VENDORED_SDK_COMMIT`），补丁内容与本文档描述的完全一致。
> 下文提到的 DS4 专用 **app 侧**改动（DS4 HID 描述符、USB 音频协议实现等）**不适用于本仓库**：
> 本仓库的 app 侧代码在 `src/`，只有共享 USB/BT 栈本身的修改才在这里。

用途：代码上传/上游交代。回答三个问题：**改了什么、为什么改、用原版会出什么问题**。
不包含 app 侧（`ds4dongle-bl618`）的改动。

提交：`ec8c6e9d feat(usb,bt): P0 audio session hardening, F0 guard rework, BT TX ownership`
15 个文件，+2028/−91。全部为本地 commit，未 push。

---

## 0. 分类（上传时建议按这个拆分/说明）

| 类别 | 文件 | 是否 DS4 专用 |
|---|---|---|
| A. 通用安全/健壮性修复 | `usbd_audio.c`（R06 长度校验）、`conn.c`（有界等待/非阻塞）、`l2cap*.c`（所有权/错误传播）、`freertos_port.c`（fault halt） | 否，可上游 |
| B. USB 设备控制器语义补齐 | `bflb_usb_v2.c`（cancel/witness、F0、ISO 完成策略）、`usb_dc.h` | 部分通用；F0 判据依赖本硬件 FIFO |
| C. CherryUSB 核心行为修正 | `usbd_core.c`（SET_INTERFACE 端点集合、存在性预检） | 通用问题，但改动面需上游评审 |
| D. 调试/诊断增强 | `vsnprintf.c`（console tap）、`syscalls_nosys.c`（weak `_write`）、`hci_core.c`（MTU hook）、F0 计数 | 否，纯增量 |

---

## 1. `drivers/lhal/src/bflb_usb_v2.c` + 新文件 `bflb_usb_v2_guard.h`

### 原版的问题

1. **端点没有“取消”语义。** `usbd_ep_close()` 只是清 START、复位 FIFO、清软件状态；
   `usbd_ep_start_write/read()` 只查 `ep_enable`，不检查是否有取消在进行。
   于是 close/reset 之后、应用还没处理完时，feed/任务仍可能向一个已关闭或正在取消的端点提交，
   旧 DMA 的完成也可能被记到新事务上。这是 P0 里 R02/R10 的根因。
2. **F0 安全网判据是“连续 200 次 G3 中断没有 F0 完成”**，与时间/进度无关；
   首次命中就把 `out_ep.ep_active` 永久清掉——扬声器从此静默，且没有任何可观测事件。
   实测中“主机暂停播放（alt1 仍开、只是不发 ISO）”就会满足该条件，导致暂停后永久无声。
3. **G2 零字节完成与 G3 VDMA 完成可以对同一端点各派发一次**：
   `USB_RX0BYTE_INT` 的处理不看 `ep_active`/`xfer_buf`/取消状态，类层回调会重复 re-arm，
   旧完成会被记到新传输上。
4. **没有 arm 时间/事务代次**，无法判断“在途事务是否超时”。

### 改动

- 新增 `usbd_ep_cancel()` / `usbd_ep_cancel_settled()`（声明在 `usb_dc.h`）：
  清 START、复位 FIFO、清已锁存的 G3 完成位、释放软件槽、置 PENDING；
  `settled` 以**硬件状态**（START 位 + 该 FIFO 的完成位都为 0）为见证，不用时间猜。
  PENDING 期间 `usbd_ep_start_write/read()` 返回 `-3`，且数据端点的
  “PENDING 检查 + 软件槽发布 + VDMA 启动”在**同一个屏蔽中断区**内完成。
- F0 安全网重写为 header-only `bflb_usb_v2_guard.h`：
  - 判据加入 **FIBC0 FIFO 占用**：FIFO 为空 = 主机没发数据（暂停），**不得** abort；
    只有“FIFO 有数据但 VDMA 长时间不排空”才算卡死。
  - abort 预算改为**连续失败**（真实完成清零），干净 pass 后才合成零长度完成；
    终态 FAULT 通过弱钩子 `usbd_event_f0_fault_handler()` 上报，应用可恢复，不再静默停用。
  - 计数器：`idle/safetynet/synth/fault/stale_drop/witness_timeout/misfire`。
- ISO OUT 只由 G3 VDMA 完成退休：RX0BYTE 对 ISO 端点不派发；
  非 ISO 端点保留“所有权门 + G3 待处理快照”。
- 记录每个端点最近一次 arm 时间；G3 段先快照状态字再消费。

### 用原版会怎样

- 暂停/恢复播放后扬声器永久无声（已实测复现：`[EVT] audio spk ep fault`）。
- close/reopen、USB RESET、快速重连时旧完成/旧提交错配，出现“偶发无声、切设备才恢复”。
- 无法从计数判断 F0 是误触发还是真卡死。

---

## 2. `components/usb/cherryusb/core/usbd_core.c`

### 原版的问题

1. `usbd_set_interface()` 只看 `alt_setting == 0` 就关闭该接口的所有端点。
   但 HID 的 alt0 **本身就带 IN 端点**（DS4 描述符如此），合法的
   `SET_INTERFACE(hid, 0)` 会把 IN 端点拆掉，应用侧 `ep_in_busy` 仍为 true，
   之后输入永远无法恢复提交。
2. 用 `LO_BYTE(wIndex)` 直接索引 `intf_altsetting[16]`，没有边界/存在性检查：
   主机发 `wIndex >= 16` 或指向不存在的接口时会越界/误操作。
3. `if_desc == NULL` 时仍然向类层广播通知；端点操作失败也照样通知。
4. 端点已打开时重复 `SET_INTERFACE` 只重配 MPS/FIFO，不中止在途 VDMA。

### 改动

- 先做一次“目标 interface+alt 是否存在”的预检，不存在直接 STALL 且不动任何状态。
- 按**目标 alt 的端点集合**做差量：目标 alt 的端点重配置；上一个 alt 的端点关闭；
  其他 alt 不动。重配置前先 `usbd_ep_cancel()` 退休在途传输。
- `SET_CONFIGURATION` 和总线 reset 清空 `intf_altsetting[]`。
- 只在端点操作成功后才 `usbd_class_event_notify_handler()`。

### 用原版会怎样

- 合法 HID `SET_INTERFACE(alt0)` 后手柄输入卡死（需要拔插）。
- 非法 `wIndex` 可能越界写/误关别人的端点。
- 重复 alt 重配时旧完成错配到新传输。

---

## 3. `components/usb/cherryusb/class/audio/usbd_audio.c`（R06）

### 原版的问题

`memcpy(&volume, *data, *len)`、mute、sampling-freq 都没有校验方向和长度：
`*len` 由主机控制，超长会越界写栈上的 1/2/4 字节变量；GET/SET 方向也不检查。

### 改动

对 mute（1B）、volume（2B）、sampling freq（UAC1 3B / UAC2 4B）做
“方向 + 精确长度”校验，非法请求返回 -1（STALL）。

### 用原版会怎样

主机（或恶意/异常驱动）可以越界写设备栈；正常的超长 SET_CUR 也会破坏相邻变量。

---

## 4. BLE host（`blestack/src/host/`）

### 4.1 `conn.c` / `conn_internal.h`

原版：
- `conn_tx_alloc()` 用 `K_FOREVER`；在本构建（`BFLB_BLE`，无 `CONFIG_BT_DEBUG_CONN`）下
  没有 no-wait 守卫，pool 耗尽会**无限阻塞调用者**——包括 BT workqueue（TX context 正是在那里释放，
  形成自死锁）和任何提交的应用任务。
- `net_buf_put()` → `k_queue_append()` → `xQueueSend(..., portMAX_DELAY)`，
  队列满时阻塞，无法用于“绝不能阻塞”的音频/控制路径。

改动：
- `CONN_TX_ALLOC_TIMEOUT = 20 ms` 的有界等待；`conn_tx_alloc_no_wait()` 用 `K_NO_WAIT`。
- `conn_tx_queue_put_no_wait()`：先统计整条 frag 链、检查剩余空间，再用 0 tick 入队；
  要么整链入队，要么返回 `-ENOBUFS`，不会留下半条链。
- `bt_conn_send_cb_no_wait()`：非阻塞版发送；所有错误都返回 `-ENOBUFS`（可重试）。

用原版：pool 耗尽时 BT 任务/音频任务被卡死；TX 队列满时提交路径阻塞；
半条链入队后调用者 unref 会造成已入队 buffer 被释放。

### 4.2 `l2cap.c` / `l2cap_br.c` / `l2cap_internal.h` / `l2cap.h`

原版：
- `bt_l2cap_br_chan_send_cb()` 调用 `bt_l2cap_send_cb()` 后**丢弃返回值**，接着读 `buf->len`。
  断连时内层已 unref 该 buffer 并返回 `-ENOTCONN`，外层仍在读已释放内存；
  应用还会把失败当成功，等一个永远不会来的完成。
- `bt_l2cap_br_chan_send()` 同类问题。
- 没有明确的“谁消费 buffer”的契约，调用者容易二次 unref。
- 没有按 CID/通道实例校验的“提交即入队”路径，断连/重连窗口内可能把 PDU 交给已退休的通道。

改动：
- 先锁存长度再交出 buffer；传播内层错误；失败后不再触碰 buffer；
  统一“每次返回都消费”的契约（文档写清各分支）。
- 新增 `bt_l2cap_send_cb_no_wait()` 与 `bt_l2cap_br_chan_send_handoff()`：
  在调用者持锁窗口内完成“通道/ CID 复核 + 非阻塞入队”，
  `lookup_tx_cid(conn,cid)==chan` 拒绝被重绑的通道实例。
- LE `l2cap_chan_le_send()` 在首段已分配、随后断链返回 `-ECONNRESET` 的路径上释放该段。

用原版：断连瞬间的 use-after-free / 假成功；控制通道（feature/handshake）在资源紧张时阻塞；
LE 首段泄漏。

---

## 5. 其他小改动

| 文件 | 改动 | 原因 |
|---|---|---|
| `components/libc/newlib/syscalls_nosys.c` | `_write` 加 `__attribute__((weak))` | 允许应用接管 console 输出（调试日志） |
| `components/libc/vsnprintf.c` | 新增 `g_console_tap`，console 每个字符回调一次 | 应用把 console 输出镜像到日志环（UART 为主，环为辅） |
| `components/os/freertos/freertos_port.c` | assert/stack overflow/malloc failed 不再 `printf+abort`，改为记录 `g_fault_reason/g_fault_arg` 后关中断停机 | 这些 hook 可能在 ISR 或已损坏的栈上运行，printf/backtrace 会死锁或二次崩溃；停机前留下可读记录 |
| `components/wireless/bluetooth/blestack/src/host/hci_core.c` | 新增 `g_bt_mtu_hook`，上报 ACL MTU/包数 | 只读诊断（音频节拍/吞吐） |

---

## 6. 上游/评审时需要特别说明的点

1. **F0 守卫的 FIFO 占用判据**依赖 BL616 DCIA 的 `FIBC0`（0x1B0）语义：
   “有数据排不出去”才是卡死。若上游不认可该判据，需要保留“空闲不 abort”这一条，
   否则暂停播放会再次变成永久静音。
2. **ISO OUT 不使用 RX0BYTE 完成**：这是按 USB 规范（ISO 无 ZLP 终止语义）做的选择；
   若未来有 ISO OUT 需要 ZLP 的场景，需要重新评估。
3. **SET_INTERFACE 的端点集合差量**改变了 CherryUSB 的既有行为（原版按 alt==0 一刀切），
   属于通用修正，但影响面较大，建议单独 commit 并附 HID alt0/音频 alt1 的用例。
4. **L2CAP 的“每次返回都消费”契约**与上游现有调用者可能不一致：本仓库内已审计所有调用者，
   上游合并时需要再扫一遍其他 profile（LE/SMP 等）。
5. `conn_tx_alloc` 从 `K_FOREVER` 改为 20 ms 有界等待，是行为变更：调用者必须能处理 `-ENOBUFS`。

---

## 7. 建议的上传拆分

1. `usbd_audio` 长度校验（R06）+ `usbd_core` 存在性预检（R07）——独立、低风险。
2. `conn`/`l2cap` 所有权与非阻塞发送——需要附调用者审计。
3. `bflb_usb_v2` cancel/witness + F0 守卫——附硬件复现与计数。
4. 调试增强（console tap、weak `_write`、MTU hook）——独立。
