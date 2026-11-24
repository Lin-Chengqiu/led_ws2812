# led_ws2812

非阻塞 WS2812 / WS2812B 灯带驱动 + 灯效引擎。

[English version](README_EN.md)

---

## 目录

1. [特性](#1-特性)
2. [文件结构](#2-文件结构)
3. [快速开始](#3-快速开始)
4. [API 速查](#4-api-速查)
5. [灯效模式](#5-灯效模式)
6. [编码流程](#6-编码流程)
7. [实现传输后端](#7-实现传输后端)
8. [术语对照：数据手册 ↔ 库](#8-术语对照数据手册--库)
9. [编译期配置](#9-编译期配置)
10. [返回值与约束](#10-返回值与约束)
11. [资源占用](#11-资源占用)
12. [构建与验证](#12-构建与验证)
13. [许可](#13-许可)

> 数据手册：[`docs/WS2812B_V5_Datasheet.pdf`](docs/WS2812B_V5_Datasheet.pdf)（WorldSemi 官方 V5 版）。本文的时序与术语均以此为准。

---

## 1. 特性

| 项目    | 说明                                                                 |
| ----- | ------------------------------------------------------------------ |
| 非阻塞   | 动画在 `ws2812_ticks()` 里推进，ISR 内不做位翻转；底层回调启动传输后立即返回                  |
| 零动态内存 | 三块缓冲全部由调用者提供、挂在 `WS2812Backend` 上；库不 `malloc`                      |
| 多灯带   | 一个 `ws2812_ticks()` 驱动所有已注册的灯带，每条灯带各持一个后端                          |
| 灯效引擎  | 内置 OFF / SOLID / BLINK / BREATH / RAINBOW / CHASE，MANUAL 模式可直接写帧缓冲 |
| 颜色处理  | 六种线序、主亮度、gamma 2.2 查表，全部在编码期完成                                     |
| 故障恢复  | 忙时丢帧重试、起拍保护、传输超时强制恢复并报 `ERROR` 事件                                  |

---

## 2. 文件结构

```
led_ws2812/
├── ws2812.h                      公开 API、类型、编译期配置
├── ws2812.c                      编码 / 灯效状态机 / 链表调度
├── tests/
│   └── test_ws2812.c             主机端单元测试，无需硬件
├── docs/
│   └── WS2812B_V5_Datasheet.pdf  WorldSemi 官方数据手册
├── README.md                     本文（简体中文）
├── README_EN.md                  English
├── Makefile
├── CMakeLists.txt
└── LICENSE
```

把 `ws2812.h` / `ws2812.c` 加入工程即可；也可用 `add_library(ws2812 STATIC ws2812.c)` 构建。

---

## 3. 快速开始

```c
#include "ws2812.h"

#define LED_NUM 24

static uint8_t  s_frame[LED_NUM * 3];             /* 逻辑帧缓冲，RGB */
static uint8_t  s_pixel_data[LED_NUM * 3];        /* 24bit GRB 数据 */
static uint16_t s_words[LED_NUM * 3 * 8 + 8];    /* 时序字（含前后 RES） */
static WS2812   s_strip;

/* 传输后端：两个纯硬件回调 + 两块缓冲，见 §7 */
static WS2812Backend s_backend = {
    .transmit   = my_transmit,
    .is_busy    = my_is_busy,
    .pixel_data = s_pixel_data,
    .timing     = { .words = s_words, .word_cap = LED_NUM * 3 * 8 + 8,
                    .t0h = CMP_LOW, .t1h = CMP_HIGH, .res_level = 0,
                    .res_lead = 4, .res_trail = 4 },
};

void app_init(void)
{
    ws2812_init(&s_strip, s_frame, LED_NUM);
    ws2812_attach_backend(&s_strip, &s_backend);
    ws2812_set_order(&s_strip, WS2812_ORDER_GRB);
    ws2812_set_color(&s_strip, 0xFF0000);
    ws2812_set_mode(&s_strip, WS2812_MODE_BREATH);
    ws2812_start(&s_strip);
}

/* 1ms 定时器中调用，或在主循环里按 1ms 节拍调用 */
void app_tick_1ms(void)
{
    ws2812_ticks();
}
```

三条约束：

1. `ws2812_ticks()` 必须按 **1ms** 节拍调用（`WS2812_TICKS_INTERVAL` 可改，但相位与节流精度随之变化）。
2. `transmit()` 与 `is_busy()` 必须非阻塞。
3. 三块缓冲（逻辑 RGB、24bit GRB 数据、时序字）都由调用者提供；后两块挂在 `WS2812Backend` 上，逻辑帧缓冲传给 `ws2812_init()`。

---

## 4. API 速查

### 生命周期

| 函数                                                            | 说明                                                        |
| ------------------------------------------------------------- | --------------------------------------------------------- |
| `ws2812_init(h, buf, pixel_count)`                            | 初始化句柄。默认 GRB / 亮度 255 / 20ms 一帧 / 1000ms 周期 / MANUAL      |
| `ws2812_attach_backend(h, &backend)`                               | 挂接传输后端。`pixel_data` 接收 24bit GRB 数据，`timing.words` 接收时序字  |
| `ws2812_attach(h, event, cb, ud)` / `ws2812_detach(h, event)` | 挂接 / 移除事件回调（`FRAME` / `CYCLE` / `ERROR`）                  |
| `ws2812_start(h)` / `ws2812_stop(h)`                          | 加入 / 移出调度链表；返回 `0` / `WS2812_ERR_BUSY` / `WS2812_ERR_ARG` |
| `ws2812_ticks()`                                              | 后台心跳，遍历链表                                                 |
| `ws2812_step(h)`                                           | 推进单条灯带一个 tick（传输状态、帧节流、渲染、刷屏）；公开以便分步调试与单元测试    |

### 参数

`ws2812_set_order()` · `ws2812_set_brightness()` · `ws2812_set_gamma()` · `ws2812_set_frame_ms()` · `ws2812_set_period_ms()`

### 绘图

`ws2812_set_pixel()` · `ws2812_get_pixel()` · `ws2812_fill()` · `ws2812_clear()` · `ws2812_refresh()`

### 灯效

`ws2812_set_mode()` · `ws2812_set_color()` · `ws2812_get_mode()` · `ws2812_get_color()` · `ws2812_restart_effect()`

`ws2812_restart_effect()` 只把动画相位、色相与周期计时归零并强制下一 tick 重绘；**不清帧缓冲、不复位芯片**。要灭灯用 `ws2812_clear()`。

工具函数 `ws2812_hsv(hue, sat, val)` 是纯转换，返回 `0xRRGGBB`；hue 0..1535 对应 0..360°。

### 辅助函数

`ws2812_build_timing_words(timing, data, len)` — 把 24bit GRB 数据构建成时序字写入 `timing.words`。库内部自动调用，也可单独测试。

---

## 5. 灯效模式

| 模式                    | 行为                              | `period_ms` 的含义 | 每帧重绘    |
| --------------------- | ------------------------------- | --------------- | ------- |
| `WS2812_MODE_MANUAL`  | 帧缓冲归用户，驱动只负责刷屏                  | 无意义             | 否       |
| `WS2812_MODE_OFF`     | 全灭                              | 无意义             | 仅进入时清一次 |
| `WS2812_MODE_SOLID`   | 纯色常亮                            | 无意义             | 仅在颜色变更时 |
| `WS2812_MODE_BLINK`   | 前半周期亮、后半周期灭                     | 一个明暗循环          | 仅翻转时    |
| `WS2812_MODE_BREATH`  | 亮度 0→255→0，平方律做视觉线性化            | 一次完整呼吸          | 是       |
| `WS2812_MODE_RAINBOW` | 整条彩虹，灯珠间有相位差                    | 色相转 360°        | 是       |
| `WS2812_MODE_CHASE`   | 单点跑马灯 + 拖尾（`WS2812_CHASE_TAIL`） | 跑完一整条           | 是       |

`WS2812_EVENT_CYCLE` 在每个 `period_ms` 结束时触发一次，可用于「呼吸三次后切色」这类序列编排。

---

## 6. 编码流程

```
0xRRGGBB ──► 帧缓冲 RGB ──► 亮度缩放 ──► gamma 2.2 ──► 线序重排 ──► build_timing_words ──► transmit()
             pixel_count*3      (0..255)    (256B LUT)   (GRB/BGR/…)     timing.words
                                   └──────────► backend.pixel_data ◄────────┘  (24bit GRB)
```

- 亮度是**线性域乘子**，先施加再做 gamma 编码；反过来会让低亮度段明显过亮。
- 颜色顺序在编码期完成，用户代码永远只用 `0xRRGGBB`；换灯珠型号只改 `WS2812_ORDER_*`。
- `ws2812_build_timing_words()` 输出固定为 `[RES 前导] + [每字节 8 字，MSB first] + [RES 尾随]`。

---

## 7. 实现传输后端

`WS2812Backend` 划分职责：库负责线序重排、亮度与 gamma、时序字展开、判忙、丢帧重试、超时恢复；底层只把时序字灌进外设，并回答「还在传吗」。

### 7.1 数据手册时序（配置 `WS2812BitTiming` 的依据）

| 手册符号          | 值（V5）           | 说明                         |
| ------------- | --------------- | -------------------------- |
| `T0H`         | 220–380ns       | 0 码高电平时间                   |
| `T1H`         | 580ns–1µs       | 1 码高电平时间                   |
| `T0L` / `T1L` | 580ns–1µs       | 低电平时间                      |
| 位周期           | 1.25us（800Kbps） | MSB first                  |
| `RES`         | >280µs          | 帧单元低电平时间，`WS2812_RESET_US` |

### 7.2 实现手段（不绑定厂商）

| 方式            | 思路                                                        | 代价                           |
| ------------- | --------------------------------------------------------- | ---------------------------- |
| 定时器 PWM + DMA | 一个位周期 = `ARR`，T0H/T1H 对应两个比较值，DMA 把每位占空比搬进比较寄存器           | 占一个定时器 + 一个 DMA 通道；CPU 只启动一次 |
| SPI 移位        | 用编码表把 1 bit 扩成 3~8 bit（如 `0b100` / `0b110`），靠波特率凑 800Kbps | 不占定时器，但要改线序为 MOSI            |
| 片上波形外设        | 把时序字直接喂给可编程波形发生器                                          | 最省 CPU，视芯片而定                 |
| GPIO 位翻转      | 手工延时翻转                                                    | 仅适合演示，中断一打断波形就废              |

### 7.3 `WS2812BitTiming` 与 `ws2812_build_timing_words()`

```c
typedef struct {
    uint16_t *words;       /* 时序字缓冲 */
    uint16_t word_cap;     /* 容量（字数） */
    uint16_t t0h;          /* T0H 对应的比较值 */
    uint16_t t1h;          /* T1H 对应的比较值 */
    uint16_t res_level;    /* RES 对应的比较值（通常为 0） */
    uint16_t res_lead;     /* 24bit 数据前的 RES 字数 */
    uint16_t res_trail;    /* 24bit 数据后的 RES 字数 */
} WS2812BitTiming;

uint16_t ws2812_build_timing_words(WS2812BitTiming *timing, const uint8_t *data, uint16_t len);
/* 返回写入 timing->words 的字数；参数非法或缓冲太小返回 0 */
```

`t0h != t1h` 是硬约束。两块缓冲属于后端对象，初始化一次即可，不必每帧传。

### 7.4 回调实现

```c
/* 库保证 words != NULL 且 count >= 3 */
static int my_transmit(WS2812 *h, const uint16_t *words, uint16_t count)
{
    (void)h;
    timer_reload();
    preload_ccr(words[0], words[1]);
    start_dma(&words[2], count - 2);
    return 0;
}

/* 只读硬件完成标志：1 在途，0 完成 */
static int my_is_busy(WS2812 *h)
{
    (void)h;
    return dma_channel_active();
}

static WS2812Backend s_backend = {
    .transmit     = my_transmit,
    .is_busy      = my_is_busy,
    .pixel_data   = s_pixel_data,   /* 24bit GRB 数据，pixel_count*3 */
    .timing       = { .words = s_words, .word_cap = WORD_MAX,
                      .t0h = CMP_LOW, .t1h = CMP_HIGH, .res_level = 0,
                      .res_lead = 4, .res_trail = 4 },
    .settle_ticks = 3,             /* 起拍保护：这几个 tick 内不查 busy */
    .timeout      = 100            /* 卡住时强制恢复并报 ERROR */
};

ws2812_init(&strip, frame, pixel_count);
ws2812_attach_backend(&strip, &s_backend);
ws2812_set_mode(&strip, WS2812_MODE_BREATH);
ws2812_start(&strip);
```

`settle_ticks` 用于「使能通道后状态位要等一段时间才有效」的硬件（例如 `CHACT`）；若 `is_busy()` 读的是立即可用的锁存位（例如 `CHEN`），填 `0`。

每条灯带各持一个 `WS2812Backend`，缓冲不共用。默认 20ms 一帧远大于 RES 时间；若把 `frame_ms` 压得很小，复位时间需由实现补足。

完整示例见 `tests/test_ws2812.c::test_transfer_backend`。

---

## 8. 术语对照：数据手册 ↔ 库

命名按 WorldSemi V5 手册的协议名词对齐，避免一词多义。

| 手册原文                        | 含义                                      | 库标识符                                                     |
| --------------------------- | --------------------------------------- | -------------------------------------------------------- |
| `Composition of 24bit Data` | G7..G0 R7..R0 B7..B0，GRB、high bit first | `WS2812Order`、`channel_order`、`pixel_data`               |
| `PIX` / `pixel`             | 单颗灯 = 一个 24bit 像素                       | `pixel_count`、`WS2812_BYTES_PER_LED`                     |
| `T0H` / `T1H`               | 0/1 码高电平时间                              | `t0h` / `t1h`（对应比较值）                                     |
| `RES`                       | Frame unit, low voltage time            | `res_level` / `res_lead` / `res_trail`、`WS2812_RESET_US` |
| `DIN` / `DOUT`              | 数据输入 / 级联输出                             | 由硬件层管理，库不涉及                                              |
| `Refresh Frequency`         | 刷新率 2KHz                                | `frame_ms` / `frame_ticks` / `ws2812_refresh()`          |
| `single NZR`                | 单线 NRZ 编码                               | `transmit()` + `timing.words`                            |
| `data latch`                | 像素内部锁存 24bit                            | RES 尾随字保证锁存                                              |

手册里没有 `code`、`guard`、`running`、`tx`、`wire` 这些词，因此库不使用它们作为公开字段名。

---

## 9. 编译期配置

在包含 `ws2812.h` 之前定义，或通过编译选项 `-D` 传入。

| 宏                          | 默认     | 说明                                           |
| -------------------------- | ------ | -------------------------------------------- |
| `WS2812_TICKS_INTERVAL`    | `1`    | `ws2812_ticks()` 的调用周期，单位 ms                 |
| `WS2812_DEFAULT_FRAME_MS`  | `20`   | 默认帧间隔（约 50fps）                               |
| `WS2812_DEFAULT_PERIOD_MS` | `1000` | 默认灯效周期                                       |
| `WS2812_CHASE_TAIL`        | `3`    | CHASE 拖尾长度，含最亮那颗                             |
| `WS2812_USE_GAMMA`         | `1`    | 编译进 256B gamma 2.2 查表；置 0 省 Flash            |
| `WS2812_RESET_US`          | `280`  | RES 低电平时间，手册要求 >280µs                        |
| `WS2812_THREAD_SAFE`       | 未定义    | 定义后须同时提供 `WS2812_LOCK()` / `WS2812_UNLOCK()` |

常量：`WS2812_BYTES_PER_LED = 3`、`WS2812_MAX_BYTES = 65535u`。版本宏 `WS2812_VERSION_MAJOR/MINOR/PATCH`。

回调在未加锁的上下文中执行，因此回调里可以安全地调用 `ws2812_start()` / `ws2812_stop()`。

---

## 10. 返回值与约束

| 宏                 | 值    | 触发条件             |
| ----------------- | ---- | ---------------- |
| `WS2812_ERR_BUSY` | `-1` | 传输在途，或句柄已在调度链表中  |
| `WS2812_ERR_ARG`  | `-2` | 参数非法、缓冲不足、句柄未初始化 |
| `0`               | —    | 成功               |

`ws2812_build_timing_words()` 是例外：它返回写入的**字数**，参数非法或缓冲不足时返回 `0`。

其余约束：

1. **缓冲分离**：`pixel_data`（24bit GRB 数据）与逻辑 RGB 帧缓冲必须分开，否则刷新后 `buf` 里的逻辑颜色会被改写。
2. **长度上限**：`pixel_count ≤ 21845`；`ws2812_start()` 会拒绝越界配置。时序字缓冲需容纳 `pixel_count × 24` 加前后 RES 字。
3. **丢帧而非阻塞**：底层忙时本帧丢弃并触发 `WS2812_EVENT_ERROR`，下一个 tick 重试。若 `frame_ms` 小于一帧传输时间（约 `pixel_count × 30us`），会持续丢帧。
4. **亮度是输出侧参数**：改动亮度不需要重绘帧缓冲，下一帧自动生效。
5. **`ws2812_step()` 直接调用会绕过链表**：验证 `stop` 行为要用 `ws2812_ticks()`。
6. **超时恢复不碰硬件**：超时只解除库自己的传输状态并报 `ERROR`；若外设真的卡死，下一帧仍会在 `is_busy()` 预检处被丢弃，需要硬件层自行复位。

---

## 11. 资源占用

32 位 MCU、`-Os`、C99，实测 `sizeof`：

| 项目                   | 32 颗灯                   | 说明                                    |
| -------------------- | ----------------------- | ------------------------------------- |
| `WS2812` 句柄          | 80 B                    | 含 3 个回调槽与链表指针                         |
| `WS2812Backend`      | 56 B                    | 含内嵌的 `WS2812BitTiming`（24 B）与库状态字段    |
| 逻辑帧缓冲 + `pixel_data` | 192 B                   | `2 × pixel_count × 3`                 |
| `timing.words`       | 1552 B                  | `(pixel_count × 24 + 8) × 2`，uint16_t |
| gamma 查表             | 256 B Flash             | `WS2812_USE_GAMMA=0` 可关               |
| 每帧 CPU               | 约 `pixel_count × 3` 次乘除 | 纯整数运算，无浮点                             |


合计 RAM 约 **1.9 KB**（32 颗灯）。灯数增大时主要成本是 `timing.words`，约 48 B/颗。

---

## 12. 构建与验证

```sh
make test                      # 编译并运行主机端单元测试
make clean

# 或 CMake
cmake -B build && cmake --build build && ctest --test-dir build
```

**已验证**（本机实际编译执行）：

- 单元测试 **34 个用例 / 225 项断言全部通过**：六种线序逐字节比对、亮度与 gamma 查表、亮度/gamma 应用顺序、帧节流、六种灯效状态机（含对称性、回卷、拖尾位置）、uint16 边界回卷、`CYCLE` 计数、回调中摘除后续灯带、多实例链表、`stop` 后不再刷屏、底层忙重试与 `ERROR` 事件、越界与空指针防护、24bit 编码、时序字展开、纯传输后端（transmit 参数保障、发送前 busy 回调、起拍保护、超时恢复）。
- 控制台末行：`checks: 225, passed: 225, failed: 0` / `RESULT: PASS`。
- 驱动本体在 `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wcast-qual -Wstrict-prototypes -Wmissing-prototypes -Wundef -Wswitch-enum` 下 **0 告警**。
- 目标板 HAL（HC32F460 Timer6 PWM + DMA）与库交叉编译、链接通过。

**未验证**：`transmit()` 的实际波形时序需要在你的硬件上确认。首次点亮请先用示波器量一位波形，高电平宽度应落在 T0H / T1H 窗口内，帧末低电平 > 280µs（V5 手册值）。

---

## 13. 许可

MIT. Author: WorkBuddy (operated by lincq). Copyright: 2026 lincq.

