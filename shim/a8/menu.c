/*
 * Galaxy A8 boot menu: the three buttons pick the kernel boot-args that matter for graphics, on the S-Boot
 * framebuffer, before XNU is copied into place.
 *
 *   vol up / vol down   move          power   change the item (or run it)
 *
 * It never writes anything: it edits the command line the payload carries (loader_cmdline_override) and the
 * kernel gets that one. With no key pressed for 5 seconds the payload's own line is used untouched, so an
 * unattended boot behaves exactly as without the menu; an idle menu boots after 2 minutes for the same reason.
 *
 * The buttons are GPIO on gpa1 (PORTING_JACKPOTLTE.md: vol-down 6, vol-up 5, power 7), active low. The data
 * register address is a guess from the Exynos pin-bank layout until a build with -DA8_BUTTON_PROBE has shown
 * which register bit each button is on; then MENU_DAT/MENU_UP/MENU_DOWN/MENU_SEL are set with EXTRA_CFLAGS.
 */
#include <stdint.h>
#include "../console.h"
#include "../loader.h"
#include "menu.h"

#ifndef MENU_DAT
#define MENU_DAT	0x11CB0024UL	/* gpa1 data register: bank at 0x20, DAT at +4 (Exynos pinctrl layout) */
#endif
#ifndef MENU_UP
#define MENU_UP		5
#endif
#ifndef MENU_DOWN
#define MENU_DOWN	6
#endif
#ifndef MENU_SEL
#define MENU_SEL	7
#endif
#ifndef PROBE_BASE
#define PROBE_BASE	0x11CB0000UL	/* the ALIVE pin controller */
#endif

#define BLACK	0xff000000U
#define CYAN	0xff00ffffU
#define GREY	0xff909090U
#define ORANGE	0xffffa000U

#define LINE_MAX	608		/* BOOT_LINE_LENGTH in loader.c */

static int slen(const char *s)
{
	int n = 0;

	while (s[n])
		n++;
	return n;
}

static void scopy(char *d, const char *s)
{
	while ((*d++ = *s++))
		;
}

static void hex8(char *b, uint32_t v)
{
	static const char h[] = "0123456789ABCDEF";

	for (int i = 0; i < 8; i++)
		b[i] = h[(v >> (28 - 4 * i)) & 0xf];
	b[8] = 0;
}

/* ---- buttons ---- */

#define K_UP	1u
#define K_DOWN	2u
#define K_SEL	4u

static unsigned keys(void)
{
	uint32_t v = a8_rd32(MENU_DAT);
	unsigned k = 0;

	if (!((v >> MENU_UP) & 1))
		k |= K_UP;
	if (!((v >> MENU_DOWN) & 1))
		k |= K_DOWN;
	if (!((v >> MENU_SEL) & 1))
		k |= K_SEL;
	return k;
}

/* ---- the flag model ---- */

enum { GL, METAL, PROFILE, MHZ, REBOOT, NFLAGS };
enum { M_NONE, M_POSTERS, M_ALL };

static int cur[NFLAGS], def[NFLAGS];
static int reboot_secs = 240;
/* the Metal process list: the payload's own if it has one, else the poster processes */
static char posters[256] =
	"PosterBoard,CollectionsPoster,PhotosPosterProvider,EmojiPosterExtension,GradientPosterExtension,"
	"UnityPosterExtension,PridePosterExtension,WeatherPoster,AegirPoster,ExtragalacticPoster,LegacyPoster";
static char out[LINE_MAX];

/* one token of the command line: [t, t+n) */
static int is_key(const char *t, int n, const char *key)
{
	int k = slen(key);

	if (n < k)
		return 0;
	for (int i = 0; i < k; i++)
		if (t[i] != key[i])
			return 0;
	return n == k || t[k] == '=';
}

static int is_managed(const char *t, int n)
{
	return is_key(t, n, "sweetinject") || is_key(t, n, "sweetgl") || is_key(t, n, "sweetmetalfor") ||
	       is_key(t, n, "sweetmetal") || is_key(t, n, "metal_profile") || is_key(t, n, "sweetgpumhz") ||
	       is_key(t, n, "sweetreboot");
}

static const char *val(const char *t, int n)	/* the part after '=', or "" */
{
	for (int i = 0; i < n; i++)
		if (t[i] == '=')
			return t + i + 1;
	return t + n;
}

