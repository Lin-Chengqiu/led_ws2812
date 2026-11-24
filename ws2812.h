/*
 * ws2812.h - Non-blocking WS2812/WS2812B driver with animation engine
 *
 * Features:
 *   1. No dynamic memory: frame and transmit buffers are supplied by the caller.
 *   2. No blocking: animation advances in ws2812_ticks(); no bit-banging in ISR.
 *   3. Platform-independent: the only seam is a hardware-only transfer backend.
 *   4. Multiple strips: one ws2812_ticks() drives all registered strips.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef WS2812_H
#define WS2812_H

#include <stdint.h>

/* ============================ Return codes ============================ */

/* A resource is busy: transfer in progress, or handle already registered. */
#define WS2812_ERR_BUSY  (-1)
/* Invalid argument, buffer too small, or uninitialised handle. */
#define WS2812_ERR_ARG   (-2)

/* ============================ Version ============================ */

#define WS2812_VERSION_MAJOR 1
#define WS2812_VERSION_MINOR 0
#define WS2812_VERSION_PATCH 0

/* ============================ Build-time config ============================ */

/* Background tick interval in ms. 1ms is recommended; 5ms also works with
 * reduced animation phase and blink-edge resolution. */
#ifndef WS2812_TICKS_INTERVAL
#define WS2812_TICKS_INTERVAL 1
#endif

/* Default refresh interval in ms. 20ms ~ 50fps, enough for visual animation
 * and leaves headroom for the underlying transfer. */
#ifndef WS2812_DEFAULT_FRAME_MS
#define WS2812_DEFAULT_FRAME_MS 20
#endif

/* Default animation period in ms: one blink cycle, one rainbow revolution,
 * or one chase pass along the strip. */
#ifndef WS2812_DEFAULT_PERIOD_MS
#define WS2812_DEFAULT_PERIOD_MS 1000
#endif

/* Chase tail length, including the brightest LED. */
#ifndef WS2812_CHASE_TAIL
#define WS2812_CHASE_TAIL 3
#endif

/* Compile in a gamma 2.2 lookup table (256 bytes of Flash). */
#ifndef WS2812_USE_GAMMA
#define WS2812_USE_GAMMA 1
#endif

/* Bytes per LED: 3 for WS2812/SK6812/WS2812B. */
#define WS2812_BYTES_PER_LED 3

/* RES (frame-unit low time) from the WS2812B-V5 datasheet: >280us.
 * With the default 20ms frame interval no special handling is needed. */
#ifndef WS2812_RESET_US
#define WS2812_RESET_US 280
#endif

/* Maximum bytes per write call, limited by the uint16_t length parameter. */
#define WS2812_MAX_BYTES 65535u

/* ---- Optional RTOS locking; protects the list and shared state.
 * Example:
 *   #define WS2812_THREAD_SAFE
 *   #define WS2812_LOCK()   osMutexAcquire(ws_mutex, osWaitForever)
 *   #define WS2812_UNLOCK() osMutexRelease(ws_mutex)
 * Callbacks run unlocked, so they may safely call ws2812_start()/stop().
 */
#ifdef WS2812_THREAD_SAFE
  #if !defined(WS2812_LOCK) || !defined(WS2812_UNLOCK)
    #error "Define WS2812_LOCK() and WS2812_UNLOCK() when WS2812_THREAD_SAFE is enabled"
  #endif
#else
  #define WS2812_LOCK()
  #define WS2812_UNLOCK()
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ============================ Types ============================ */

/* Forward declarations */
typedef struct _WS2812 WS2812;

/* Wire byte channel_order varies across devices (WS2812B, SK6812, WS2812E...).
 * Reordering happens at encode time; callers always use 0xRRGGBB. */
typedef enum {
	WS2812_ORDER_RGB = 0,
	WS2812_ORDER_GRB,   /* Most common for WS2812B / WS2812E */
	WS2812_ORDER_BRG,
	WS2812_ORDER_RBG,
	WS2812_ORDER_GBR,
	WS2812_ORDER_BGR,   /* Common for SK6812 */
	WS2812_ORDER_COUNT
} WS2812Order;

