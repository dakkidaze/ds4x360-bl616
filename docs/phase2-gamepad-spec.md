# Phase 2 手柄映射规格（DS4 BT → XUSB 20B + 触摸板连发）

状态：实施中。Phase 0 已通过硬件验收（`docs/phase0-verification.md`）。
本轮 **AUDIO 仍为 0**，只做手柄；音频（Phase 1）排在本阶段之后。

约束：映射逻辑必须可在 host 上单测，不依赖 SDK；USB 侧只负责收发。

## 1. 范围

- DS4 BT 状态块（63B，`main.c:on_hid_input()` 已解析）→ XUSB 20B 输入报告。
- 按钮 / 十字键 / 摇杆（含 Y 取反、非对称缩放、径向死区）/ 扳机。
- 触摸板修饰键连发（语义见 §6）。
- XUSB OUT：8B rumble、3B LED → DS4 0x05 → 现有 BT 输出路径。
- 4 ms 节拍；断连时发中立报告。
- 不做：触摸板点击上报、陀螺仪、触摸坐标、音频。

## 2. 文件归属（并行写入范围）

**Lane A（纯逻辑，host 可测）**
- `src/xinput_map.h` / `src/xinput_map.c`
- `src/xinput_turbo.h`（连发纯函数）
- `tools/xinput_map_test.c`（host 单测）

**Lane B（集成，依赖 A 的头文件）**
- `src/usb_xinput.c` / `src/usb_xinput.h`（输出回调）
- `src/usb_gamepad.c`（DS4 0x05 胶水、初始化时注册回调）
- `src/main.c`（4 ms 节拍、状态快照、调用映射）
- `CMakeLists.txt`（XINPUT 构建加入 `src/xinput_map.c`）

A、B 不互相改文件。B 在 A 的 `.h` 落地后再开始编译。

## 3. 输入：DS4 状态块（63B）

| 偏移 | 内容 | 位定义 |
|---|---|---|
| 0 | LX | 0..255，128 居中 |
| 1 | LY | 0..255，128 居中（DS4 向下为正） |
| 2 | RX | 同上 |
| 3 | RY | 同上 |
| 4 | 低 4 位 hat + 高 4 位面键 | hat: 0=N,1=NE,2=E,3=SE,4=S,5=SW,6=W,7=NW,8=中立；面键: 0x10 Square, 0x20 Cross, 0x40 Circle, 0x80 Triangle |
| 5 | 肩键/扳机/系统键 | 0x01 L1, 0x02 R1, 0x04 L2, 0x08 R2, 0x10 Share, 0x20 Options, 0x40 L3, 0x80 R3 |
| 6 | PS / 触摸板 | 0x01 PS(Guide), 0x02 触摸板点击（只作修饰键，不上报） |
| 7 | L2 模拟量 | 0..255 |
| 8 | R2 模拟量 | 0..255 |
| 9+ | 时间戳/触摸/陀螺仪/电池 | 本阶段不用 |

位定义与 `src/ds4_hid_desc.h` 的 14 个按钮 usage（Square, Cross, Circle, Triangle,
L1, R1, L2, R2, Share, Options, L3, R3, PS, Touchpad）一致。

## 4. 输出：XUSB 20B 报告

| 偏移 | 内容 |
|---|---|
| 0 | 0x00 |
| 1 | 0x14 |
| 2–3 | 按钮字 u16 LE，合法掩码 `0xF7FF`（bit11 保留） |
| 4 | LT 0..255 |
| 5 | RT 0..255 |
| 6–7 | LX int16 LE |
| 8–9 | LY int16 LE（XInput 向上为正） |
| 10–11 | RX int16 LE |
| 12–13 | RY int16 LE |
| 14–19 | 0 |

## 5. 映射

| XInput | 位 | DS4 |
|---|---|---|
| Dpad 上/下/左/右 | 0x0001/0x0002/0x0004/0x0008 | hat（对角置两位，8=无） |
| Start | 0x0010 | Options（byte5 0x20） |
| Back | 0x0020 | Share（byte5 0x10） |
| L3 / R3 | 0x0040 / 0x0080 | byte5 0x40 / 0x80 |
| LB / RB | 0x0100 / 0x0200 | L1 / R1 |
| Guide | 0x0400 | PS（byte6 0x01） |
| A / B / X / Y | 0x1000/0x2000/0x4000/0x8000 | Cross / Circle / Square / Triangle |
| LT / RT | byte4 / byte5 | byte7 / byte8 直通，无缩放 |

写入前 `buttons &= 0xF7FF`。

### 摇杆

```
v = raw - 128
r = (v >= 0) ? v / 127.0f : v / 128.0f     // -1..1，128 -> 0
if (axis == Y) r = -r                       // DS4 向下为正 -> XInput 向上为正
```

径向死区（每根摇杆独立，r 空间）：

```
mag = sqrt(rx*rx + ry*ry)
if (mag <= DZ) { rx = ry = 0; }
else {
    t = (mag - DZ) / (MAX - DZ);   // DZ=0.10, MAX=1.00
    t *= (1.0f + ANTI);            // ANTI=0.20
    if (t > 1.0f) t = 1.0f;
    rx = rx / mag * t;  ry = ry / mag * t;
}
```

输出：

```
out = (int16_t)((r + 1.0f) * 0.5f * 65535.0f - 32768.0f)
```

