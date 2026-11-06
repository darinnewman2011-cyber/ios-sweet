#pragma once
#include <stdint.h>

/* services of diag.c */
uint64_t a8_now(void);
uint64_t a8_ticks(void);
void a8_delay_ms(unsigned ms);
int a8_present_for_menu(void);
uint32_t a8_rd32(uintptr_t addr);

/* a8/menu.c */
void menu_probe(void);			/* -DA8_BUTTON_PROBE: which register bit is each button */
void menu_run(const char *base_cmdline);	/* sets loader_cmdline_override if the user changed anything */
