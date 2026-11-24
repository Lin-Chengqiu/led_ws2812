# led_ws2812

Non-blocking WS2812 / WS2812B strip driver with an animation engine.

[简体中文](README.md)

---

## Contents

1. [Features](#1-features)
2. [Files](#2-files)
3. [Quick Start](#3-quick-start)
4. [API Reference](#4-api-reference)
5. [Animation Modes](#5-animation-modes)
6. [Encoding Pipeline](#6-encoding-pipeline)
7. [Implementing a Transfer Backend](#7-implementing-a-transfer-backend)
8. [Glossary: Datasheet ↔ Library](#8-glossary-datasheet--library)
9. [Compile-Time Configuration](#9-compile-time-configuration)
10. [Return Values and Constraints](#10-return-values-and-constraints)
11. [Resource Usage](#11-resource-usage)
12. [Build and Verification](#12-build-and-verification)
13. [License](#13-license)

> Datasheet: [`docs/WS2812B_V5_Datasheet.pdf`](docs/WS2812B_V5_Datasheet.pdf) (official WorldSemi V5). All timing values and terminology in this document follow that revision.

---

## 1. Features

| | |
|---|---|
| Non-blocking | Animation advances in `ws2812_ticks()`; no bit-banging in ISRs; the backend callback starts a transfer and returns immediately |
| No dynamic memory | All three buffers are caller-owned and attached to `WS2812Backend`; the library never calls `malloc` |
| Multiple strips | One `ws2812_ticks()` drives every registered strip; each strip owns its own backend |
| Animation engine | Built-in OFF / SOLID / BLINK / BREATH / RAINBOW / CHASE; MANUAL mode writes the frame buffer directly |
| Color processing | Six wire orders, master brightness and a gamma 2.2 LUT, all applied at encode time |
| Failure recovery | Frame drop and retry when busy, start-up settling window, transfer timeout with `ERROR` event |

---

## 2. Files

```
led_ws2812/
├── ws2812.h                      Public API, types, compile-time options
├── ws2812.c                      Encoding / animation state machines / list scheduler
├── tests/
│   └── test_ws2812.c             Host unit tests, no hardware required
├── docs/
│   └── WS2812B_V5_Datasheet.pdf  Official WorldSemi datasheet
├── README.md                     简体中文
├── README_EN.md                 This file
├── Makefile
├── CMakeLists.txt
└── LICENSE
```

Add `ws2812.h` / `ws2812.c` to your project, or build with `add_library(ws2812 STATIC ws2812.c)`.

---

## 3. Quick Start

```c
#include "ws2812.h"

#define LED_NUM 24

static uint8_t  s_frame[LED_NUM * 3];             /* Logical frame buffer, RGB */
static uint8_t  s_pixel_data[LED_NUM * 3];        /* 24bit GRB data */
static uint16_t s_words[LED_NUM * 3 * 8 + 8];    /* Timing words incl. leading/trailing RES */
static WS2812   s_strip;

/* Transfer backend: two hardware-only callbacks + two buffers, see §7 */
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

/* Call from a 1ms timer, or on a 1ms cadence in the main loop */
void app_tick_1ms(void)
{
    ws2812_ticks();
}
```

Three constraints:

1. `ws2812_ticks()` must be called on a **1ms** cadence (`WS2812_TICKS_INTERVAL` is configurable, but phase and throttling resolution scale with it).
2. `transmit()` and `is_busy()` must be non-blocking.
3. All three buffers (logical RGB, 24bit GRB data, timing words) are caller-owned. The latter two hang off `WS2812Backend`; the logical frame buffer is passed to `ws2812_init()`.

---

## 4. API Reference

### Lifecycle

| Function | Description |
|---|---|
| `ws2812_init(h, buf, pixel_count)` | Initialize the handle. Defaults: GRB, brightness 255, 20ms frame, 1000ms period, MANUAL |
| `ws2812_attach_backend(h, &backend)` | Attach a transfer backend. `pixel_data` receives 24bit GRB data, `timing.words` receives timing words |
| `ws2812_attach(h, event, cb, ud)` / `ws2812_detach(h, event)` | Register / remove event callbacks (`FRAME` / `CYCLE` / `ERROR`) |
| `ws2812_start(h)` / `ws2812_stop(h)` | Add to / remove from the scheduler list; returns `0` / `WS2812_ERR_BUSY` / `WS2812_ERR_ARG` |
| `ws2812_ticks()` | Background heartbeat; walks the whole list |
| `ws2812_step(h)` | Advance one strip by one tick: transfer tracking, throttle, render, refresh (public for step debugging and unit tests) |

### Parameters

`ws2812_set_order()` · `ws2812_set_brightness()` · `ws2812_set_gamma()` · `ws2812_set_frame_ms()` · `ws2812_set_period_ms()`

### Drawing

`ws2812_set_pixel()` · `ws2812_get_pixel()` · `ws2812_fill()` · `ws2812_clear()` · `ws2812_refresh()`

### Effects

`ws2812_set_mode()` · `ws2812_set_color()` · `ws2812_get_mode()` · `ws2812_get_color()` · `ws2812_restart_effect()`

`ws2812_restart_effect()` zeroes the animation phase, hue and period timing and forces a redraw on the next tick; it **does not clear the frame buffer or reset the chip**. Use `ws2812_clear()` to turn LEDs off.

Utility: `ws2812_hsv(hue, sat, val)` is a pure conversion returning `0xRRGGBB`; hue 0..1535 maps to 0..360°.

### Helper

`ws2812_build_timing_words(timing, data, len)` — builds 24bit GRB data into `timing.words`. Called internally by the library; can also be tested on its own.

---

## 5. Animation Modes

| Mode | Behavior | Meaning of `period_ms` | Redraw each frame |
|---|---|---|---|
| `WS2812_MODE_MANUAL` | Caller owns the frame buffer; driver only refreshes | N/A | No |
| `WS2812_MODE_OFF` | All off | N/A | Once on entry |
| `WS2812_MODE_SOLID` | Constant color | N/A | Only when color changes |
| `WS2812_MODE_BLINK` | On for the first half, off for the second | One on/off cycle | Only on toggle |
| `WS2812_MODE_BREATH` | Brightness 0→255→0, square law for perceptual linearity | One full breath | Yes |
| `WS2812_MODE_RAINBOW` | Whole-strip rainbow with per-pixel phase offset | 360° hue rotation | Yes |
| `WS2812_MODE_CHASE` | Moving dot with a tail (`WS2812_CHASE_TAIL`) | One pass along the strip | Yes |

`WS2812_EVENT_CYCLE` fires once at the end of each `period_ms`, enabling sequences such as "change color after three breaths".

---

## 6. Encoding Pipeline

```
0xRRGGBB ──► frame RGB ──► brightness ──► gamma 2.2 ──► channel reorder ──► build_timing_words ──► transmit()
             pixel_count*3     (0..255)     (256B LUT)     (GRB/BGR/…)       timing.words
                                   └──────────► backend.pixel_data ◄────────┘  (24bit GRB)
```

- Brightness is a **linear-domain multiplier**, applied before gamma encoding. Reversing the order makes low brightness levels visibly too bright.
- Channel reordering happens at encode time; caller code always uses `0xRRGGBB`. Switching LED variants only requires changing `WS2812_ORDER_*`.
- `ws2812_build_timing_words()` always emits `[leading RES] + [8 words per byte, MSB first] + [trailing RES]`.

---

## 7. Implementing a Transfer Backend

`WS2812Backend` splits the work: the library owns channel reordering, brightness, gamma, word expansion, busy detection, frame retry and timeout recovery. The port pushes timing words into a peripheral and answers "still transferring?".

### 7.1 Datasheet Timing (basis for `WS2812BitTiming`)

| Symbol | Value (V5) | Meaning |
|---|---|---|
| `T0H` | 220–380ns | 0 code, high voltage time |
| `T1H` | 580ns–1µs | 1 code, high voltage time |
| `T0L` / `T1L` | 580ns–1µs | Low voltage time |
| Bit period | 1.25us (800Kbps) | MSB first |
| `RES` | >280µs | Frame unit, low voltage time; `WS2812_RESET_US` |

### 7.2 Implementation Options (vendor-neutral)

| Method | Approach | Cost |
|---|---|---|
| Timer PWM + DMA | One bit period = `ARR`, T0H/T1H map to two compare values, DMA moves each duty value into the compare register | One timer + one DMA channel; CPU starts once |
| SPI shift | Expand 1 bit into 3–8 bits (e.g. `0b100` / `0b110`), pick a baud rate near 800Kbps | No timer, but needs MOSI wiring |
| On-chip waveform peripheral | Feed timing words to a programmable waveform generator | Lowest CPU load; chip-dependent |
| GPIO bit-banging | Manual delay loops | Demo only; any interrupt corrupts the waveform |

### 7.3 `WS2812BitTiming` and `ws2812_build_timing_words()`

```c
typedef struct {
    uint16_t *words;       /* Timing-word buffer */
    uint16_t word_cap;     /* Capacity in words */
    uint16_t t0h;          /* Compare value for T0H */
    uint16_t t1h;          /* Compare value for T1H */
    uint16_t res_level;    /* Compare value for RES (usually 0) */
    uint16_t res_lead;     /* RES words before the 24bit data */
    uint16_t res_trail;    /* RES words after the 24bit data */
} WS2812BitTiming;

uint16_t ws2812_build_timing_words(WS2812BitTiming *timing, const uint8_t *data, uint16_t len);
/* Returns the number of words written; 0 on invalid arguments or insufficient space */
```

`t0h != t1h` is a hard requirement. Both buffers belong to the backend object; configure them once, no per-frame passing.

### 7.4 Callback Implementation

```c
/* The library guarantees words != NULL and count >= 3 */
static int my_transmit(WS2812 *h, const uint16_t *words, uint16_t count)
{
    (void)h;
    timer_reload();
    preload_ccr(words[0], words[1]);
    start_dma(&words[2], count - 2);
    return 0;
}

/* Read a hardware status bit only: 1 in flight, 0 complete */
static int my_is_busy(WS2812 *h)
{
    (void)h;
    return dma_channel_active();
}

static WS2812Backend s_backend = {
    .transmit     = my_transmit,
    .is_busy      = my_is_busy,
    .pixel_data   = s_pixel_data,   /* 24bit GRB data, pixel_count*3 */
    .timing       = { .words = s_words, .word_cap = WORD_MAX,
                      .t0h = CMP_LOW, .t1h = CMP_HIGH, .res_level = 0,
                      .res_lead = 4, .res_trail = 4 },
    .settle_ticks = 3,             /* Start-up window: skip busy queries here */
    .timeout      = 100            /* Force recovery + ERROR when stuck */
};

ws2812_init(&strip, frame, pixel_count);
ws2812_attach_backend(&strip, &s_backend);
ws2812_set_mode(&strip, WS2812_MODE_BREATH);
ws2812_start(&strip);
```

`settle_ticks` is needed when the status bit becomes valid some time after enable (for example `CHACT`). If `is_busy()` reads a latched bit that is valid immediately (for example `CHEN`), use `0`.

Each strip needs its own `WS2812Backend`; buffers are not shared. The default 20ms frame is far longer than RES. If `frame_ms` is pushed very low, the port must supply the reset time before transmitting.

A complete example is `tests/test_ws2812.c::test_transfer_backend`.

---

## 8. Glossary: Datasheet ↔ Library

Identifiers follow the protocol vocabulary of the WorldSemi V5 datasheet to avoid overloading a single term with several meanings.

| Datasheet term | Meaning | Library identifier |
|---|---|---|
| `Composition of 24bit Data` | G7..G0 R7..R0 B7..B0, GRB, high bit first | `WS2812Order`, `channel_order`, `pixel_data` |
| `PIX` / `pixel` | One LED = one 24bit pixel | `pixel_count`, `WS2812_BYTES_PER_LED` |
| `T0H` / `T1H` | 0/1 code high voltage time | `t0h` / `t1h` (their compare values) |
| `RES` | Frame unit, low voltage time | `res_level` / `res_lead` / `res_trail`, `WS2812_RESET_US` |
| `DIN` / `DOUT` | Data in / cascade out | Managed by hardware, not exposed by the library |
| `Refresh Frequency` | 2KHz refresh rate | `frame_ms` / `frame_ticks` / `ws2812_refresh()` |
| `single NZR` | Single-wire NRZ encoding | `transmit()` + `timing.words` |
| `data latch` | Pixel-internal 24bit latch | Guaranteed by the trailing RES words |

The datasheet contains no `code`, `guard`, `running`, `tx` or `wire` as nouns, so the library does not use them for public field names.

---

## 9. Compile-Time Configuration

Define before including `ws2812.h`, or pass via `-D`.

| Macro | Default | Description |
|---|---|---|
| `WS2812_TICKS_INTERVAL` | `1` | Period of `ws2812_ticks()` in ms |
| `WS2812_DEFAULT_FRAME_MS` | `20` | Default frame interval (~50fps) |
| `WS2812_DEFAULT_PERIOD_MS` | `1000` | Default animation period |
| `WS2812_CHASE_TAIL` | `3` | CHASE tail length, including the brightest LED |
| `WS2812_USE_GAMMA` | `1` | Compile in the 256B gamma 2.2 LUT; set to 0 to save Flash |
| `WS2812_RESET_US` | `280` | RES low time; datasheet requires >280µs |
| `WS2812_THREAD_SAFE` | undefined | When defined, also provide `WS2812_LOCK()` / `WS2812_UNLOCK()` |

Constants: `WS2812_BYTES_PER_LED = 3`, `WS2812_MAX_BYTES = 65535u`. Version macros: `WS2812_VERSION_MAJOR/MINOR/PATCH`.

Callbacks run unlocked, so it is safe to call `ws2812_start()` / `ws2812_stop()` from inside a callback.

---

## 10. Return Values and Constraints

| Macro | Value | Condition |
|---|---|---|
| `WS2812_ERR_BUSY` | `-1` | Transfer in flight, or handle already registered |
| `WS2812_ERR_ARG` | `-2` | Invalid argument, insufficient buffer, uninitialised handle |
| `0` | — | Success |

`ws2812_build_timing_words()` is the exception: it returns the **number of words written**, or `0` on invalid arguments / insufficient space.

Other constraints:

1. **Keep buffers separate.** `pixel_data` (24bit GRB) and the logical RGB frame buffer must differ, or a refresh overwrites the logical colors in `buf`.
2. **Length limit.** `pixel_count ≤ 21845`; `ws2812_start()` rejects oversized configurations. The timing-word buffer must hold `pixel_count × 24` plus leading and trailing RES words.
3. **Drop, do not block.** When the backend is busy the frame is dropped, `WS2812_EVENT_ERROR` fires, and the next tick retries. If `frame_ms` is shorter than one transfer (~`pixel_count × 30us`), frames are dropped continuously.
4. **Brightness is output-side.** Changing brightness does not require redrawing the frame buffer; it takes effect on the next frame.
5. **Calling `ws2812_step()` directly bypasses the list.** Use `ws2812_ticks()` when verifying `stop()` behavior.
6. **Timeout recovery does not touch hardware.** It only clears the library's own transfer state and emits `ERROR`. If the peripheral is genuinely stuck, the next frame is still dropped at the `is_busy()` pre-check; a hardware reset is the port's responsibility.

---

## 11. Resource Usage

32-bit MCU, `-Os`, C99; measured with `sizeof`:

| Item | 32 LEDs | Notes |
|---|---|---|
| `WS2812` handle | 80 B | Includes 3 callback slots and the list pointer |
| `WS2812Backend` | 56 B | Includes the embedded `WS2812BitTiming` (24 B) and library state fields |
| Frame buffer + `pixel_data` | 192 B | `2 × pixel_count × 3` |
| `timing.words` | 1552 B | `(pixel_count × 24 + 8) × 2`, uint16_t |
| Gamma LUT | 256 B Flash | Disabled with `WS2812_USE_GAMMA=0` |
| CPU per frame | ~`pixel_count × 3` mul/div | Integer-only, no floating point |

Total RAM ≈ **1.9 KB** for 32 LEDs. As the strip grows, `timing.words` dominates at roughly 48 B per LED.

---

## 12. Build and Verification

```sh
make test                      # build and run the host unit tests
make clean

# or CMake
cmake -B build && cmake --build build && ctest --test-dir build
```

**Verified** (actually compiled and executed on this machine):

- Unit tests: **34 cases / 225 assertions, all passing**. Coverage: byte-exact comparison of six wire orders, brightness and gamma LUT, brightness/gamma ordering, frame throttling, six animation state machines (symmetry, wraparound, tail position), uint16 boundary wraparound, `CYCLE` counting, removing a later strip from a callback, multi-instance list, no refresh after `stop`, backend busy retry and `ERROR` event, bounds and null-pointer guards, 24bit encoding, timing-word expansion, transfer-only backend (transmit argument guarantees, pre-transfer busy callback, start-up window, timeout recovery).
- Console tail: `checks: 225, passed: 225, failed: 0` / `RESULT: PASS`.
- Core compiles with **0 warnings** under `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wcast-qual -Wstrict-prototypes -Wmissing-prototypes -Wundef -Wswitch-enum`.
- Target HAL (HC32F460 Timer6 PWM + DMA) cross-compiles and links against the library.

**Not verified**: the actual waveform timing of `transmit()` must be confirmed on your hardware. On first power-up, probe one bit with an oscilloscope: high-level widths should fall inside the T0H / T1H windows, and the trailing low level should exceed 280µs (V5 datasheet).

---

## 13. License

MIT. Author: WorkBuddy (operated by lincq). Copyright: 2026 lincq.
