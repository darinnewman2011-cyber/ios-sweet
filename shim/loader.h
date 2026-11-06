#pragma once
#include <stdint.h>

/* Boot the SWEETXNU payload found in the ABL ramdisk; returns only on error. */
const char *loader_boot(uintptr_t dtb);
/* Same, for a payload already in memory at [start, end). */
const char *loader_boot_payload(uintptr_t start, uintptr_t end);
/* Board hooks, weak no-ops by default: before writing [pa, pa+len), and right before the jump. */
void loader_hook_dst(uintptr_t pa, uint64_t len);
void loader_hook_jump(void);

/* The boot menu (a8/menu.c) may replace the payload's command line; NULL: use the payload's own. */
extern const char *loader_cmdline_override;
const char *loader_payload_cmdline(uintptr_t start);
