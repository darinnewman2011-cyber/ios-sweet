#pragma once
#include <stdint.h>

/* sweet defaults; other devices override with -DFB_BASE=... -DFB_H=... (shim/a8/build.sh) */
#ifndef FB_BASE
#define FB_BASE		0x9c000000UL	/* reserved-memory cont_splash_region */
#endif
#ifndef FB_W
#define FB_W		1080
#endif
#ifndef FB_H
#define FB_H		2400
#endif
#define FB_STRIDE	(FB_W * 4)	/* a8r8g8b8, matches mainline simplefb */

#define RED	0xffff0000U
#define BLUE	0xff0000a0U
#define WHITE	0xffffffffU
#define YELLOW	0xffffff00U
#define GREEN	0xff00ff00U

extern volatile uint32_t *con_fb;

void con_clear(uint32_t bg);
void con_skip(void);
/* text at row r (0-based), column c, explicit colours: for menus, which redraw whole screens */
void con_text(int r, int c, const char *s, uint32_t fg, uint32_t bg);
#define CON_ROWS ((FB_H - 2 * 24) / 48)
#define CON_COLS ((FB_W - 2 * 24) / 24)
void con_str(const char *label, const char *s);
void con_hex(const char *label, uint64_t v);
/* label, then "a=xxxxxxxx b=xxxxxxxx" style pairs of 32-bit values */
void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb);
