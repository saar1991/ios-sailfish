/*
 * WP210 screen: NT37705 command-mode panel behind MT6895's dual display pipe.
 * Draw into all three boot buffers, then pulse both pipes' mutex EN and
 * DSI_START (found with the bare-metal test shim, wp210/README.md).
 */
#include "console.h"
#include "font8x16.h"

#define SCALE		3
#define LINE_H		(16 * SCALE + 6)
#define MMSYS0		0x14000000UL
#define MMSYS1		0x14400000UL
#define MUTEX0_EN	0x14001020UL
#define MUTEX1_EN	0x14401020UL
#define DSI_START	0x14017000UL

#define W32(a, v)	(*(volatile uint32_t *)(a) = (v))

static int line;

static uint64_t cnt(void) { uint64_t v; __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v)); return v; }
static uint64_t frq(void) { uint64_t v; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v ? v : 13000000; }

static void put_px(uint32_t x, uint32_t y, uint32_t c)
{
	for (int b = 0; b < FB_NBUF; b++)
		*(volatile uint32_t *)(FB_BASE + b * FB_BUF_STRIDE + (uint64_t)y * FB_STRIDE + x * 4) = c;
}

void con_init(void)
{
	const uint64_t mm[2] = { MMSYS0, MMSYS1 };
	for (int i = 0; i < 2; i++) {
		W32(mm[i] + 0x108, ~0u);
		W32(mm[i] + 0x118, ~0u);
		W32(mm[i] + 0x1a8, ~0u);
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

void con_flush(void)
{
	__asm__ volatile("dsb sy" ::: "memory");
	W32(MUTEX0_EN, 0); W32(MUTEX0_EN, 1);
	W32(MUTEX1_EN, 0); W32(MUTEX1_EN, 1);
	W32(DSI_START, 0); W32(DSI_START, 1);
	uint64_t end = cnt() + frq() / 30;	/* let the frame go out */
	while (cnt() < end)
		;
}

void con_clear(uint32_t bg)
{
	for (uint32_t y = 0; y < FB_H; y++)
		for (uint32_t x = 0; x < FB_W; x++)
			put_px(x, y, bg);
	line = 0;
}

static void put_char(int col, char ch, uint32_t fg)
{
	if (ch < 32 || ch > 126)
		ch = '?';
	const unsigned char *g = font8x16[ch - 32];
	uint32_t ox = 20 + col * 8 * SCALE, oy = 80 + line * LINE_H;
	if (ox + 8 * SCALE > FB_W || oy + 16 * SCALE > FB_H)
		return;
	for (int y = 0; y < 16; y++)
		for (int x = 0; x < 8; x++) {
			uint32_t c = (g[y] & (0x80 >> x)) ? fg : 0xff000000u;
			for (int sy = 0; sy < SCALE; sy++)
				for (int sx = 0; sx < SCALE; sx++)
					put_px(ox + x * SCALE + sx, oy + y * SCALE + sy, c);
		}
}

static int put_s(int col, const char *s, uint32_t fg)
{
	while (*s)
		put_char(col++, *s++, fg);
	return col;
}

static void fmt_hex(char *buf, uint64_t v, int digits)
{
	for (int i = digits - 1; i >= 0; i--, v >>= 4)
		buf[i] = "0123456789ABCDEF"[v & 15];
	buf[digits] = 0;
}

void con_skip(void) { line++; }

void con_str(const char *label, const char *s)
{
	put_s(put_s(0, label, YELLOW) + 1, s, WHITE);
	line++;
}

void con_hex(const char *label, uint64_t v)
{
	char b[17];
	fmt_hex(b, v, 16);
	con_str(label, b);
}

void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb)
{
	char h[9];
	int c = put_s(0, label, YELLOW) + 1;
	c = put_s(c, a, WHITE); fmt_hex(h, va, 8); c = put_s(c, h, WHITE) + 1;
	c = put_s(c, b, WHITE); fmt_hex(h, vb, 8); put_s(c, h, WHITE);
	line++;
}