static void parse(const char *base)
{
	const char *t = base;

	for (int i = 0; i < NFLAGS; i++)
		def[i] = 0;
	def[MHZ] = 0;
	while (*t) {
		int n = 0;

		while (*t == ' ')
			t++;
		while (t[n] && t[n] != ' ')
			n++;
		if (n == 0)
			break;
		if (is_key(t, n, "sweetinject"))
			def[GL] = 1;
		else if (is_key(t, n, "sweetmetalfor")) {
			const char *v = val(t, n);
			int vl = n - (int)(v - t);

			if (vl == 1 && v[0] == '*')
				def[METAL] = M_ALL;
			else if (vl > 0 && vl < (int)sizeof(posters)) {
				def[METAL] = M_POSTERS;
				for (int i = 0; i < vl; i++)
					posters[i] = v[i];
				posters[vl] = 0;
			}
		} else if (is_key(t, n, "metal_profile"))
			def[PROFILE] = 1;
		else if (is_key(t, n, "sweetgpumhz")) {
			const char *v = val(t, n);

			def[MHZ] = v[0] == '5' && v[1] == '5' && v[2] == '0';
		} else if (is_key(t, n, "sweetreboot")) {
			const char *v = val(t, n);
			int x = 0;

			while (*v >= '0' && *v <= '9')
				x = x * 10 + (*v++ - '0');
			def[REBOOT] = x > 0;
			if (x > 0)
				reboot_secs = x;
		}
		t += n;
	}
	for (int i = 0; i < NFLAGS; i++)
		cur[i] = def[i];
}

static int same_as_default(void)
{
	for (int i = 0; i < NFLAGS; i++)
		if (cur[i] != def[i])
			return 0;
	return 1;
}

/* append s to o at *pos; 0 if it does not fit (the NUL is kept) */
static int app(char *o, int *pos, const char *s, int n)
{
	if (*pos + n + 2 > LINE_MAX)
		return 0;
	for (int i = 0; i < n; i++)
		o[(*pos)++] = s[i];
	o[*pos] = 0;
	return 1;
}

static int app_s(char *o, int *pos, const char *s)
{
	return app(o, pos, s, slen(s));
}

static int app_num(char *o, int *pos, int v)
{
	char b[12];
	int n = 0;

	if (v == 0)
		b[n++] = '0';
	while (v > 0) {
		b[n++] = '0' + v % 10;
		v /= 10;
	}
	for (int i = 0; i < n; i++)
		if (!app(o, pos, &b[n - 1 - i], 1))
			return 0;
	return 1;
}

/* the managed part only: what the menu decides */
static int managed_args(char *o, int *pos)
{
	int ok = 1;

	if (cur[GL])
		ok &= app_s(o, pos, " sweetinject=backboardd");
	else
		ok &= app_s(o, pos, " sweetgl=0");
	if (cur[METAL] == M_ALL)
		ok &= app_s(o, pos, " sweetmetalfor=*");
	else if (cur[METAL] == M_POSTERS) {
		ok &= app_s(o, pos, " sweetmetalfor=");
		ok &= app_s(o, pos, posters);
	}
	if (cur[PROFILE])
		ok &= app_s(o, pos, " metal_profile=1");
	if (cur[MHZ])
		ok &= app_s(o, pos, " sweetgpumhz=550");
	ok &= app_s(o, pos, " sweetreboot=");
	ok &= app_num(o, pos, cur[REBOOT] ? reboot_secs : 0);
	return ok;
}

/* the full line: the payload's tokens that the menu does not manage, then the managed ones */
static int build(const char *base, char *o)
{
	const char *t = base;
	int pos = 0;

	o[0] = 0;
	while (*t) {
		int n = 0;

		while (*t == ' ')
			t++;
		while (t[n] && t[n] != ' ')
			n++;
		if (n == 0)
			break;
		if (!is_managed(t, n)) {
			if (pos && !app(o, &pos, " ", 1))
				return 0;
			if (!app(o, &pos, t, n))
				return 0;
		}
		t += n;
	}
	if (!managed_args(o, &pos))
		return 0;
	if (o[0] == ' ') {	/* nothing before the managed part */
		for (int i = 0; i <= pos; i++)
			o[i] = o[i + 1];
	}
	return 1;
}

/* ---- screen ---- */

#define N_ITEMS	11
static const char *item_name(int i)
{
	switch (i) {
	case 0: return "PRESET  GL + METAL POSTERS (DEFAULT)";
	case 1: return "PRESET  OPENGL ONLY";
	case 2: return "PRESET  METAL ONLY (WHOLE SYSTEM)";
	case 3: return "PRESET  NEITHER (CPU DRAWING)";
	case 4: return "GL FOR THE SCREEN (BACKBOARDD)";
	case 5: return "METAL VISIBLE TO";
	case 6: return "METAL PROFILE (APPLE2)";
	case 7: return "GPU CLOCK";
	case 8: return "AUTO REBOOT (TEST TIMER)";
	case 9: return "RESET ALL TO DEFAULT";
	}
	return "BOOT NOW";
}