/* Animation modes */
typedef enum {
	WS2812_MODE_MANUAL = 0, /* Caller owns the frame buffer; driver only refreshes */
	WS2812_MODE_OFF,        /* All off (buffer cleared once on entry) */
	WS2812_MODE_SOLID,      /* Solid color */
	WS2812_MODE_BLINK,      /* On for half a period, off for the other half */
	WS2812_MODE_BREATH,     /* Breathing: brightness 0 -> 255 -> 0 */
	WS2812_MODE_RAINBOW,    /* Whole-strip rainbow, 360 degrees per period */
	WS2812_MODE_CHASE,      /* Moving dot with a tail */
	WS2812_MODE_COUNT
} WS2812Mode;

/* Events index the callback table; do not reorder. */
typedef enum {
	WS2812_EVENT_FRAME = 0, /* A frame was handed to the transmit callback */
	WS2812_EVENT_CYCLE,     /* One animation period completed */
	WS2812_EVENT_ERROR,     /* Backend busy, bad argument or transfer timeout */
	WS2812_EVENT_COUNT
} WS2812Event;

/* One callback handles all events; the event is passed explicitly. */
typedef void (*WS2812Callback)(WS2812 *handle, WS2812Event event, void *user_data);

/* Hardware-only transfer hooks. Neither may inspect arguments, track state
 * or retry: the library does all of that.
 *   transmit : start sending count words, return immediately, 0 on success.
 *              The library guarantees words != NULL and count >= 3, so a
 *              two-stage preload may read words[0] and words[1] unchecked.
 *   is_busy  : 1 while the transfer is in flight, 0 when it has completed */
typedef int (*WS2812Transmit)(WS2812 *handle, const uint16_t *words, uint16_t count);
typedef int (*WS2812IsBusy)(WS2812 *handle);

/* Bit timing: the T0H/T1H/RES compare values from the datasheet,
 * plus the timing-word buffer. One instance per strip. */
typedef struct {
	uint16_t *words;        /* Caller-owned timing-word buffer */
	uint16_t word_cap;      /* Capacity in words (T0H/T1H/RES slots) */
	uint16_t t0h;          /* Compare value for T0H (0 code high time) */
	uint16_t t1h;          /* Compare value for T1H (1 code high time) */
	uint16_t res_level;    /* Compare value during RES (low level) */
	uint16_t res_lead;     /* RES words before the 24bit pixel data */
	uint16_t res_trail;    /* RES words after the 24bit pixel data */
} WS2812BitTiming;

/* Transfer backend. Fill transmit/is_busy/pixel_data/timing/settle_ticks/
 * timeout once; the remaining fields are owned by the library. */
typedef struct {
	WS2812Transmit transmit;   /* Required */
	WS2812IsBusy is_busy;      /* Required */
	uint8_t *pixel_data;       /* Required: 24bit GRB data, pixel_count*3 bytes */
	WS2812BitTiming timing;    /* Required: timing words + compare values */
	uint8_t settle_ticks;      /* Ticks to wait before the first busy query */
	uint16_t timeout;          /* Ticks allowed for a transfer before recovery */
	uint8_t running;           /* Library state: a transfer is being tracked */
	uint8_t settle_left;        /* Library state */
	uint16_t timeout_left;     /* Library state */
} WS2812Backend;

/* Strip handle */
struct _WS2812 {
	uint8_t *buf;            /* Pixel frame buffer: pixel_count * 3 bytes, always R,G,B */
	uint16_t pixel_count;    /* Number of LEDs */
	uint16_t ticks;          /* Ticks since last refresh (frame throttling) */
	uint16_t frame_ticks;    /* Refresh interval in ticks */
	uint16_t period_ticks;   /* Animation period in ticks */
	uint16_t phase;          /* Phase within the period, in ticks */
	uint16_t hue;            /* Rainbow phase 0..1535 (6 segments * 256) */
	uint8_t  brightness;     /* Master brightness 0..255, applied at refresh */
	uint8_t  channel_order;  /* Wire channel order, WS2812Order */
	uint8_t  mode;           /* Current effect, WS2812Mode */
	uint8_t  flag;           /* Internal flags, see WS2812_FLAG_* in the .c */
	uint32_t color;          /* Base color 0xRRGGBB */
	WS2812Backend *backend;  /* Transfer backend */
	WS2812Callback cb[WS2812_EVENT_COUNT];
	void *user_data;         /* Opaque context passed to callbacks */
	WS2812 *next;            /* Work list, traversed by ws2812_ticks() */
};