必须满足：raw 128 → 0；raw 0 → -32768（X 轴）/ +32767（Y 轴，取反）；
raw 255 → 32767（X）/ -32768（Y）。端点用浮点后截断，允许 ±1 LSB。

## 6. 触摸板连发（已确认）

- 触摸板按住 + 目标键连续按住 ≥ **400 ms** 才连发；不足 400 ms 保持实按。
- 计时起点 = `max(按键按下, 触摸板按下)`。
- 频率 **15 Hz**：半周期 33 ms，相位 `off = ((now_ms / 33) & 1) == 1`。
- 目标键：A/B/X/Y + LB/RB（0x1000/0x2000/0x4000/0x8000/0x0100/0x0200）。
- 触摸板松开或目标键松开 → 立即恢复普通语义并清状态。
- 只作用于最终按钮字；不改变扳机/摇杆/震动/LED。

纯函数接口建议：

```c
typedef struct { uint32_t since_ms[TURBO_BUTTONS]; uint8_t armed; } xinput_turbo_t;
void     xinput_turbo_init(xinput_turbo_t *t);
uint16_t xinput_turbo_apply(xinput_turbo_t *t, uint16_t pressed,
                            bool touchpad, uint32_t now_ms);
```

返回本帧应上报的按钮字。未激活的键原样通过。`since_ms` 用 `UINT32_MAX`
作“未激活”哨兵，避免 `now_ms == 0` 歧义。

## 7. 输出路径（rumble / LED）

`usb_xinput.c` 已有 OUT 解析与计数。新增 ISR 安全回调：

```c
typedef void (*usb_xinput_rumble_cb_t)(uint8_t strong, uint8_t weak);
typedef void (*usb_xinput_led_cb_t)(uint8_t cmd);
void usb_xinput_set_output_cbs(usb_xinput_rumble_cb_t r, usb_xinput_led_cb_t l);
```

XUSB 8B：`00 08 00 <strong> <weak> 00 00 00`；3B：`01 03 <cmd>`。

`usb_gamepad.c` 的 XINPUT 分支注册回调，胶水必须 ISR 安全（只填 32B 缓冲 +
调用现有 `output_callback`，它内部是 `xQueueSendToBackFromISR`）：

DS4 USB 0x05 载荷（`msg[0]` 是 report id，BT payload 从 `msg[1]` 开始）：

| msg 下标 | 含义 |
|---|---|
| 0 | 0x05 |
| 1 | flags：0x01 rumble，0x02 lightbar |
| 2–3 | 0 |
| 4 | DS4 小/高频马达 = XUSB **weak**（字节 4） |
| 5 | DS4 大/低频马达 = XUSB **strong**（字节 3） |
| 6–8 | 灯条 R/G/B（仅 LED 命令） |

`cmd 0x06..0x09` → 玩家 1..4 常亮，映射为不同颜色（P1 蓝 / P2 红 / P3 绿 /
P4 品红）；其他 cmd 忽略。现有 `on_hid_primer()` 的金色灯条保留，玩家灯只在
主机发 LED 命令时覆盖。

## 8. main.c 集成

XINPUT 构建下：

1. `usb_task` 保留一份 `latest[63]`；`xQueueReceive` 收到新报告时 `memcpy`。
2. 每轮循环（含超时）调用映射并发送：
   ```c
   xinput_map_set_state(&map, latest, ds4_connected);
   xinput_map_tick(&map, now_ms, report);
   usb_xinput_send_report(report);
   ```
3. 等待时间改为 `pdMS_TO_TICKS(4)`（仅 XINPUT 构建），保证连发相位与摇杆刷新。
4. `ds4_connected == false` 时映射输出全中立（20B 全 0，仅 `00 14` 头）。
5. 删除/绕过 Phase 0 的固定 A 报告：`usb_xinput_register()` 里不再预置
   “A 常按”；首帧由映射产生。
6. 非 XINPUT 构建路径完全不变。

`now_ms = (uint32_t)(bflb_mtimer_get_time_us() / 1000ULL)`（main.c 已有同款用法）。

## 9. Host 单测（Lane A）

`cc -std=c11 -Wall -Wextra -I src tools/xinput_map_test.c src/xinput_map.c -lm -o /tmp/xinput_map_test`

覆盖：

1. 每个按钮单独按下 → 对应 XInput 位，且不影响其他位。
2. 8 个 hat 方向 + 对角。
3. 摇杆端点/中点（X 与 Y），Y 取反。
4. 死区内输出 0；死区外单调、端点可达。
5. 扳机 0/128/255 直通。
6. 按钮字掩码只剩 `0xF7FF` 内的位。
7. 连发：触摸板未按 → 永不脉冲；按住 399 ms → 实按；401 ms 后按 33 ms 相位
   出现 off 帧；松开复位；先按 A 再按触摸板 → 从触摸板按下重新计时。
8. 断连 → 20B 全中立。

## 10. 验收

- host 单测全过；`-Wall -Wextra` 无警告。
- `BOARD_TYPE=lctech616 USB_SPEED=fs AUDIO=0 XINPUT=1 bash build_macos.sh` 通过，
  无新增警告。
- 默认 DS4 persona 构建回归通过。
- 真机：按钮/摇杆/扳机逐项；按住触摸板长按 A 出现连发；XInput 测试工具震动时
  DS4 实际震动（证明 OUT 链路）。