static const char *state_text(int i)
{
	switch (i) {
	case 4: return cur[GL] ? "ON" : "OFF";
	case 5: return cur[METAL] == M_ALL ? "ALL" : cur[METAL] == M_POSTERS ? "POSTERS" : "NONE";
	case 6: return cur[PROFILE] ? "ON" : "OFF";
	case 7: return cur[MHZ] ? "550 MHZ" : "343 MHZ";
	case 8: return cur[REBOOT] ? "ON" : "OFF";
	}
	return "";
}

static void preset(int gl, int metal, int profile)
{
	cur[GL] = gl;
	cur[METAL] = metal;
	cur[PROFILE] = profile;
	cur[MHZ] = 1;
}

/* returns 1 to boot */
static int activate(int i, const char *base, const char **msg)
{
	switch (i) {
	case 0: preset(1, M_POSTERS, 1); break;
	case 1: preset(1, M_NONE, 0); break;
	case 2: preset(0, M_ALL, 1); break;
	case 3: preset(0, M_NONE, 0); break;
	case 4: cur[GL] ^= 1; break;
	case 5: cur[METAL] = (cur[METAL] + 1) % 3; break;
	case 6: cur[PROFILE] ^= 1; break;
	case 7: cur[MHZ] ^= 1; break;
	case 8: cur[REBOOT] ^= 1; break;
	case 9:
		for (int k = 0; k < NFLAGS; k++)
			cur[k] = def[k];
		break;
	case 10:
		if (!build(base, out)) {
			*msg = "COMMAND LINE TOO LONG";
			return 0;
		}
		return 1;
	}
	return 0;
}

static void draw(int sel, int interactive, int secs, const char *msg, const char *base)
{
	char tmp[LINE_MAX];
	int pos = 0;

	con_clear(BLACK);
	con_text(0, 0, "A8 BOOT MENU", CYAN, BLACK);
	con_text(1, 0, "VOL UP/DOWN MOVE   POWER CHANGE", GREY, BLACK);
	for (int i = 0; i < N_ITEMS; i++) {
		uint32_t fg = i == sel ? WHITE : (i < 4 ? ORANGE : WHITE), bg = i == sel ? BLUE : BLACK;
		const char *st = state_text(i);
		int c = 2, n = slen(item_name(i));

		con_text(3 + i, 0, i == sel ? ">" : " ", WHITE, bg);
		con_text(3 + i, c, item_name(i), fg, bg);
		if (st[0])
			con_text(3 + i, c + n + 1 < 30 ? 30 : c + n + 1, st, GREEN, bg);
	}
	con_text(15, 0, "MENU CHANGES THESE ARGS:", GREY, BLACK);
	tmp[0] = 0;
	if (managed_args(tmp, &pos)) {
		/* wrapped at 40 columns, on spaces */
		int row = 16, start = 1;

		while (start < pos && row < 24) {
			int end = start + 40, e;
			char line[44];

			if (end >= pos)
				end = pos;
			else
				while (end > start && tmp[end] != ' ')
					end--;
			if (end <= start)
				end = start + 40 < pos ? start + 40 : pos;
			for (e = 0; start + e < end && e < 41; e++)
				line[e] = tmp[start + e];
			line[e] = 0;
			con_text(row++, 0, line, WHITE, BLACK);
			start = end + 1;
		}
	}
	if (!same_as_default())
		con_text(25, 0, "CHANGED FROM THE BAKED DEFAULT", ORANGE, BLACK);
	if (msg[0])
		con_text(27, 0, msg, RED, BLACK);
	if (!interactive) {
		char b[48];

		scopy(b, "BOOTING THE DEFAULT IN 0 S (ANY KEY STOPS)");	/* an initialiser would call memcpy */
		b[23] = '0' + secs;
		con_text(29, 0, b, YELLOW, BLACK);
	} else {
		con_text(29, 0, "MENU IDLE 2 MIN BOOTS THE CHOSEN SET", GREY, BLACK);
	}
	(void)base;
	a8_present_for_menu();
}

