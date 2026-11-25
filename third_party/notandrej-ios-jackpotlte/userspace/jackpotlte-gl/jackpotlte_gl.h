/* SPDX-License-Identifier: MIT
 *
 * What the EAGL shim (userspace/jackpotlte-gl/) asks of the OpenGL ES library
 * behind it.  One implementation: jackpotlte_gl_osmesa.c, over Mesa's OSMesa as
 * tools/mesa-jackpotlte/ builds it (panfrost, Bifrost v6).
 */
#ifndef JACKPOTLTE_GL_H
#define JACKPOTLTE_GL_H

#include "gl_slots.h"

#include <stdint.h>

struct jackpotlte_gl;

/*
 * The library's function of that name, a gl* entry point, or NULL if it has
 * none.  This is how gl_slots.c's entries are resolved, and the reason they are
 * not called by name: iOS's own OpenGLES.framework is in the shared cache and
 * defines every gl* symbol, so a by-name call reaches Apple's driver, which
 * knows nothing of this context.  The library is asked through its own
 * OSMesaGetProcAddress instead.
 */
typedef void (*jackpotlte_gl_func)(void);
jackpotlte_gl_func jackpotlte_gl_proc(const char *name);

/*
 * Called by every entry of the dispatch table before the call is passed on
 * (gl_slots.c): the table's first argument is the context the caller means, and
 * a GL call runs on the thread where its context is current.  The display
 * server's renderer is not always the thread that made the context, so this is
 * where that is made true again - a comparison when it already is.
 */
void jackpotlte_gl_use(void *context);

/*
 * Called by the dispatch table's drawing entries after the call is passed on
 * (gl_slots.c, from the SUBMIT set in tools/jackpotlte/eagl-glue.py): the
 * library keeps a draw in a batch until a flush, and Apple's driver does not,
 * so without this a renderer that draws and never flushes draws nowhere.
 * Bring-up aid; costs the batching.
 */
void jackpotlte_gl_after_draw(void);

/* An OpenGL ES 2 or 3 context of the library, or NULL. */
struct jackpotlte_gl *jackpotlte_gl_create(int version);

/* Make GL the calling thread's (NULL: none).  0: failed. */
int jackpotlte_gl_make_current(struct jackpotlte_gl *gl);

void jackpotlte_gl_destroy(struct jackpotlte_gl *gl);

/* For the log. */
const char *jackpotlte_gl_library(void);

/*
 * One log line, on top of stderr (eagl.c and this library each say() to
 * stderr).  The library is loaded into the display server, whose stderr is not
 * anywhere a bring-up can read it, so with the boot-arg jtgllog=1 the line also
 * goes to /dev/console -- the kernel's console, which is where a panic draws
 * and what the persistent ring keeps.  The switch is read once, from
 * kern.bootargs; absent, nothing is opened and nothing extra is written.
 */
void jackpotlte_gl_log(const char *line);

/*
 * Make the memory of the SweetSurface SURFACE the texture bound to
 * GL_TEXTURE_2D, where the library's driver can (the kernel's GPU driver makes
 * a buffer of the surface: 0154's 0x7e, through the drm face).  0: not done,
 * and the caller fills the texture with ordinary calls.
 */
int jackpotlte_gl_surface_texture(struct jackpotlte_gl *gl, uint32_t surface);

#endif