/* ============================ Functions ============================ */

/* ---- Lifecycle ---- */

/* Initialize the handle. buf is caller-owned and must hold at least pixel_count*3 bytes.
 * Defaults: GRB, brightness 255, 20ms frame, 1000ms period, MANUAL mode. */
void ws2812_init(WS2812 *handle, uint8_t *buf, uint16_t pixel_count);

/* Attach a transfer backend. backend->pixel_data receives the 24bit GRB data and
 * backend->timing.words receives the timing words; both are caller-owned. Expansion,
 * busy checks, settle_ticks, timeout and frame throttling are handled by the library;
 * the backend only moves words. */
void ws2812_attach_backend(WS2812 *handle, WS2812Backend *backend);

/* Attach/detach event callbacks; call before ws2812_start(). */
void ws2812_attach(WS2812 *handle, WS2812Event event, WS2812Callback cb, void *user_data);
void ws2812_detach(WS2812 *handle, WS2812Event event);

/* Add to / remove from the work list.
 * Returns 0 on success, WS2812_ERR_BUSY if already registered,
 * WS2812_ERR_ARG for invalid arguments. */
int  ws2812_start(WS2812 *handle);
void ws2812_stop(WS2812 *handle);

/* Background heartbeat, called every 1ms; steps every strip in the work list. */
void ws2812_ticks(void);

/* Advance ONE strip by one tick: settle/busy/timeout tracking, frame throttle,
 * render, refresh. Called by ws2812_ticks(); public for step debugging/tests. */
void ws2812_step(WS2812 *handle);

/* ---- Configuration ---- */

/* Channel order on the wire (GRB/BGR/...). Caller colors stay 0xRRGGBB. */
void ws2812_set_order(WS2812 *handle, WS2812Order channel_order);
void ws2812_set_brightness(WS2812 *handle, uint8_t brightness);
/* Enable/disable the gamma 2.2 LUT (0 off / 1 on); not a gamma value. */
void ws2812_set_gamma(WS2812 *handle, uint8_t enable);
void ws2812_set_frame_ms(WS2812 *handle, uint16_t ms);   /* 0 is treated as 1 tick */
void ws2812_set_period_ms(WS2812 *handle, uint16_t ms);

/* ---- Direct drawing ---- */

void     ws2812_set_pixel(WS2812 *handle, uint16_t index, uint32_t rgb);
uint32_t ws2812_get_pixel(const WS2812 *handle, uint16_t index);
void     ws2812_fill(WS2812 *handle, uint32_t rgb);
void     ws2812_clear(WS2812 *handle);

/* Encode and output one frame immediately, WITHOUT advancing the animation.
 * Returns 0 on success, otherwise the backend's error code. */
int  ws2812_refresh(WS2812 *handle);

/* ---- Effects ---- */

void ws2812_set_mode(WS2812 *handle, WS2812Mode mode);
/* Base color for the active effect. To set one LED, use ws2812_set_pixel(). */
void ws2812_set_color(WS2812 *handle, uint32_t rgb);
WS2812Mode ws2812_get_mode(const WS2812 *handle);
uint32_t   ws2812_get_color(const WS2812 *handle);

/* Restart the effect: zero phase/hue, force a redraw on the next tick.
 * Does NOT touch the frame buffer, hardware, or RES timing; use ws2812_clear()
 * to turn LEDs off. */
void ws2812_restart_effect(WS2812 *handle);

/* ---- Utilities ---- */

/* Pure conversion: HSV(0..1535, 0..255, 0..255) -> 0xRRGGBB. No strip state. */
uint32_t ws2812_hsv(uint16_t hue, uint8_t sat, uint8_t val);

/* Build timing->words from 24bit GRB data: each bit becomes t0h or t1h,
 * MSB first, framed by RES words:
 *   [res_lead x res_level] [8 words per byte] [res_trail x res_level]
 *
 * Returns the number of words written, or 0 if an argument is invalid, the
 * buffer is too small, or t0h == t1h. */
uint16_t ws2812_build_timing_words(WS2812BitTiming *timing, const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* WS2812_H */
