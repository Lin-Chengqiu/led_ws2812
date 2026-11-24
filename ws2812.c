/*
 * ws2812.c - Non-blocking WS2812 driver and animation state machine
 *
 * SPDX-License-Identifier: MIT
 */

#include "ws2812.h"
#include <string.h>

/* ============================ Internal flags ============================ */

#define WS2812_FLAG_GAMMA 0x01u /* Apply gamma 2.2 at refresh */
#define WS2812_FLAG_DIRTY 0x02u /* Static effect needs one redraw */
#define WS2812_FLAG_ON    0x04u /* Blink effect is in the "on" half-period */

/* ============================ Internal data ============================ */

/* Head of the work list */
static WS2812 *head_handle = NULL;

/* Destination byte index for each of R, G, B in the pixel_data channel_order */
static const uint8_t s_order_map[WS2812_ORDER_COUNT][3] = {
	{ 0, 1, 2 }, /* RGB : r g b */
	{ 1, 0, 2 }, /* GRB : g r b  (most common for WS2812B) */
	{ 1, 2, 0 }, /* BRG : b r g */
	{ 0, 2, 1 }, /* RBG : r b g */
	{ 2, 0, 1 }, /* GBR : g b r */
	{ 2, 1, 0 }  /* BGR : b g r  (common for SK6812) */
};

#if WS2812_USE_GAMMA
/* gamma 2.2: out = round(255 * (in/255)^2.2), 256 bytes of Flash */
static const uint8_t s_gamma_lut[256] = {
	  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
	  0,   0,   0,   1,   1,   1,   1,   1,   1,   1,   1,   1,
	  1,   2,   2,   2,   2,   2,   2,   2,   3,   3,   3,   3,
	  3,   4,   4,   4,   4,   5,   5,   5,   5,   6,   6,   6,
	  6,   7,   7,   7,   8,   8,   8,   9,   9,   9,  10,  10,
	 11,  11,  11,  12,  12,  13,  13,  13,  14,  14,  15,  15,
	 16,  16,  17,  17,  18,  18,  19,  19,  20,  20,  21,  22,
	 22,  23,  23,  24,  25,  25,  26,  26,  27,  28,  28,  29,
	 30,  30,  31,  32,  33,  33,  34,  35,  35,  36,  37,  38,
	 39,  39,  40,  41,  42,  43,  43,  44,  45,  46,  47,  48,
	 49,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,
	 60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,
	 73,  74,  75,  76,  77,  78,  79,  81,  82,  83,  84,  85,
	 87,  88,  89,  90,  91,  93,  94,  95,  97,  98,  99, 100,
	102, 103, 105, 106, 107, 109, 110, 111, 113, 114, 116, 117,
	119, 120, 121, 123, 124, 126, 127, 129, 130, 132, 133, 135,
	137, 138, 140, 141, 143, 145, 146, 148, 149, 151, 153, 154,
	156, 158, 159, 161, 163, 165, 166, 168, 170, 172, 173, 175,
	177, 179, 181, 182, 184, 186, 188, 190, 192, 194, 196, 197,
	199, 201, 203, 205, 207, 209, 211, 213, 215, 217, 219, 221,
	223, 225, 227, 229, 231, 234, 236, 238, 240, 242, 244, 246,
	248, 251, 253, 255
};
#endif

/* ============================ Forward declarations ============================ */

static void ws2812_advance(WS2812 *handle);
static void ws2812_render(WS2812 *handle);
static int  ws2812_refresh_frame(WS2812 *handle);
static void ws2812_emit(WS2812 *handle, WS2812Event event);
static int  ws2812_in_list(WS2812 *handle);
static void ws2812_fill_scaled(WS2812 *handle, uint32_t rgb, uint16_t level);
static uint16_t ws2812_breath_level(uint16_t phase, uint16_t period);
static inline uint16_t ws2812_ms_to_ticks(uint16_t ms);

/* ============================ Lifecycle ============================ */

/**
 * @brief  Initialize a strip handle
 * @param  handle: statically allocated handle; must outlive start/stop
 * @param  buf: frame buffer, at least pixel_count * 3 bytes
 * @param  pixel_count: number of LEDs
 */
