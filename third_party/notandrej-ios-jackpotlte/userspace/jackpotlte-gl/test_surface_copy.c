/* SPDX-License-Identifier: MIT
 *
 * Host test for the -texImageIOSurface: copy path (eagl.c).  The phone's GL
 * shim is not testable on the phone during a bring-up, and this decides which
 * surfaces become textures and in what format - the difference between the lock
 * screen's clock and a white square (JACKPOTLTE_PROGRESS.md, jt64: surface 6 is
 * '..' and surface 5 is 'L008', both refused).
 *
 * eagl.c is written for the phone; this file puts a fake IOSurface and fake GL
 * behind it and calls the real static functions.
 *
 *   userspace/build.sh            builds and runs it as out/test_surface_copy
 *   clang -I. test_surface_copy.c stubs_linux.c -o test   (no Apple SDK: the
 *                                  objc/dyld/sysctl stubs are the caller's)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static size_t bytes_a_pixel(unsigned long, unsigned long);

/* The surface the shim sees, defined before eagl.c so fields are visible. */
struct __IOSurface {
   uint32_t id;
   uint32_t pixel_format;
   size_t width, height, bpr, bpe, planes, alloc;
   unsigned char *bytes;
};

/* Fake GL, in the shapes the phone's are resolved into. */
static int fake_texture = 3;
static unsigned fake_error;
static unsigned char captured[1 << 20];
static struct { int called, internal, w, h, format, type; } upload;

static void
glTexImage2D_fake(unsigned target, int level, int internal, int w, int h,
                  int border, unsigned format, unsigned type, const void *pixels)
{
   (void)level; (void)border;
   upload.called = 1; upload.internal = internal; upload.w = w; upload.h = h;
   upload.format = (int)format; upload.type = (int)type;
   if (pixels != NULL) {
      size_t bpp = bytes_a_pixel(format, type);
      size_t n = (size_t)w * (size_t)h * (bpp != 0 ? bpp : 1);

      if (n <= sizeof(captured))
         memcpy(captured, pixels, n);
   }
   if (target != 0x0de1) {
      fake_error = 0x500;
   }
}
static void glTexParameteri_fake(unsigned t, unsigned p, int v)
{ (void)t; (void)p; (void)v; }
static unsigned glGetError_fake(void)
{ unsigned e = fake_error; fake_error = 0; return e; }
static void glGetIntegerv_fake(unsigned p, int *out)
{ *out = (p == 0x8069) ? fake_texture : 0; }
static const unsigned char *glGetString_fake(unsigned n)
{ (void)n; return (const unsigned char *)"fake"; }

#include "eagl.c"

/* ---- the objects the shim's headers declare --------------------------- */

uint32_t IOSurfaceGetID(struct __IOSurface *s) { return s->id; }
uint32_t IOSurfaceGetPixelFormat(struct __IOSurface *s) { return s->pixel_format; }
size_t IOSurfaceGetPlaneCount(struct __IOSurface *s) { return s->planes; }
size_t IOSurfaceGetBytesPerElement(struct __IOSurface *s) { return s->bpe; }
size_t IOSurfaceGetBytesPerRow(struct __IOSurface *s) { return s->bpr; }
size_t IOSurfaceGetAllocSize(struct __IOSurface *s) { return s->alloc; }
size_t IOSurfaceGetWidth(struct __IOSurface *s) { return s->width; }
size_t IOSurfaceGetHeight(struct __IOSurface *s) { return s->height; }
void *IOSurfaceGetBaseAddress(struct __IOSurface *s) { return s->bytes; }
int IOSurfaceLock(struct __IOSurface *s, uint32_t o, uint32_t *seed)
{ (void)s; (void)o; (void)seed; return 0; }
int IOSurfaceUnlock(struct __IOSurface *s, uint32_t o, uint32_t *seed)
{ (void)s; (void)o; (void)seed; return 0; }

