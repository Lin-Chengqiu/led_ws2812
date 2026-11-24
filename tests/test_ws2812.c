/*
 * test_ws2812.c - Host unit tests (no MCU or peripherals required)
 *
 * Build: make test                    (from the repository root)
 *        cc -std=c99 -Wall -Wextra -I. ws2812.c tests/test_ws2812.c -o t && ./t
 *
 * Covers: channel order, brightness, gamma, frame throttling, all six
 * animation state machines, linked-list multi-instance, busy retry,
 * bounds checking and pixel_data encoding.
 */

#include <stdio.h>
#include <string.h>
#include "ws2812.h"

/* ============================ Test framework ============================ */

static int g_pass;
static int g_fail;
static int g_checks;

#define CHECK(cond) do { \
	if (cond) { g_pass++; } \
	else { g_fail++; printf("    FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
	g_checks++; \
} while (0)

#define CHECK_EQ_INT(a, b) do { \
	long _va = (long)(a), _vb = (long)(b); \
	if (_va == _vb) { g_pass++; } \
	else { g_fail++; printf("    FAIL  %s:%d  %s=%ld, expected %ld\n", \
	                        __FILE__, __LINE__, #a, _va, _vb); } \
	g_checks++; \
} while (0)

static const char *g_case = "";

static void begin(const char *name)
{
	g_case = name;
	printf("  [CASE] %s\n", name);
}

/* ============================ Mock transfer backend ============================ */

#define TEST_LEDS 8

/* Byte-level view: encoded 24bit GRB data is read back from the backend. */
static uint8_t  g_tx[512];
static uint16_t g_len;
static int      g_calls;
static int      g_busy_ret;      /* Injected transmit failure */

/* Timing-word view: what transmit() actually receives. */
static uint16_t g_tx_last[64];
static uint16_t g_tx_count;
static int      g_tx_calls;
static int      g_tx_queries;
static int      g_tx_busy_value;

static int mock_transmit(WS2812 *handle, const uint16_t *words, uint16_t count)
{
	uint16_t bytes = (uint16_t)((uint32_t)handle->pixel_count * 3u);
	uint16_t n = count;

	g_calls++;
	g_tx_calls++;
	g_tx_count = count;
	if (n > 64u) n = 64u;
	memcpy(g_tx_last, words, n * sizeof(uint16_t));

	if (g_busy_ret < 0) return g_busy_ret;
	if (bytes > sizeof(g_tx)) bytes = (uint16_t)sizeof(g_tx);
	if (handle->backend != NULL && handle->backend->pixel_data != NULL) {
		memcpy(g_tx, handle->backend->pixel_data, bytes);
	}
	g_len = bytes;
	return 0;
}

static int mock_is_busy(WS2812 *handle)
{
	(void)handle;
	g_tx_queries++;
	return g_tx_busy_value;
}

static int g_frame_cnt;
static int g_cycle_cnt;
static int g_error_cnt;

static void on_event(WS2812 *handle, WS2812Event event, void *user_data)
{
	(void)handle;
	(void)user_data;
	switch (event) {
	case WS2812_EVENT_FRAME: g_frame_cnt++; break;
	case WS2812_EVENT_CYCLE: g_cycle_cnt++; break;
	case WS2812_EVENT_ERROR: g_error_cnt++; break;
	default: break;
	}
}

/* Stop the next strip from a callback: ticks must not advance a removed handle */
static void stop_next_cb(WS2812 *handle, WS2812Event event, void *user_data)
{
	(void)handle;
	(void)event;
	ws2812_stop((WS2812 *)user_data);
}

/* ============================ Fixtures ============================ */

static WS2812 strip;
static WS2812 strip2;
static uint8_t buf[TEST_LEDS * 3];
static uint8_t tx[TEST_LEDS * 3];
static uint8_t buf2[TEST_LEDS * 3];
static uint8_t tx2[TEST_LEDS * 3];

static WS2812Backend g_be;
static WS2812Backend g_be2;
static uint16_t g_words[512];
static uint16_t g_words2[512];

/* Configure a transfer backend: 24bit GRB data + timing words. */
static void setup_backend(WS2812Backend *be, uint8_t *pixel_data, uint16_t *words)
{
	memset(be, 0, sizeof(*be));
	be->transmit = mock_transmit;
	be->is_busy = mock_is_busy;
	be->pixel_data = pixel_data;
	be->timing.words = words;
	be->timing.word_cap = 512;
	be->timing.t0h = 10;
	be->timing.t1h = 20;
	be->timing.res_level = 0;
	be->settle_ticks = 0;    /* Synchronous mock backend: no start-up window */
	be->timeout = 5;
}

static void setup(WS2812 *h, uint8_t *b, uint8_t *t, uint16_t pixel_count)
{
	g_len = 0;
	g_calls = 0;
	g_tx_calls = 0;
	g_tx_queries = 0;
	g_tx_busy_value = 0;
	g_busy_ret = 0;
	g_frame_cnt = 0;
	g_cycle_cnt = 0;
	g_error_cnt = 0;
	memset(g_tx, 0, sizeof(g_tx));
	memset(b, 0, TEST_LEDS * 3);

	ws2812_init(h, b, pixel_count);
	ws2812_set_gamma(h, 0);         /* Gamma off for byte-exact comparisons */
	ws2812_set_frame_ms(h, 1);      /* One frame per tick: 1 tick == 1ms phase */
	ws2812_set_period_ms(h, 1000);

	if (h == &strip2) {
		setup_backend(&g_be2, t, g_words2);
		ws2812_attach_backend(h, &g_be2);
	} else {
		setup_backend(&g_be, t, g_words);
		ws2812_attach_backend(h, &g_be);
	}

	ws2812_attach(h, WS2812_EVENT_FRAME, on_event, NULL);
	ws2812_attach(h, WS2812_EVENT_CYCLE, on_event, NULL);
	ws2812_attach(h, WS2812_EVENT_ERROR, on_event, NULL);
	ws2812_start(h);
}

static void teardown(WS2812 *h)
{
	ws2812_stop(h);
}

/* n handler calls == n * WS2812_TICKS_INTERVAL milliseconds */
static void run_ticks(WS2812 *h, int n)
{
	int i;
	for (i = 0; i < n; i++) {
		ws2812_step(h);
	}
}

/* Read logical RGB of LED index from the most recent frame.
 * Tests use the default GRB channel_order: g_tx[i*3+0]=G, +1=R, +2=B. */
static void get_rgb(int index, uint8_t *r, uint8_t *g, uint8_t *b)
{
	*g = g_tx[index * 3 + 0];
	*r = g_tx[index * 3 + 1];
	*b = g_tx[index * 3 + 2];
}

/* ============================ Cases ============================ */

static void test_init_defaults(void)
{
	WS2812 h;
	uint8_t b[TEST_LEDS * 3];

	begin("defaults and initialization");
	memset(&h, 0xAA, sizeof(h));
	ws2812_init(&h, b, 4);

	CHECK_EQ_INT(h.pixel_count, 4);
	CHECK_EQ_INT(h.brightness, 255);
	CHECK_EQ_INT(h.channel_order, WS2812_ORDER_GRB);
	CHECK_EQ_INT(h.mode, WS2812_MODE_MANUAL);
	CHECK_EQ_INT(h.frame_ticks, WS2812_DEFAULT_FRAME_MS / WS2812_TICKS_INTERVAL);
	CHECK_EQ_INT(h.period_ticks, WS2812_DEFAULT_PERIOD_MS / WS2812_TICKS_INTERVAL);
	CHECK(h.backend == NULL);
	CHECK(h.next == NULL);
	CHECK_EQ_INT(b[0], 0);   /* Buffer cleared at init */

	begin("start argument validation");
	CHECK_EQ_INT(ws2812_start(&h), WS2812_ERR_ARG);          /* No backend attached */
	{
		static WS2812Backend be;
		static uint8_t pixel_data[4 * 3];
		static uint16_t words[4 * 3 * 8 + 8];
		setup_backend(&be, pixel_data, words);
		ws2812_attach_backend(&h, &be);
	}
	CHECK_EQ_INT(ws2812_start(&h), 0);
	CHECK_EQ_INT(ws2812_start(&h), WS2812_ERR_BUSY);          /* Duplicate start */
	ws2812_stop(&h);
	CHECK_EQ_INT(ws2812_start(NULL), WS2812_ERR_ARG);
}

static void test_color_order(void)
{
	/* 0x123456 -> R=0x12 G=0x34 B=0x56 */
	static const uint8_t expect[WS2812_ORDER_COUNT][3] = {
		{ 0x12, 0x34, 0x56 }, /* RGB */
		{ 0x34, 0x12, 0x56 }, /* GRB */
		{ 0x56, 0x12, 0x34 }, /* BRG */
		{ 0x12, 0x56, 0x34 }, /* RBG */
		{ 0x34, 0x56, 0x12 }, /* GBR */
		{ 0x56, 0x34, 0x12 }  /* BGR */
	};
	int o;

	begin("encoding for all six channel orders");
	setup(&strip, buf, tx, 3);
	ws2812_set_pixel(&strip, 0, 0x123456);

	for (o = 0; o < WS2812_ORDER_COUNT; o++) {
		ws2812_set_order(&strip, (WS2812Order)o);
		CHECK_EQ_INT(ws2812_refresh(&strip), 0);
		CHECK_EQ_INT(g_tx[0], expect[o][0]);
		CHECK_EQ_INT(g_tx[1], expect[o][1]);
		CHECK_EQ_INT(g_tx[2], expect[o][2]);
	}
	CHECK_EQ_INT(g_len, 9);
	teardown(&strip);
}

static void test_brightness_and_gamma(void)
{
	begin("master brightness scaling");
	setup(&strip, buf, tx, 2);
	ws2812_set_pixel(&strip, 0, 0xFFFFFF);
	ws2812_set_pixel(&strip, 1, 0x804020);

	ws2812_set_brightness(&strip, 255);
	(void)ws2812_refresh(&strip);
	CHECK_EQ_INT(g_tx[0], 0xFF);

	ws2812_set_brightness(&strip, 128);
	(void)ws2812_refresh(&strip);
	CHECK_EQ_INT(g_tx[0], 128);   /* 255*128/255 */
	CHECK_EQ_INT(g_tx[1], 128);
	CHECK_EQ_INT(g_tx[2], 128);

	ws2812_set_brightness(&strip, 0);
	(void)ws2812_refresh(&strip);
	CHECK_EQ_INT(g_tx[0], 0);
	CHECK_EQ_INT(g_tx[3], 0);

	begin("gamma 2.2 lookup table");
	ws2812_set_brightness(&strip, 255);
	ws2812_set_pixel(&strip, 0, 0x808080);
	ws2812_set_gamma(&strip, 1);
	(void)ws2812_refresh(&strip);
	CHECK_EQ_INT(g_tx[0], 56);    /* gamma[128] */

	ws2812_set_pixel(&strip, 0, 0xFFFFFF);
	(void)ws2812_refresh(&strip);
	CHECK_EQ_INT(g_tx[0], 255);   /* Full scale is lossless */

	ws2812_set_pixel(&strip, 0, 0x010101);
	(void)ws2812_refresh(&strip);
	CHECK_EQ_INT(g_tx[0], 0);     /* Dark end is compressed */

	ws2812_set_gamma(&strip, 0);
	ws2812_set_pixel(&strip, 0, 0x808080);
	(void)ws2812_refresh(&strip);
	CHECK_EQ_INT(g_tx[0], 128);   /* Linear when disabled */
	teardown(&strip);
}

static void test_frame_throttle(void)
{
	begin("frame throttling: 10ms interval");
	setup(&strip, buf, tx, 1);
	ws2812_set_frame_ms(&strip, 10);
	ws2812_set_pixel(&strip, 0, 0x010000);

	run_ticks(&strip, 9);
	CHECK_EQ_INT(g_calls, 0);     /* Not yet time to refresh */
	run_ticks(&strip, 1);
	CHECK_EQ_INT(g_calls, 1);     /* Refresh on the 10th tick */
	run_ticks(&strip, 10);
	CHECK_EQ_INT(g_calls, 2);

	begin("frame interval conversion");
	ws2812_set_frame_ms(&strip, 5);
	CHECK_EQ_INT(strip.frame_ticks, 5 / WS2812_TICKS_INTERVAL);
	ws2812_set_frame_ms(&strip, 0);
	CHECK_EQ_INT(strip.frame_ticks, 1);   /* 0 is treated as one tick */
	teardown(&strip);
}

static void test_manual_mode(void)
{
	begin("MANUAL mode: direct frame buffer writes");
	setup(&strip, buf, tx, 4);
	ws2812_set_pixel(&strip, 0, 0xFF0000);
	ws2812_set_pixel(&strip, 3, 0x0000FF);
	CHECK_EQ_INT(ws2812_get_pixel(&strip, 0), 0xFF0000);
	CHECK_EQ_INT(ws2812_get_pixel(&strip, 3), 0x0000FF);

	(void)ws2812_refresh(&strip);
	{
		uint8_t r, g, b;
		get_rgb(0, &r, &g, &b);
		CHECK_EQ_INT(r, 0xFF); CHECK_EQ_INT(g, 0x00); CHECK_EQ_INT(b, 0x00);
		get_rgb(1, &r, &g, &b);
		CHECK_EQ_INT(r, 0x00); CHECK_EQ_INT(g, 0x00); CHECK_EQ_INT(b, 0x00);
		get_rgb(3, &r, &g, &b);
		CHECK_EQ_INT(b, 0xFF);
	}
	CHECK_EQ_INT(buf[0], 0xFF);   /* With a separate tx buffer, RGB stays intact */
	teardown(&strip);
}

static void test_wire_encoding(void)
{
	begin("pixel_data encoding: backend buffer receives GRB bytes");
	setup(&strip, buf, tx, 2);
	ws2812_set_pixel(&strip, 0, 0x123456);
	(void)ws2812_refresh(&strip);
	CHECK_EQ_INT(tx[0], 0x34);    /* Reordered to GRB in the wire buffer */
	CHECK_EQ_INT(tx[1], 0x12);
	CHECK_EQ_INT(tx[2], 0x56);
	CHECK_EQ_INT(buf[0], 0x12);   /* Logical RGB buffer stays readable */
	CHECK_EQ_INT(buf[1], 0x34);
	CHECK_EQ_INT(buf[2], 0x56);
	teardown(&strip);
}

static void test_solid_mode(void)
{
	begin("SOLID: constant color");
	setup(&strip, buf, tx, 4);
	ws2812_set_color(&strip, 0x00FF00);
	ws2812_set_mode(&strip, WS2812_MODE_SOLID);

	run_ticks(&strip, 1);          /* Mode change shows on the next tick */
	CHECK_EQ_INT(g_calls, 1);
	CHECK_EQ_INT(g_len, 12);
	{
		uint8_t r, g, b;
		int i;
		for (i = 0; i < 4; i++) {
			get_rgb(i, &r, &g, &b);
			CHECK_EQ_INT(r, 0x00);
			CHECK_EQ_INT(g, 0xFF);
			CHECK_EQ_INT(b, 0x00);
		}
	}
	CHECK_EQ_INT(buf[0], 0x00);    /* Logical buffer is still RGB */
	CHECK_EQ_INT(buf[1], 0xFF);
	teardown(&strip);
}

static void test_blink_mode(void)
{
	uint8_t r, g, b;

	begin("BLINK: on for the first half, off for the second");
	setup(&strip, buf, tx, 2);
	ws2812_set_color(&strip, 0xFFFFFF);
	ws2812_set_period_ms(&strip, 1000);
	ws2812_set_mode(&strip, WS2812_MODE_BLINK);

	run_ticks(&strip, 1);          /* Phase 1ms -> on */
	get_rgb(0, &r, &g, &b);
	CHECK_EQ_INT(r, 0xFF);

	run_ticks(&strip, 498);        /* Phase 499ms: still on */
	get_rgb(0, &r, &g, &b);
	CHECK_EQ_INT(r, 0xFF);

	run_ticks(&strip, 1);          /* Phase 500ms -> off */
	get_rgb(0, &r, &g, &b);
	CHECK_EQ_INT(r, 0x00);

	run_ticks(&strip, 500);        /* Phase 1000ms -> wrap to 0, on */
	get_rgb(0, &r, &g, &b);
	CHECK_EQ_INT(r, 0xFF);
	teardown(&strip);
}

static void test_breath_mode(void)
{
	uint8_t r, g, b;

	begin("BREATH: peak at mid-period, dark at both ends");
	setup(&strip, buf, tx, 2);
	ws2812_set_color(&strip, 0xFFFFFF);
	ws2812_set_period_ms(&strip, 1000);
	ws2812_set_mode(&strip, WS2812_MODE_BREATH);

	run_ticks(&strip, 500);        /* Phase 500ms = peak */
	get_rgb(0, &r, &g, &b);
	CHECK_EQ_INT(r, 255);
	CHECK_EQ_INT(g, 255);
	CHECK_EQ_INT(b, 255);

	run_ticks(&strip, 200);        /* Phase 700ms, past the peak */
	get_rgb(0, &r, &g, &b);
	{
		uint8_t mid = r;
		CHECK(mid > 0 && mid < 255);

		run_ticks(&strip, 300);    /* Phase 1000ms -> wrap to 0 */
		get_rgb(0, &r, &g, &b);
		CHECK(r <= 3);             /* Trough is nearly off */
	}

	/* Symmetry: brightness at phase 200ms and 800ms must match */
	{
		uint8_t a, c;
		ws2812_restart_effect(&strip);
		run_ticks(&strip, 200);    /* 200ms */
		get_rgb(0, &a, &g, &b);
		ws2812_restart_effect(&strip);
		run_ticks(&strip, 800);    /* 800ms */
		get_rgb(0, &c, &g, &b);
		CHECK_EQ_INT(a, c);
	}
	teardown(&strip);
}

static void test_rainbow_mode(void)
{
	uint8_t r0, g0, b0;
	uint8_t r1, g1, b1;

	begin("RAINBOW: hue rotation and per-LED gradient");
	setup(&strip, buf, tx, 4);
	ws2812_set_period_ms(&strip, 1000);
	ws2812_set_mode(&strip, WS2812_MODE_RAINBOW);

	run_ticks(&strip, 1);          /* Phase 1ms, hue ~ red */
	get_rgb(0, &r0, &g0, &b0);
	CHECK(r0 > 200);
	CHECK(g0 < 40);

	get_rgb(3, &r1, &g1, &b1);    /* A gradient along the strip */
	CHECK(!(r0 == r1 && g0 == g1 && b0 == b1));

	run_ticks(&strip, 249);        /* Phase 250ms, hue moved to yellow-green */
	get_rgb(0, &r1, &g1, &b1);
	CHECK(!(r0 == r1 && g0 == g1 && b0 == b1));
	CHECK(g1 > g0);

	/* Hue returns to its start after a full period */
	{
		uint8_t r2, g2, b2;
		ws2812_restart_effect(&strip);
		run_ticks(&strip, 1);
		get_rgb(0, &r0, &g0, &b0);
		run_ticks(&strip, 1000);   /* +1000ms, one full period */
		get_rgb(0, &r2, &g2, &b2);
		CHECK_EQ_INT(r2, r0);
		CHECK_EQ_INT(g2, g0);
		CHECK_EQ_INT(b2, b0);
	}
	teardown(&strip);
}

static void test_chase_mode(void)
{
	uint8_t r, g, b;
	int head0 = -1, head_mid = -1;
	int i;

	begin("CHASE: highlight advances with time");
	setup(&strip, buf, tx, 8);
	ws2812_set_color(&strip, 0x0000FF);
	ws2812_set_period_ms(&strip, 1000);
	ws2812_set_mode(&strip, WS2812_MODE_CHASE);

	run_ticks(&strip, 1);
	for (i = 0; i < 8; i++) {
		get_rgb(i, &r, &g, &b);
		if (b > 200) { head0 = i; break; }
	}
	CHECK(head0 == 0);

	run_ticks(&strip, 499);        /* Phase 500ms -> halfway */
	for (i = 0; i < 8; i++) {
		get_rgb(i, &r, &g, &b);
		if (b > 200) { head_mid = i; break; }
	}
	CHECK_EQ_INT(head_mid, 4);
	teardown(&strip);
}

static void test_cycle_event(void)
{
	begin("CYCLE: fired once per animation period");
	setup(&strip, buf, tx, 2);
	ws2812_set_color(&strip, 0xFFFFFF);
	ws2812_set_period_ms(&strip, 1000);
	ws2812_set_mode(&strip, WS2812_MODE_BLINK);

	CHECK_EQ_INT(g_cycle_cnt, 0);
	run_ticks(&strip, 1000);       /* 1000ms = one period */
	CHECK_EQ_INT(g_cycle_cnt, 1);
	run_ticks(&strip, 1000);
	CHECK_EQ_INT(g_cycle_cnt, 2);
	run_ticks(&strip, 1500);       /* 1.5 periods */
	CHECK_EQ_INT(g_cycle_cnt, 3);
	teardown(&strip);
}

static void test_multi_instance(void)
{
	begin("multi-instance: one ticks drives two strips");
	setup(&strip, buf, tx, 2);
	ws2812_init(&strip2, buf2, 2);
	ws2812_set_gamma(&strip2, 0);
	ws2812_set_frame_ms(&strip2, 10);
	setup_backend(&g_be2, tx2, g_words2);
	ws2812_attach_backend(&strip2, &g_be2);
	CHECK_EQ_INT(ws2812_start(&strip2), 0);

	ws2812_set_color(&strip, 0xFF0000);
	ws2812_set_mode(&strip, WS2812_MODE_SOLID);
	ws2812_set_color(&strip2, 0x0000FF);
	ws2812_set_mode(&strip2, WS2812_MODE_SOLID);

	ws2812_ticks();                /* One heartbeat refreshes both */
	CHECK_EQ_INT(g_calls, 2);
	CHECK_EQ_INT(tx[1], 0xFF);     /* strip: in GRB, R is the 2nd byte */
	CHECK_EQ_INT(tx[0], 0x00);
	CHECK_EQ_INT(tx2[2], 0xFF);    /* strip2: B is the 3rd byte in GRB */

	teardown(&strip2);
	teardown(&strip);
}

static void test_stop(void)
{
	int before;
	int i;

	begin("no refresh after stop");
	setup(&strip, buf, tx, 2);
	ws2812_set_color(&strip, 0xFFFFFF);
	ws2812_set_mode(&strip, WS2812_MODE_SOLID);
	ws2812_ticks();
	CHECK_EQ_INT(g_calls, 1);

	ws2812_stop(&strip);
	before = g_calls;
	for (i = 0; i < 20; i++) {
		ws2812_ticks();            /* Strip is no longer in the list */
	}
	CHECK_EQ_INT(g_calls, before);

	/* Re-register and resume */
	CHECK_EQ_INT(ws2812_start(&strip), 0);
	for (i = 0; i < 3; i++) {
		ws2812_ticks();
	}
	CHECK_EQ_INT(g_calls, before + 3);
	teardown(&strip);
}

static void test_long_period_wrap(void)
{
	begin("long period wrap: 65535ms must not lose CYCLE at uint16 boundary");
	setup(&strip, buf, tx, 2);
	ws2812_set_period_ms(&strip, 65535);
	ws2812_set_frame_ms(&strip, 20);
	ws2812_set_mode(&strip, WS2812_MODE_SOLID);  /* Preload ticks = frame_ticks */
	strip.phase = 65534;

	run_ticks(&strip, 1);
	CHECK_EQ_INT(g_cycle_cnt, 1);       /* Exactly one wrap */
	CHECK_EQ_INT(strip.phase, 19);      /* 65534+20-65535 */
	teardown(&strip);
}

static void test_callback_stop_next(void)
{
	begin("callback stops a later strip: it must not be advanced this round");
	/* strip2 starts first as list tail; strip starts later as head */
	ws2812_init(&strip2, buf2, 2);
	ws2812_set_gamma(&strip2, 0);
	ws2812_set_frame_ms(&strip2, 1);
	setup_backend(&g_be2, tx2, g_words2);
	ws2812_attach_backend(&strip2, &g_be2);
	CHECK_EQ_INT(ws2812_start(&strip2), 0);

	ws2812_init(&strip, buf, 2);
	ws2812_set_gamma(&strip, 0);
	ws2812_set_frame_ms(&strip, 1);
	setup_backend(&g_be, tx, g_words);
	ws2812_attach_backend(&strip, &g_be);
	ws2812_attach(&strip, WS2812_EVENT_FRAME, stop_next_cb, &strip2);
	CHECK_EQ_INT(ws2812_start(&strip), 0);

	g_calls = 0;
	ws2812_ticks();
	CHECK_EQ_INT(g_calls, 1);           /* Only the head strip should refresh */

	teardown(&strip2);
	teardown(&strip);
}

static void test_busy_retry(void)
{
	begin("backend busy: drop frame, emit ERROR, retry next tick");
	setup(&strip, buf, tx, 2);
	ws2812_set_color(&strip, 0xFFFFFF);
	ws2812_set_mode(&strip, WS2812_MODE_SOLID);

	g_busy_ret = WS2812_ERR_BUSY;
	run_ticks(&strip, 1);
	CHECK_EQ_INT(g_calls, 1);
	CHECK_EQ_INT(g_frame_cnt, 0);
	CHECK_EQ_INT(g_error_cnt, 1);

	run_ticks(&strip, 1);          /* Failed frame did not reset ticks */
	CHECK_EQ_INT(g_calls, 2);
	CHECK_EQ_INT(g_error_cnt, 2);

	g_busy_ret = 0;
	run_ticks(&strip, 1);
	CHECK_EQ_INT(g_calls, 3);
	CHECK_EQ_INT(g_frame_cnt, 1);
	CHECK_EQ_INT(g_error_cnt, 2);
	teardown(&strip);
}

static void test_bounds(void)
{
	WS2812 h;

	begin("bounds and NULL-pointer guards");
	memset(&h, 0, sizeof(h));
	ws2812_init(&h, NULL, 0);
	ws2812_set_pixel(&h, 0, 0xFFFFFF);          /* No buffer: silent return */
	CHECK_EQ_INT((int)ws2812_get_pixel(&h, 0), 0);
	CHECK_EQ_INT(ws2812_refresh(&h), WS2812_ERR_ARG);       /* No writer attached */

	setup(&strip, buf, tx, 3);
	ws2812_set_pixel(&strip, 3, 0xFFFFFF);      /* Out of range */
	ws2812_set_pixel(&strip, 100, 0xFFFFFF);
	CHECK_EQ_INT((int)ws2812_get_pixel(&strip, 3), 0);
	CHECK_EQ_INT((int)ws2812_get_pixel(&strip, 0xFFFF), 0);

	/* NULL guards for every entry point */
	ws2812_set_pixel(NULL, 0, 0);
	ws2812_fill(NULL, 0);
	ws2812_clear(NULL);
	ws2812_set_mode(NULL, WS2812_MODE_OFF);
	ws2812_set_color(NULL, 0);
	ws2812_set_brightness(NULL, 0);
	ws2812_set_order(NULL, WS2812_ORDER_RGB);
	ws2812_set_frame_ms(NULL, 10);
	ws2812_set_period_ms(NULL, 10);
	ws2812_attach(NULL, WS2812_EVENT_FRAME, NULL, NULL);
	ws2812_detach(NULL, WS2812_EVENT_FRAME);
	ws2812_step(NULL);
	ws2812_stop(NULL);
	CHECK_EQ_INT(ws2812_refresh(NULL), WS2812_ERR_ARG);
	CHECK_EQ_INT(ws2812_get_mode(NULL), WS2812_MODE_MANUAL);
	CHECK_EQ_INT((int)ws2812_get_color(NULL), 0);
	CHECK(1);                                   /* No crash is the assertion */

	begin("buffer length limit");
	{
		static uint8_t big[8];
		static WS2812Backend be;
		static uint8_t pixel_data[8];
		static uint16_t words[64];
		ws2812_init(&h, big, (uint16_t)(WS2812_MAX_BYTES / 3 + 1));
		setup_backend(&be, pixel_data, words);
		ws2812_attach_backend(&h, &be);
		CHECK_EQ_INT(ws2812_start(&h), WS2812_ERR_ARG);     /* Exceeds uint16 byte limit */
		ws2812_stop(&h);
	}
	teardown(&strip);
}

static void test_off_and_clear(void)
{
	uint8_t r, g, b;

	begin("OFF and clear");
	setup(&strip, buf, tx, 2);
	ws2812_fill(&strip, 0xFF00FF);
	CHECK_EQ_INT(ws2812_get_pixel(&strip, 0), 0xFF00FF);

	ws2812_set_mode(&strip, WS2812_MODE_OFF);
	run_ticks(&strip, 1);
	get_rgb(0, &r, &g, &b);
	CHECK_EQ_INT(r, 0); CHECK_EQ_INT(g, 0); CHECK_EQ_INT(b, 0);
	CHECK_EQ_INT(ws2812_get_pixel(&strip, 0), 0);

	ws2812_set_pixel(&strip, 1, 0x00FF00);
	ws2812_clear(&strip);
	CHECK_EQ_INT(ws2812_get_pixel(&strip, 1), 0);
	teardown(&strip);
}

static void test_transmit_argument_guard(void)
{
	WS2812Backend backend;
	uint16_t words[64];

	begin("library validates transmit arguments (words, count >= 3)");

	memset(&backend, 0, sizeof(backend));
	backend.transmit = mock_transmit;
	backend.is_busy = mock_is_busy;
	backend.pixel_data = tx;
	backend.timing.words = words;
	backend.timing.word_cap = 64;
	backend.timing.t0h = 10;
	backend.timing.t1h = 20;
	backend.timing.res_level = 0;
	backend.timing.res_lead = 0;
	backend.timing.res_trail = 0;
	backend.settle_ticks = 2;
	backend.timeout = 5;

	setup(&strip, buf, tx, 1);
	ws2812_attach_backend(&strip, &backend);

	/* NULL output buffer: rejected before transmit */
	backend.timing.words = NULL;
	g_tx_calls = 0;
	CHECK_EQ_INT(ws2812_refresh(&strip), WS2812_ERR_ARG);
	CHECK_EQ_INT(g_tx_calls, 0);

	/* Valid buffer: 1 LED = 3 bytes -> 24 words, well above the 3-word floor */
	backend.timing.words = words;
	CHECK_EQ_INT(ws2812_refresh(&strip), 0);
	CHECK_EQ_INT(g_tx_calls, 1);
	CHECK_EQ_INT(g_tx_count, 24);

	/* Backend with no transmit callback must be rejected */
	backend.transmit = NULL;
	CHECK_EQ_INT(ws2812_refresh(&strip), WS2812_ERR_ARG);
	CHECK_EQ_INT(g_tx_calls, 1);

	teardown(&strip);
}

static void test_busy_checked_before_transmit(void)
{
	WS2812Backend backend;
	uint16_t words[64];

	begin("library checks busy callback before transmit");

	memset(&backend, 0, sizeof(backend));
	backend.transmit = mock_transmit;
	backend.is_busy = mock_is_busy;
	backend.pixel_data = tx;
	backend.timing.words = words;
	backend.timing.word_cap = 64;
	backend.timing.t0h = 10;
	backend.timing.t1h = 20;
	backend.timing.res_level = 0;
	backend.timing.res_lead = 1;
	backend.timing.res_trail = 1;
	backend.settle_ticks = 2;
	backend.timeout = 5;

	setup(&strip, buf, tx, 2);
	ws2812_attach_backend(&strip, &backend);

	/* Peripheral busy from an external owner, backend not running yet.
	 * The library must ask is_busy() before calling transmit(). */
	g_tx_busy_value = 1;
	g_tx_calls = 0;
	g_tx_queries = 0;
	CHECK_EQ_INT(ws2812_refresh(&strip), WS2812_ERR_BUSY);
	CHECK_EQ_INT(g_tx_calls, 0);      /* no transfer started */
	CHECK(g_tx_queries > 0);          /* is_busy() was actually called */

	/* Free again: same refresh now goes out. */
	g_tx_busy_value = 0;
	CHECK_EQ_INT(ws2812_refresh(&strip), 0);
	CHECK_EQ_INT(g_tx_calls, 1);

	teardown(&strip);
}

static void test_transfer_backend(void)
{
	WS2812Backend backend;
	uint16_t words[64];

	begin("transfer backend: library builds words and calls transmit");

	memset(&backend, 0, sizeof(backend));
	memset(words, 0, sizeof(words));
	backend.transmit = mock_transmit;
	backend.is_busy = mock_is_busy;
	backend.pixel_data = tx;
	backend.timing.words = words;
	backend.timing.word_cap = 64;
	backend.timing.t0h = 10;
	backend.timing.t1h = 20;
	backend.timing.res_level = 0;
	backend.timing.res_lead = 1;
	backend.timing.res_trail = 1;
	backend.settle_ticks = 2;
	backend.timeout = 5;

	g_tx_calls = 0;
	g_tx_queries = 0;
	g_tx_busy_value = 0;

	setup(&strip, buf, tx, 2);
	ws2812_fill(&strip, 0xFFFFFF);   /* All bits 1 */
	ws2812_attach_backend(&strip, &backend);

	/* One frame: library builds bytes -> words and hands them to transmit */
	run_ticks(&strip, 1);
	CHECK_EQ_INT(g_tx_calls, 1);
	CHECK_EQ_INT(g_tx_count, 1 + 6u * 8u + 1u);  /* 2 LEDs = 6 bytes */
	CHECK_EQ_INT(g_tx_last[0], 0);               /* reset prefix */
	CHECK_EQ_INT(g_tx_last[1], 20);              /* 0xFF: all ones */

	begin("transfer backend: settle_ticks and busy handled by library");
	g_tx_busy_value = 1;
	run_ticks(&strip, 2);      /* start-up settle_ticks window */
	CHECK_EQ_INT(g_tx_calls, 1);  /* no new frame during settle_ticks */
	run_ticks(&strip, 1);      /* settle_ticks elapsed, backend still busy */
	CHECK_EQ_INT(g_tx_calls, 1);  /* still no new frame while busy */

	begin("transfer backend: timeout recovers and reports ERROR");
	g_error_cnt = 0;
	run_ticks(&strip, 8);
	CHECK(g_error_cnt > 0);       /* library emitted ERROR on timeout */

	g_tx_busy_value = 0;
	{
		int before = g_tx_calls;
		run_ticks(&strip, 1);
		CHECK(g_tx_calls > before);  /* recovered: frames go out again */
	}

	teardown(&strip);
}

static void test_start_accepts_backend(void)
{
	WS2812Backend backend;
	uint16_t words[64];

	begin("start accepts a strip whose only seam is a backend");

	memset(&backend, 0, sizeof(backend));
	backend.transmit = mock_transmit;
	backend.is_busy = mock_is_busy;
	backend.pixel_data = tx;
	backend.timing.words = words;
	backend.timing.word_cap = 64;
	backend.timing.t0h = 10;
	backend.timing.t1h = 20;
	backend.timing.res_level = 0;
	backend.timing.res_lead = 1;
	backend.timing.res_trail = 1;
	backend.settle_ticks = 0;
	backend.timeout = 5;

	setup(&strip, buf, tx, 2);
	teardown(&strip);                     /* leave the work list clean first */
	ws2812_attach_backend(&strip, &backend);   /* this clears handle->write */

	/* Backend-only strip: start() must not demand a byte-level writer. */
	CHECK_EQ_INT(ws2812_start(&strip), 0);
	CHECK_EQ_INT(ws2812_start(&strip), WS2812_ERR_BUSY);
	ws2812_stop(&strip);

	/* And a started strip must actually reach transmit() on a tick. */
	g_tx_busy_value = 0;
	g_tx_calls = 0;
	g_tx_queries = 0;
	CHECK_EQ_INT(ws2812_start(&strip), 0);
	run_ticks(&strip, 1);
	CHECK_EQ_INT(g_tx_calls, 1);
	CHECK_EQ_INT(g_tx_count, 1 + 6u * 8u + 1u);

	teardown(&strip);
}

static void test_brightness_gamma_order(void)
{
	begin("brightness is applied in perceptual space, before gamma");

	setup(&strip, buf, tx, 2);
	ws2812_set_pixel(&strip, 0, 0xFFFFFF);
	ws2812_set_gamma(&strip, 1);
	ws2812_set_brightness(&strip, 128);
	(void)ws2812_refresh(&strip);

	/* src=255, brightness=128 -> 128 in perceptual space -> gamma[128]=56.
	 * Inverse gamma gives back 128: perceived brightness is exactly 50%. */
	CHECK_EQ_INT(g_tx[0], 56);

	/* If brightness were applied after gamma, the result would be 128,
	 * which corresponds to 73% perceived brightness, not 50%. */
	ws2812_set_brightness(&strip, 64);
	(void)ws2812_refresh(&strip);
	CHECK_EQ_INT(g_tx[0], 12);   /* gamma[64] */

	/* With gamma off the same inputs stay linear. */
	ws2812_set_gamma(&strip, 0);
	ws2812_set_brightness(&strip, 128);
	(void)ws2812_refresh(&strip);
	CHECK_EQ_INT(g_tx[0], 128);

	teardown(&strip);
}

static void test_build_timing_words(void)
{
	uint16_t out[32];
	WS2812BitTiming timing;
	uint16_t n;
	int i;

	begin("build_timing_words: 24bit data to compare values");

	/* One byte 0x80: 2 resets + MSB-first bits + 3 resets = 13 words */
	for (i = 0; i < 32; i++) out[i] = 0xFFFFu;
	timing.words = out;
	timing.word_cap = 32;
	timing.t0h = 30;
	timing.t1h = 60;
	timing.res_level = 0;
	timing.res_lead = 2;
	timing.res_trail = 3;

	n = ws2812_build_timing_words(&timing, (const uint8_t *)"\x80", 1);
	CHECK_EQ_INT(n, 13);
	CHECK_EQ_INT(out[0], 0);   /* reset prefix */
	CHECK_EQ_INT(out[1], 0);
	CHECK_EQ_INT(out[2], 60);  /* bit7 = 1 */
	for (i = 3; i < 10; i++) {
		CHECK_EQ_INT(out[i], 30); /* bits6..0 = 0 */
	}
	CHECK_EQ_INT(out[10], 0);  /* reset suffix */
	CHECK_EQ_INT(out[11], 0);
	CHECK_EQ_INT(out[12], 0);
	CHECK_EQ_INT(out[13], 0xFFFFu);  /* nothing written past the result */

	/* 0x01 places the '1' on the last data bit, proving MSB-first channel_order */
	n = ws2812_build_timing_words(&timing, (const uint8_t *)"\x01", 1);
	CHECK_EQ_INT(n, 13);
	CHECK_EQ_INT(out[2], 30);
	CHECK_EQ_INT(out[9], 60);

	/* Two bytes: reset + 16 bits + reset = 21 words */
	n = ws2812_build_timing_words(&timing, (const uint8_t *)"\xA5\x3C", 2);
	CHECK_EQ_INT(n, 21);
	CHECK_EQ_INT(out[2], 60);  /* 0xA5 = 1010 0101 */
	CHECK_EQ_INT(out[3], 30);
	CHECK_EQ_INT(out[4], 60);
	CHECK_EQ_INT(out[9], 60);
	CHECK_EQ_INT(out[10], 30); /* 0x3C = 0011 1100 */
	CHECK_EQ_INT(out[11], 30);
	CHECK_EQ_INT(out[12], 60);

	begin("build_timing_words: no RES padding");
	timing.res_lead = 0;
	timing.res_trail = 0;
	timing.t0h = 10;
	timing.t1h = 20;
	timing.word_cap = 8;
	n = ws2812_build_timing_words(&timing, (const uint8_t *)"\xFF", 1);
	CHECK_EQ_INT(n, 8);
	for (i = 0; i < 8; i++) CHECK_EQ_INT(out[i], 20);

	begin("build_timing_words: invalid inputs and capacity");
	timing.res_lead = 2;
	timing.res_trail = 3;
	timing.t0h = 30;
	timing.t1h = 60;
	timing.word_cap = 32;
	CHECK_EQ_INT(ws2812_build_timing_words(NULL, (const uint8_t *)"\x80", 1), 0);
	CHECK_EQ_INT(ws2812_build_timing_words(&timing, NULL, 1), 0);
	CHECK_EQ_INT(ws2812_build_timing_words(&timing, (const uint8_t *)"\x80", 0), 0);
	timing.words = NULL;
	CHECK_EQ_INT(ws2812_build_timing_words(&timing, (const uint8_t *)"\x80", 1), 0);
	timing.words = out;
	timing.word_cap = 12;
	CHECK_EQ_INT(ws2812_build_timing_words(&timing, (const uint8_t *)"\x80", 1), 0);
	timing.word_cap = 13;
	CHECK_EQ_INT(ws2812_build_timing_words(&timing, (const uint8_t *)"\x80", 1), 13);
	timing.t1h = 30;  /* t0h == t1h cannot encode bits */
	CHECK_EQ_INT(ws2812_build_timing_words(&timing, (const uint8_t *)"\x80", 1), 0);
}

static void test_hsv(void)
{
	begin("HSV utility");
	CHECK_EQ_INT((int)ws2812_hsv(0, 255, 255), 0xFF0000);     /*   0 deg red */
	CHECK_EQ_INT((int)ws2812_hsv(256, 255, 255), 0xFFFF00);   /*  60 deg yellow */
	CHECK_EQ_INT((int)ws2812_hsv(512, 255, 255), 0x00FF00);   /* 120 deg green */
	CHECK_EQ_INT((int)ws2812_hsv(768, 255, 255), 0x00FFFF);   /* 180 deg cyan */
	CHECK_EQ_INT((int)ws2812_hsv(1024, 255, 255), 0x0000FF);  /* 240 deg blue */
	CHECK_EQ_INT((int)ws2812_hsv(1280, 255, 255), 0xFF00FF);  /* 300 deg magenta */
	CHECK_EQ_INT((int)ws2812_hsv(0, 0, 255), 0xFFFFFF);       /* No saturation = white */
	CHECK_EQ_INT((int)ws2812_hsv(0, 255, 0), 0x000000);       /* No value = black */
	CHECK_EQ_INT((int)ws2812_hsv(1536, 255, 255), 0xFF0000);  /* Hue wraps */
}

/* ============================ Entry ============================ */

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);   /* Unbuffered so progress survives a crash */

	printf("ws2812 unit tests  (v%d.%d.%d, TICKS_INTERVAL=%dms, GAMMA=%d)\n",
	       WS2812_VERSION_MAJOR, WS2812_VERSION_MINOR, WS2812_VERSION_PATCH,
	       WS2812_TICKS_INTERVAL, WS2812_USE_GAMMA);
	printf("--------------------------------------------------------------\n");

	test_init_defaults();
	test_color_order();
	test_brightness_and_gamma();
	test_frame_throttle();
	test_manual_mode();
	test_wire_encoding();
	test_solid_mode();
	test_blink_mode();
	test_breath_mode();
	test_rainbow_mode();
	test_chase_mode();
	test_cycle_event();
	test_multi_instance();
	test_stop();
	test_long_period_wrap();
	test_callback_stop_next();
	test_busy_retry();
	test_bounds();
	test_off_and_clear();
	test_hsv();
	test_brightness_gamma_order();
	test_transmit_argument_guard();
	test_busy_checked_before_transmit();
	test_build_timing_words();
	test_transfer_backend();
	test_start_accepts_backend();

	printf("--------------------------------------------------------------\n");
	printf("checks: %d, passed: %d, failed: %d\n", g_checks, g_pass, g_fail);
	printf("%s\n", g_fail == 0 ? "RESULT: PASS" : "RESULT: FAIL");
	return g_fail == 0 ? 0 : 1;
}