void ws2812_init(WS2812 *handle, uint8_t *buf, uint16_t pixel_count)
{
	if (!handle) return;

	memset(handle, 0, sizeof(WS2812));
	handle->buf = buf;
	handle->pixel_count = pixel_count;
	handle->channel_order = WS2812_ORDER_GRB;
	handle->brightness = 255;
	handle->mode = (uint8_t)WS2812_MODE_MANUAL;
	handle->color = 0xFFFFFFu;
	handle->frame_ticks = ws2812_ms_to_ticks(WS2812_DEFAULT_FRAME_MS);
	handle->period_ticks = ws2812_ms_to_ticks(WS2812_DEFAULT_PERIOD_MS);
#if WS2812_USE_GAMMA
	handle->flag = WS2812_FLAG_GAMMA;
#endif
	if (buf && pixel_count && (uint32_t)pixel_count * WS2812_BYTES_PER_LED <= WS2812_MAX_BYTES) {
		/* Clear only for a valid length, so a bad pixel_count cannot overrun the buffer */
		memset(buf, 0, (size_t)pixel_count * WS2812_BYTES_PER_LED);
	}
}

/**
 * @brief  Attach the low-level writer
 * @param  tx: encoded buffer, at least pixel_count*3 bytes; NULL encodes in place
 */
void ws2812_attach_backend(WS2812 *handle, WS2812Backend *backend)
{
	if (!handle || !backend) return;
	handle->backend = backend;
}

void ws2812_attach(WS2812 *handle, WS2812Event event, WS2812Callback cb, void *user_data)
{
	if (!handle || event >= WS2812_EVENT_COUNT) return;
	handle->cb[event] = cb;
	handle->user_data = user_data;
}

void ws2812_detach(WS2812 *handle, WS2812Event event)
{
	if (!handle || event >= WS2812_EVENT_COUNT) return;
	handle->cb[event] = NULL;
}

/**
 * @brief  Start a strip: add it to the work list
 * @retval 0 success, -1 already registered, -2 invalid argument
 */
int ws2812_start(WS2812 *handle)
{
	WS2812 *target;

	if (!handle) return WS2812_ERR_ARG;
	if (!handle->backend || !handle->buf || handle->pixel_count == 0) return WS2812_ERR_ARG;
	if (!handle->backend->pixel_data || !handle->backend->transmit || !handle->backend->is_busy) {
		return WS2812_ERR_ARG;
	}
	if ((uint32_t)handle->pixel_count * WS2812_BYTES_PER_LED > WS2812_MAX_BYTES) return WS2812_ERR_ARG;

	WS2812_LOCK();
	for (target = head_handle; target != NULL; target = target->next) {
		if (target == handle) {
			WS2812_UNLOCK();
			return WS2812_ERR_BUSY;
		}
	}
	handle->next = head_handle;
	head_handle = handle;
	WS2812_UNLOCK();
	return 0;
}

/**
 * @brief  Stop a strip: remove it from the list, keep the frame buffer
 */
void ws2812_stop(WS2812 *handle)
{
	WS2812 **curr;

	if (!handle) return;

	WS2812_LOCK();
	for (curr = &head_handle; *curr != NULL; ) {
		WS2812 *entry = *curr;
		if (entry == handle) {
			*curr = entry->next;
			entry->next = NULL;
			WS2812_UNLOCK();
			return;
		}
		curr = &entry->next;
	}
	WS2812_UNLOCK();
}

/** @brief  Check whether a handle is still in the work list */
static int ws2812_in_list(WS2812 *handle)
{
	WS2812 *target;
	int found = 0;

	if (handle == NULL) return 0;

	WS2812_LOCK();
	for (target = head_handle; target != NULL; target = target->next) {
		if (target == handle) {
			found = 1;
			break;
		}
	}
	WS2812_UNLOCK();
	return found;
}

/**
 * @brief  Background heartbeat, called by a 1ms timer or low-priority task
 */
void ws2812_ticks(void)
{
	WS2812 *target;
	WS2812 *next;

	WS2812_LOCK();
	target = head_handle;
	WS2812_UNLOCK();

	while (target != NULL) {
		WS2812_LOCK();
		next = target->next;
		WS2812_UNLOCK();

		ws2812_step(target);

		/* A callback may have stopped next; verify it is still registered */
		if (next == NULL || !ws2812_in_list(next)) {
			break;
		}
		target = next;
	}
}

/**
 * @brief  Advance one strip: throttle -> phase -> render -> refresh
 */