jackpotlte_gl_func
jackpotlte_gl_proc(const char *name)
{
   if (strcmp(name, "glTexImage2D") == 0) return (jackpotlte_gl_func)glTexImage2D_fake;
   if (strcmp(name, "glTexParameteri") == 0) return (jackpotlte_gl_func)glTexParameteri_fake;
   if (strcmp(name, "glGetError") == 0) return (jackpotlte_gl_func)glGetError_fake;
   if (strcmp(name, "glGetIntegerv") == 0) return (jackpotlte_gl_func)glGetIntegerv_fake;
   if (strcmp(name, "glGetString") == 0) return (jackpotlte_gl_func)glGetString_fake;
   return NULL;
}

static int stub_context;

void jackpotlte_gl_fill_table(void *table[JACKPOTLTE_GL_SLOTS]) { (void)table; }
struct jackpotlte_gl *jackpotlte_gl_create(int v) { (void)v; return (void *)&stub_context; }
int jackpotlte_gl_make_current(struct jackpotlte_gl *g) { return g == (void *)&stub_context; }
void jackpotlte_gl_destroy(struct jackpotlte_gl *g) { (void)g; }
const char *jackpotlte_gl_library(void) { return "fake"; }
int jackpotlte_gl_surface_texture(struct jackpotlte_gl *g, uint32_t s)
{ (void)g; (void)s; return 1; }

/* ---- the checks ------------------------------------------------------- */

static struct __IOSurface *
surface_new(uint32_t id, uint32_t fmt, size_t w, size_t h, size_t bpr,
            size_t bpe, size_t alloc)
{
   struct __IOSurface *s = calloc(1, sizeof(*s));
   size_t n = alloc != 0 ? alloc : bpr * h;

   s->id = id; s->pixel_format = fmt; s->width = w; s->height = h;
   s->bpr = bpr; s->bpe = bpe; s->alloc = n;
   s->bytes = calloc(1, n);
   for (size_t i = 0; i < n; i++)
      s->bytes[i] = (unsigned char)(i * 7 + 1);
   return s;
}

static int failures;

static void
check(const char *what, int ok)
{
   printf("%-56s %s\n", what, ok ? "ok" : "FAIL");
   if (!ok) failures++;
}