void menu_run(const char *base)
{
	unsigned mask, prev, k;
	int sel = N_ITEMS - 1, interactive = 0, shown = -1;
	const char *msg = "";
	uint64_t f = a8_ticks(), t0 = a8_now(), last = t0;

	parse(base);
	mask = prev = keys();	/* a key already down at the start is ignored until it is let go */
	for (;;) {
		uint64_t t = a8_now();
		int secs = 5 - (int)((t - t0) / f);

		k = keys();
		mask &= k;
		k &= ~mask;
		if (!interactive) {
			if (k) {
				interactive = 1;
				last = t;
				draw(sel, 1, 0, msg, base);
			} else if (secs <= 0) {
				return;	/* untouched: the payload's own line */
			} else if (secs != shown) {
				shown = secs;
				draw(sel, 0, secs, msg, base);
			}
		} else {
			unsigned e = k & ~prev;

			if (e) {
				last = t;
				msg = "";
				if (e & K_UP)
					sel = (sel + N_ITEMS - 1) % N_ITEMS;
				if (e & K_DOWN)
					sel = (sel + 1) % N_ITEMS;
				if (e & K_SEL) {
					if (activate(sel, base, &msg))
						break;
				}
				draw(sel, 1, 0, msg, base);
			} else if (t - last > 120 * f) {
				if (!build(base, out)) {
					return;
				}
				break;
			}
		}
		prev = k;
		a8_delay_ms(20);
	}
	if (!same_as_default())
		loader_cmdline_override = out;
	else
		loader_cmdline_override = 0;
	con_clear(BLACK);
	con_text(0, 0, "BOOTING...", CYAN, BLACK);
	a8_present_for_menu();
}

/* ---- button probe ---- */

#define PROBE_WORDS	64
#define PROBE_ROWS	30

static void put2(char *b, unsigned v)	/* two decimal digits */
{
	b[0] = '0' + (v / 10) % 10;
	b[1] = '0' + v % 10;
}

void menu_probe(void)
{
	uint32_t last[PROBE_WORDS], noisy_cnt[PROBE_WORDS];
	unsigned ev_off[PROBE_ROWS], ev_bit[PROBE_ROWS], ev_new[PROBE_ROWS], nev = 0, noisy[PROBE_WORDS];
	uint64_t f = a8_ticks(), t0 = a8_now();
	char b[64];
	int dirty = 1, secs_shown = -1;

	for (int i = 0; i < PROBE_WORDS; i++) {
		last[i] = a8_rd32(PROBE_BASE + 4 * i);
		noisy_cnt[i] = 0;
		noisy[i] = 0;
	}
	for (;;) {
		uint64_t t = a8_now();
		int left = 90 - (int)((t - t0) / f), settling = (t - t0) < f * 3 / 2;

		if (left <= 0)
			return;
		for (int i = 0; i < PROBE_WORDS; i++) {
			uint32_t v = a8_rd32(PROBE_BASE + 4 * i), x;

			if (v == last[i] || noisy[i])
				continue;
			x = v ^ last[i];
			last[i] = v;
			if (settling) {	/* a register that keeps changing by itself is no button */
				if (++noisy_cnt[i] > 3)
					noisy[i] = 1;
				continue;
			}
			for (unsigned bit = 0; bit < 32; bit++) {
				if (!((x >> bit) & 1))
					continue;
				if (nev == PROBE_ROWS) {
					for (int j = 1; j < PROBE_ROWS; j++) {
						ev_off[j - 1] = ev_off[j];
						ev_bit[j - 1] = ev_bit[j];
						ev_new[j - 1] = ev_new[j];
					}
					nev--;
				}
				ev_off[nev] = 4 * i;
				ev_bit[nev] = bit;
				ev_new[nev] = (v >> bit) & 1;
				nev++;
				dirty = 1;
			}
		}
		if (dirty || left != secs_shown) {
			con_clear(BLACK);
			con_text(0, 0, "BUTTON PROBE", CYAN, BLACK);
			con_text(1, 0, "PRESS + RELEASE, ONE AT A TIME:", WHITE, BLACK);
			con_text(2, 0, "VOL UP, THEN VOL DOWN, THEN POWER", WHITE, BLACK);
			con_text(4, 0, "LINES: OFFSET (HEX) / BIT / NOW", GREY, BLACK);
			for (unsigned j = 0; j < nev; j++) {
				char l[40];

				scopy(l, "OFF 000  BIT 00  NOW 0");
				hex8(b, ev_off[j]);
				l[4] = b[5];
				l[5] = b[6];
				l[6] = b[7];
				put2(l + 13, ev_bit[j]);
				l[21] = '0' + ev_new[j];
				con_text(6 + j, 0, l, ev_new[j] ? GREEN : YELLOW, BLACK);
			}
			scopy(b, "BOOTS AFTER 00 S");
			put2(b + 12, (unsigned)left);
			con_text(37, 0, b, YELLOW, BLACK);
			a8_present_for_menu();
			secs_shown = left;
			dirty = 0;
		}
		a8_delay_ms(15);
	}
}