void ws2812_step(WS2812 *handle)
{
	int ret;

	if (!handle) return;
	if (handle->frame_ticks == 0) return;

	if (handle->backend != NULL && handle->backend->running) {
		WS2812Backend *be = handle->backend;

		if (be->settle_left > 0u) {
			be->settle_left--;  /* Start-up protection: do not query yet */
		} else if (be->is_busy(handle) == 0) {
			be->running = 0;   /* Transfer finished */
		} else if (be->timeout_left > 0u) {
			be->timeout_left--;
			if (be->timeout_left == 0u) {
				/* Stuck transfer: force recovery instead of freezing forever */
				be->running = 0;
				ws2812_emit(handle, WS2812_EVENT_ERROR);
			}
		}
	}

	if (handle->ticks < UINT16_MAX) {
		handle->ticks++;
	}
	if (handle->ticks < handle->frame_ticks) {
		return; /* Frame throttling: not time to refresh yet */
	}

	if (handle->mode != (uint8_t)WS2812_MODE_MANUAL) {
		ws2812_advance(handle);
	}
	ws2812_render(handle);

	ret = ws2812_refresh_frame(handle);
	if (ret == 0) {
		handle->ticks = 0; /* Reset only on success; retry on the next tick otherwise */
	}
}

/* ============================ Configuration ============================ */

void ws2812_set_order(WS2812 *handle, WS2812Order channel_order)
{
	if (!handle || channel_order >= WS2812_ORDER_COUNT) return;
	handle->channel_order = (uint8_t)channel_order;
	handle->flag |= WS2812_FLAG_DIRTY;
}

void ws2812_set_brightness(WS2812 *handle, uint8_t brightness)
{
	if (!handle) return;
	handle->brightness = brightness;
	handle->flag |= WS2812_FLAG_DIRTY;
}

void ws2812_set_gamma(WS2812 *handle, uint8_t enable)
{
	if (!handle) return;
#if WS2812_USE_GAMMA
	if (enable) handle->flag |= WS2812_FLAG_GAMMA;
	else        handle->flag &= (uint8_t)~WS2812_FLAG_GAMMA;
#else
	(void)enable;
#endif
}

void ws2812_set_frame_ms(WS2812 *handle, uint16_t ms)
{
	if (!handle) return;
	handle->frame_ticks = ws2812_ms_to_ticks(ms);
}

void ws2812_set_period_ms(WS2812 *handle, uint16_t ms)
{
	if (!handle) return;
	handle->period_ticks = ws2812_ms_to_ticks(ms);
	if (handle->phase >= handle->period_ticks) {
		handle->phase = 0;
	}
	handle->flag |= WS2812_FLAG_DIRTY;
}

/* ============================ Direct drawing ============================ */

void ws2812_set_pixel(WS2812 *handle, uint16_t index, uint32_t rgb)
{
	uint8_t *p;

	if (!handle || !handle->buf || index >= handle->pixel_count) return;

	p = &handle->buf[(uint32_t)index * WS2812_BYTES_PER_LED];
	p[0] = (uint8_t)((rgb >> 16) & 0xFFu);
	p[1] = (uint8_t)((rgb >> 8) & 0xFFu);
	p[2] = (uint8_t)(rgb & 0xFFu);
}

