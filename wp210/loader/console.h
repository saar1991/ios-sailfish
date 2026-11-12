#pragma once
#include <stdint.h>
/* Oukitel WP210 Pro (MT6895): boot-logo framebuffer from /chosen atag,videolfb */
#define FB_BASE		0xfe1af000UL
#define FB_W		1080
#define FB_H		2412
#define FB_BPP		4
#define FB_STRIDE	(FB_W * FB_BPP)
#define FB_BUF_STRIDE	(1088UL * 2432 * 4)	/* 3 buffers, LK-aligned */
#define FB_NBUF		3

#define RED	0xffff0000U
#define BLUE	0xff0000a0U
#define WHITE	0xffffffffU
#define YELLOW	0xffffff00U
#define GREEN	0xff00ff00U

void con_init(void);	/* ungate the display clocks */
void con_clear(uint32_t bg);
void con_flush(void);	/* push a frame to the command-mode panel */
void con_skip(void);
void con_str(const char *label, const char *s);
void con_hex(const char *label, uint64_t v);
void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb);