int
main(void)
{
   struct jackpotlte_eagl eagl;

   memset(&eagl, 0, sizeof(eagl));
   eagl.gl = (struct jackpotlte_gl *)&stub_context;

   /* 1. L008: 8-bit luminance mask, the camera button's glyph. */
   memset(&upload, 0, sizeof(upload));
   {
      struct __IOSurface *s = surface_new(5, 0x4c303038u /* 'L008' */, 64, 64,
                                          64, 1, 4096);

      check("L008 64x64 alpha mask becomes a texture",
            surface_copy_texture(&eagl, s, 0, 0, 0, GL_ALPHA, GL_UNSIGNED_BYTE,
                                 NO, "-t") == YES);
      check("  it uploaded, not refused", upload.called);
      check("  width/height from the surface", upload.w == 64 && upload.h == 64);
      check("  GL_ALPHA internal", upload.internal == GL_ALPHA);
      check("  GL_ALPHA format, UNSIGNED_BYTE type",
            upload.format == GL_ALPHA && upload.type == GL_UNSIGNED_BYTE);
      check("  pixels are the surface's own",
            captured[0] == s->bytes[0] && captured[1] == s->bytes[1]);
   }

   /* 2. The 256x1 float gradient ramp (the clock's), format/type not given. */
   memset(&upload, 0, sizeof(upload));
   {
      struct __IOSurface *s = surface_new(6, 0, 256, 1, 4096, 2, 4096);

      check("256x1 16-byte-pixel ramp becomes a texture",
            surface_copy_texture(&eagl, s, 0, 0, 0, 0, 0, NO, "-t") == YES);
      check("  it uploaded", upload.called);
      check("  RGBA32F internal", upload.internal == GL_RGBA32F);
      check("  GL_RGBA / GL_FLOAT",
            upload.format == GL_RGBA && upload.type == GL_FLOAT);
      check("  whole ramp", upload.w == 256 && upload.h == 1);
   }

   /* 3. The jt64 log's surface 6 (JACKPOTLTE_PROGRESS.md, "jt64's 12:05 log"):
    *    256x1, pixelFormat 0, bytesPerElement 2, bytesPerRow 672, 4096
    *    allocated.  672 is not 256 of anything and 4096 is the page rounding, so
    *    the surface's own bytes a pixel is what is left. */
   memset(&upload, 0, sizeof(upload));
   memset(captured, 0, sizeof(captured));
   {
      struct __IOSurface *s = surface_new(6, 0, 256, 1, 672, 2, 4096);

      check("jt64's surface 6 becomes a texture",
            surface_copy_texture(&eagl, s, 0, 0, 0, 0, 0, NO, "-t") == YES);
      check("  two bytes a pixel, as the surface says",
            upload.format == GL_LUMINANCE_ALPHA &&
            upload.type == GL_UNSIGNED_BYTE);
      check("  one 512-byte row of the 672-byte surface row",
            captured[0] == s->bytes[0] && captured[511] == s->bytes[511]);
      check("  and nothing from past it", captured[512] == 0);
   }

   /* 4. A short allocation: bytesPerRow times the height is more than the
    *    surface says it allocated, and nothing may be read past it. */
   memset(&upload, 0, sizeof(upload));
   memset(captured, 0, sizeof(captured));
   {
      struct __IOSurface *s = surface_new(10, 0x42475241u /* 'BGRA' */, 128, 4,
                                          512, 4, 1024);

      check("a short allocation is copied short, never read past",
            surface_copy_texture(&eagl, s, 0, 0, GL_RGBA8, GL_BGRA,
                                 GL_UNSIGNED_BYTE, NO, "-t") == YES);
      check("  the two rows it holds are there",
            captured[0] == s->bytes[0] && captured[1023] == s->bytes[1023]);
   }

   /* 5. A padded row: 3x2, 4 bytes a pixel, 20-byte stride. */
   memset(&upload, 0, sizeof(upload));
   memset(captured, 0, sizeof(captured));
   {
      struct __IOSurface *s = surface_new(7, 0x42475241u /* 'BGRA' */, 3, 2, 20,
                                          4, 40);

      check("padded BGRA copy packs the rows",
            surface_copy_texture(&eagl, s, 3, 2, GL_RGBA8, GL_BGRA,
                                 GL_UNSIGNED_BYTE, NO, "-t") == YES);
      check("  row 0 is the surface's row 0",
            captured[0] == s->bytes[0] && captured[11] == s->bytes[11]);
      check("  row 1 starts at the padded stride",
            captured[12] == s->bytes[20] && captured[23] == s->bytes[31]);
   }

   /* 6. invert: the surface's last row is the texture's first. */
   memset(&upload, 0, sizeof(upload));
   memset(captured, 0, sizeof(captured));
   {
      struct __IOSurface *s = surface_new(9, 0x4c303038u, 4, 3, 4, 1, 12);

      check("invert flips the rows",
            surface_copy_texture(&eagl, s, 4, 3, GL_ALPHA, GL_ALPHA,
                                 GL_UNSIGNED_BYTE, YES, "-t") == YES);
      check("  first row of the texture is the last of the surface",
            captured[0] == s->bytes[8] && captured[3] == s->bytes[11]);
   }

   /* 7. Nothing usable at all: refused, not guessed. */
   memset(&upload, 0, sizeof(upload));
   {
      struct __IOSurface *s = surface_new(8, 0x76303234u, 7, 3, 9, 0, 27);

      check("a surface with no derivable pixel size is refused",
            surface_copy_texture(&eagl, s, 0, 0, 0, 0, 0, NO, "-t") == NO);
      check("  and nothing was uploaded", !upload.called);
   }

   printf("\n%s\n", failures ? "FAILURES" : "all checks passed");
   return failures ? 1 : 0;
}