uint32_t ws2812_get_pixel(const WS2812 *handle, uint16_t index)
{
	const uint8_t *p;

	if (!handle || !handle->buf || index >= handle->pixel_count) return 0;

	p = &handle->buf[(uint32_t)index * WS2812_BYTES_PER_LED];
	return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

void ws2812_fill(WS2812 *handle, uint32_t rgb)
{
	uint16_t i;

	if (!handle || !handle->buf) return;
	for (i = 0; i < handle->pixel_count; i++) {
		ws2812_set_pixel(handle, i, rgb);
	}
}

void ws2812_clear(WS2812 *handle)
{
	ws2812_fill(handle, 0);
}

/**
 * @brief  Encode and output one frame without advancing the animation
 * @retval 0 success, otherwise the transmit callback's error code
 */
int ws2812_refresh(WS2812 *handle)
{
	if (!handle) return WS2812_ERR_ARG;
	return ws2812_refresh_frame(handle);
}

/* ============================ Effects ============================ */

void ws2812_set_mode(WS2812 *handle, WS2812Mode mode)
{
	if (!handle || mode >= WS2812_MODE_COUNT) return;
	if (handle->mode == (uint8_t)mode) return;

	handle->mode = (uint8_t)mode;
	handle->phase = 0;
	handle->hue = 0;
	handle->flag |= WS2812_FLAG_DIRTY;
	handle->flag &= (uint8_t)~WS2812_FLAG_ON;
	/* Make the mode change visible on the next tick, not after a full frame interval */
	handle->ticks = handle->frame_ticks;
}

void ws2812_set_color(WS2812 *handle, uint32_t rgb)
{
	if (!handle) return;
	handle->color = rgb & 0xFFFFFFu;
	handle->flag |= WS2812_FLAG_DIRTY;
}

WS2812Mode ws2812_get_mode(const WS2812 *handle)
{
	if (!handle) return WS2812_MODE_MANUAL;
	return (WS2812Mode)handle->mode;
}

uint32_t ws2812_get_color(const WS2812 *handle)
{
	if (!handle) return 0;
	return handle->color;
}

void ws2812_restart_effect(WS2812 *handle)
{
	if (!handle) return;
	handle->ticks = handle->frame_ticks;
	handle->phase = 0;
	handle->hue = 0;
	handle->flag |= WS2812_FLAG_DIRTY;
	handle->flag &= (uint8_t)~WS2812_FLAG_ON;
}

/**
 * @brief  HSV -> 0xRRGGBB; hue 0..1535 maps to 0..360 degrees
 */
uint32_t ws2812_hsv(uint16_t hue, uint8_t sat, uint8_t val)
{
	uint32_t seg = (uint32_t)(hue / 256u) % 6u;
	uint32_t f = (uint32_t)(hue % 256u);
	uint32_t v = val;
	uint32_t p = (v * (255u - sat)) / 255u;
	uint32_t q = (v * (255u - (uint32_t)sat * f / 255u)) / 255u;
	uint32_t t = (v * (255u - (uint32_t)sat * (255u - f) / 255u)) / 255u;
	uint32_t r, g, b;

	switch (seg) {
	case 0:  r = v; g = t; b = p; break;
	case 1:  r = q; g = v; b = p; break;
	case 2:  r = p; g = v; b = t; break;
	case 3:  r = p; g = q; b = v; break;
	case 4:  r = t; g = p; b = v; break;
	default: r = v; g = p; b = q; break;
	}

	return (r << 16) | (g << 8) | b;
}

/* ============================ Internal implementation ============================ */

/** ms -> ticks, rounded to the nearest tick, at least one tick */
static inline uint16_t ws2812_ms_to_ticks(uint16_t ms)
{
	uint32_t ticks = (uint32_t)ms + (WS2812_TICKS_INTERVAL / 2u);

	ticks /= WS2812_TICKS_INTERVAL;
	if (ticks == 0) ticks = 1;
	if (ticks > UINT16_MAX) ticks = UINT16_MAX;
	return (uint16_t)ticks;
}

/**
 * @brief  Advance the timeline: phase, period wrap and rainbow hue sync
 */
static void ws2812_advance(WS2812 *handle)
{
	uint16_t period = handle->period_ticks ? handle->period_ticks : 1;

	{
		/* Subtract whole periods before uint16 truncation, or long periods
		 * would wrap silently and miss the CYCLE event */
		uint32_t phase = (uint32_t)handle->phase + handle->frame_ticks;

		while (phase >= period) {
			phase -= period;
			ws2812_emit(handle, WS2812_EVENT_CYCLE);
		}
		handle->phase = (uint16_t)phase;
	}

	if (handle->mode == (uint8_t)WS2812_MODE_RAINBOW) {
		/* Derive hue from phase so long runs cannot drift */
		handle->hue = (uint16_t)(((uint32_t)handle->phase * 1536u) / period);
	}
}

/**
 * @brief  Render one frame into the frame buffer for the current mode
 */
static void ws2812_render(WS2812 *handle)
{
	uint8_t dirty;
	uint32_t color;
	uint16_t period;
	uint16_t i;

	if (!handle->buf || handle->pixel_count == 0) return;

	dirty = (handle->flag & WS2812_FLAG_DIRTY) ? 1u : 0u;
	color = handle->color & 0xFFFFFFu;
	period = handle->period_ticks ? handle->period_ticks : 1;

	switch ((WS2812Mode)handle->mode) {
	case WS2812_MODE_OFF:
		if (dirty) {
			ws2812_fill(handle, 0);
			handle->flag &= (uint8_t)~WS2812_FLAG_DIRTY;
		}
		break;

	case WS2812_MODE_SOLID:
		if (dirty) {
			ws2812_fill(handle, color);
			handle->flag &= (uint8_t)~WS2812_FLAG_DIRTY;
		}
		break;

	case WS2812_MODE_BLINK: {
		uint8_t on = (handle->phase < (period / 2u)) ? 1u : 0u;
		uint8_t was_on = (handle->flag & WS2812_FLAG_ON) ? 1u : 0u;

		if (dirty || on != was_on) {
			ws2812_fill(handle, on ? color : 0u);
			if (on) handle->flag |= WS2812_FLAG_ON;
			else    handle->flag &= (uint8_t)~WS2812_FLAG_ON;
			handle->flag &= (uint8_t)~WS2812_FLAG_DIRTY;
		}
		break;
	}

	case WS2812_MODE_BREATH:
		ws2812_fill_scaled(handle, color, ws2812_breath_level(handle->phase, period));
		handle->flag &= (uint8_t)~WS2812_FLAG_DIRTY;
		break;

	case WS2812_MODE_RAINBOW:
		for (i = 0; i < handle->pixel_count; i++) {
			uint16_t h = (uint16_t)(((uint32_t)handle->hue +
			                         (uint32_t)i * 1536u / handle->pixel_count) % 1536u);
			ws2812_set_pixel(handle, i, ws2812_hsv(h, 255, 255));
		}
		handle->flag &= (uint8_t)~WS2812_FLAG_DIRTY;
		break;

	case WS2812_MODE_CHASE: {
		uint16_t pos = (uint16_t)(((uint32_t)handle->phase * handle->pixel_count) / period);
		int32_t k;

		ws2812_fill(handle, 0);
		for (k = 0; k < WS2812_CHASE_TAIL; k++) {
			int32_t idx = (int32_t)pos - k;
			uint16_t level;

			if (idx < 0) idx += handle->pixel_count;
			level = (k >= 8) ? 0u : (uint16_t)(255u >> k);
			ws2812_set_pixel(handle, (uint16_t)idx,
			                 ((uint32_t)(((color >> 16) & 0xFFu) * level / 255u) << 16) |
			                 ((uint32_t)(((color >> 8) & 0xFFu) * level / 255u) << 8) |
			                 ((uint32_t)((color & 0xFFu) * level / 255u)));
		}
		handle->flag &= (uint8_t)~WS2812_FLAG_DIRTY;
		break;
	}

	case WS2812_MODE_MANUAL:
	case WS2812_MODE_COUNT: /* Treat invalid values as MANUAL */
	default:
		/* The frame buffer belongs to the caller; do not touch it */
		break;
	}
}

/**
 * @brief  Encode and output one frame:
 *         reorder channels, apply master brightness, then gamma correction
 * @retval 0 success, otherwise the transmit callback's error code
 */
static int ws2812_refresh_frame(WS2812 *handle)
{
	const uint8_t *map;
	uint8_t *dst;
	uint8_t bri;
	uint8_t gamma;
	uint16_t len;
	uint16_t i;
	uint16_t n;
	int ret;

	if (!handle || !handle->buf || handle->pixel_count == 0) return WS2812_ERR_ARG;
	if (!handle->backend || !handle->backend->pixel_data) return WS2812_ERR_ARG;

	map = s_order_map[handle->channel_order % WS2812_ORDER_COUNT];
	dst = handle->backend->pixel_data;
	bri = handle->brightness;
	gamma = (handle->flag & WS2812_FLAG_GAMMA) ? 1u : 0u;
	len = (uint16_t)((uint32_t)handle->pixel_count * WS2812_BYTES_PER_LED);

	for (i = 0; i < handle->pixel_count; i++) {
		const uint8_t *src = &handle->buf[(uint32_t)i * WS2812_BYTES_PER_LED];
		uint8_t v[3];
		uint8_t *p = &dst[(uint32_t)i * WS2812_BYTES_PER_LED];

		v[0] = (uint8_t)((uint32_t)src[0] * bri / 255u);
		v[1] = (uint8_t)((uint32_t)src[1] * bri / 255u);
		v[2] = (uint8_t)((uint32_t)src[2] * bri / 255u);
#if WS2812_USE_GAMMA
		if (gamma) {
			v[0] = s_gamma_lut[v[0]];
			v[1] = s_gamma_lut[v[1]];
			v[2] = s_gamma_lut[v[2]];
		}
#else
		(void)gamma;
#endif
		p[map[0]] = v[0];
		p[map[1]] = v[1];
		p[map[2]] = v[2];
	}

	{
		WS2812Backend *be = handle->backend;

		/* All decisions live here, not in the hardware hook */
		if (!be->transmit || !be->is_busy) return WS2812_ERR_ARG;
		/* Busy state is owned by the backend callback, not a parallel flag.
		 * settle_left only covers the start-up window before the hardware
		 * status is meaningful. */
		if (be->settle_left > 0u || be->is_busy(handle) != 0) return WS2812_ERR_BUSY;
		n = ws2812_build_timing_words(&be->timing, dst, len);
		/* Guarantee the backend never sees a NULL pointer or too few words
		 * for two-stage preload (words[0], words[1]). */
		if (n < 3u) return WS2812_ERR_ARG;

		ret = be->transmit(handle, be->timing.words, n);
		if (ret == 0) {
			be->running = 1;
			be->settle_left = be->settle_ticks;
			be->timeout_left = be->timeout;
		}
	}
	ws2812_emit(handle, (ret == 0) ? WS2812_EVENT_FRAME : WS2812_EVENT_ERROR);
	return ret;
}

/** Fill the whole strip with a scaled color, used by the breath effect */
static void ws2812_fill_scaled(WS2812 *handle, uint32_t rgb, uint16_t level)
{
	uint16_t i;

	if (!handle || !handle->buf) return;
	for (i = 0; i < handle->pixel_count; i++) {
		ws2812_set_pixel(handle, i,
		                 ((uint32_t)(((rgb >> 16) & 0xFFu) * level / 255u) << 16) |
		                 ((uint32_t)(((rgb >> 8) & 0xFFu) * level / 255u) << 8) |
		                 ((uint32_t)((rgb & 0xFFu) * level / 255u)));
	}
}

/**
 * @brief  Breathing curve: 0 -> 255 -> 0 within one period,
 *         squared for perceptual linearity
 */
static uint16_t ws2812_breath_level(uint16_t phase, uint16_t period)
{
	uint32_t half = period / 2u;
	uint32_t t;

	if (half == 0) return 255;
	if (phase < half) {
		t = (uint32_t)phase * 255u / half;
	} else {
		t = (uint32_t)(period - phase) * 255u / half;
	}
	return (uint16_t)((t * t) / 255u);
}

/**
 * @brief  Build timing words from 24bit GRB data: one t0h/t1h per bit,
 *         MSB first, plus leading and trailing RES words
 * @retval number of words written, or 0 on invalid input / insufficient space
 */
uint16_t ws2812_build_timing_words(WS2812BitTiming *timing, const uint8_t *data, uint16_t len)
{
	uint32_t needed;
	uint16_t n = 0;
	uint16_t i;
	uint8_t bit;

	if (!timing || !timing->words || !data || len == 0u) return 0;
	if (timing->t0h == timing->t1h) return 0;

	needed = (uint32_t)timing->res_lead + (uint32_t)len * 8u + timing->res_trail;
	if (needed == 0u || needed > timing->word_cap || needed > UINT16_MAX) return 0;

	for (i = 0; i < timing->res_lead; i++) {
		timing->words[n++] = timing->res_level;
	}
	for (i = 0; i < len; i++) {
		uint8_t byte = data[i];
		for (bit = 0u; bit < 8u; bit++) {
			timing->words[n++] = (byte & (0x80u >> bit)) ? timing->t1h : timing->t0h;
		}
	}
	for (i = 0; i < timing->res_trail; i++) {
		timing->words[n++] = timing->res_level;
	}
	return n;
}

/** Dispatch a callback; silently return when none is registered */
static void ws2812_emit(WS2812 *handle, WS2812Event event)
{
	if (event >= WS2812_EVENT_COUNT) return;
	if (handle->cb[event] != NULL) {
		handle->cb[event](handle, event, handle->user_data);
	}
}
