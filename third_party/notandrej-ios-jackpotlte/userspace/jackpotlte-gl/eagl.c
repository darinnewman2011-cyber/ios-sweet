/* SPDX-License-Identifier: MIT */
/*
 * libjackpotlte-gl.dylib: an OpenGL ES implementation behind iOS's EAGLContext,
 * for the machine CoreAnimation draws on when it has no Metal.
 *
 * CoreAnimation's render server draws with Metal, or else with OpenGL ES, or
 * else with its own software rasteriser (CA::WindowServer::EAGLServer::renderer()).
 * With no Metal plug-in on this phone (docs/jackpotlte-mali.md section 6), it
 * reaches for OpenGL ES, and asks an EAGLContext for a handful of things.  All
 * of them are replaced here, so the context it gets is one of the library
 * behind this file (jackpotlte_gl_osmesa.c: Mesa's OSMesa with the phone's
 * Mali) rather than Apple's driver:
 *
 *   -initWithAPI:properties:   makes a context of the OpenGL ES version asked for
 *                              (and the -sharegroup and -sharedWithCompute:
 *                              forms of the same initialiser)
 *   -API                       that version
 *   -sharegroup                nil: nothing here shares a context (CoreImage asks;
 *                              its own accessor reads ivars of an object this
 *                              shim never initialised, so it must be answered)
 *   -setParameter:to:          noted; the value is not read
 *   -getParameter:to:          the same, in the other direction
 *   -getMacroContextPrivate    the table of functions the caller invokes as
 *                              entry(table[0], arguments...), at the offsets of
 *                              the build's own OpenGLES (eagl_slots.txt); entry
 *                              0 is this context, the rest are gl_slots.c's
 *   -texImageIOSurface:...     the texture bound to the target is to show what
 *                              the IOSurface holds (both forms the build has,
 *                              with and without `invert`)
 *   -attachImage:toCoreSurface:invertedRender:
 *                              the texture bound to the target is to be the
 *                              surface's memory (CoreAnimation then draws into
 *                              it), or to show it once
 *   -renderbufferStorage:fromDrawable:
 *                              noted, YES: whether this build takes that path
 *   -presentRenderbuffer:      (and its atTime: and afterMinimumDuration: forms)
 *                              the frame the caller has drawn is handed to the
 *                              GPU - see below
 *   +setCurrentContext:        noted; every GL entry already carries its context
 *   +currentContext            what they noted
 *   -dealloc                   the context goes
 *
 * Both surface methods go through the same path: the surface's ID is handed to
 * glEGLImageTargetTexture2DOES, which 0003's OSMesa frontend hook turns into a
 * buffer of that surface (0154's 0x7e) - no copy, the display's memory itself.
 * A surface that cannot be one (not one plane of 32-bit BGRA pixels) is logged
 * and refused rather than copied: a copy would be a different picture and a
 * different bug.
 *
 * The present methods are what makes a drawn frame exist.  Apple's context
 * presents by handing its renderbuffer to the window system; there is no
 * renderbuffer here, and the surface CoreAnimation drew into is composited by
 * the kernel's own display path (0046), so the part left to do is what Apple's
 * driver does first: get the batch to the GPU.  Mesa submits on a flush, and a
 * GL client that only draws submits nothing - which is what the jt60 log shows,
 * ending at glDrawElements with no job behind it.
 *
 * Nothing here is called by Apple's code paths that this shim does not replace:
 * the class's ivars are never filled in (there is no call to super's -init), so
 * every method CoreAnimation reaches must be one of the ones above.
 */
#include "jackpotlte_gl.h"

#include <fcntl.h>
#include <mach-o/dyld.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

/*
 * IOSurface's accessors, declared here: the iPhoneOS SDK ships no header for
 * them (the framework is there, the header is not), the same arrangement the
 * iOS backend uses for IOKit (userspace/libjackpotlte/jackpotlte_iokit.c).  The
 * names are the framework's and the process that loads this library has it.
 */
typedef struct __IOSurface *IOSurfaceRef;
uint32_t IOSurfaceGetID(IOSurfaceRef surface);
uint32_t IOSurfaceGetPixelFormat(IOSurfaceRef surface);
size_t IOSurfaceGetPlaneCount(IOSurfaceRef surface);
size_t IOSurfaceGetBytesPerElement(IOSurfaceRef surface);
size_t IOSurfaceGetBytesPerRow(IOSurfaceRef surface);
size_t IOSurfaceGetAllocSize(IOSurfaceRef surface);
size_t IOSurfaceGetWidth(IOSurfaceRef surface);
size_t IOSurfaceGetHeight(IOSurfaceRef surface);
void *IOSurfaceGetBaseAddress(IOSurfaceRef surface);
int IOSurfaceLock(IOSurfaceRef surface, uint32_t options, uint32_t *seed);
int IOSurfaceUnlock(IOSurfaceRef surface, uint32_t options, uint32_t *seed);

/* The GL names this file itself uses. */
#define GL_TEXTURE_2D             0x0de1
#define GL_TEXTURE_BINDING_2D     0x8069
#define GL_VENDOR                 0x1f00
#define GL_RENDERER               0x1f01
#define GL_VERSION                0x1f02

/* glTexImage2D's formats CoreAnimation hands -texImageIOSurface: (the surface's
 * own pixel format is not part of that call: these are). */
#define GL_ALPHA                  0x1906
#define GL_RGB                    0x1907
#define GL_RGBA                   0x1908
#define GL_BGRA                   0x80e1
#define GL_LUMINANCE              0x1909
#define GL_LUMINANCE_ALPHA        0x190a
#define GL_UNSIGNED_BYTE          0x1401
#define GL_UNSIGNED_SHORT_5_6_5   0x8363
#define GL_UNSIGNED_SHORT_4_4_4_4 0x8033
#define GL_UNSIGNED_SHORT_5_5_5_1 0x8034
#define GL_FLOAT                  0x1406
#define GL_HALF_FLOAT             0x140b
#define GL_RGBA8                  0x8058
#define GL_RGBA16F                0x881a
#define GL_RGBA32F                0x8814
#define GL_TEXTURE_MIN_FILTER     0x2801
#define GL_TEXTURE_MAG_FILTER     0x2800
#define GL_LINEAR                 0x2601

/* kIOSurfaceLockReadOnly. */
#define SURFACE_LOCK_READ_ONLY    0x1

/* As many contexts as the display server has been seen to make: the renderer's
 * own and CoreImage's.  A table this small keeps the lookup in context_of() a
 * handful of compares, and a full one is logged instead of growing. */
#define CONTEXTS 8

struct jackpotlte_eagl {
   void *table[JACKPOTLTE_GL_SLOTS];   /* [0] is this: what every entry is handed */
   id object;                          /* the EAGLContext this stands for */
   struct jackpotlte_gl *gl;
   unsigned long api;
};

static struct jackpotlte_eagl *contexts[CONTEXTS];

/* The library's functions this file calls itself (never by name: see
 * jackpotlte_gl_osmesa.c). */
static struct {
   void (*get_integerv)(unsigned pname, int *params);
   const unsigned char *(*get_string)(unsigned name);
} helpers;

static void
say(const char *format, ...)
{
   char line[512];
   va_list arguments;

   va_start(arguments, format);
   (void)vsnprintf(line, sizeof(line), format, arguments);
   va_end(arguments);
   fprintf(stderr, "jackpotlte-gl: %s\n", line);
   jackpotlte_gl_log(line);
}

/*
 * The log, doubled onto the kernel console when the boot-arg jtgllog=1 is
 * there: this library is loaded into the display server, whose stderr is not
 * anywhere a bring-up can read, while the kernel console is (it is what draws
 * a panic, and it is what the persistent ring keeps).  The switch is read once,
 * out of kern.bootargs; with it absent, nothing is opened and nothing extra is
 * written.  The kernel's own exec injector logs to the same switch.
 */
void
jackpotlte_gl_log(const char *line)
{
   static int looked, console = -1;

   if (!looked) {
      char args[1024];
      size_t size = sizeof(args);
      const char *p;

      looked = 1;
      if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && size > 0) {
         args[size < sizeof(args) ? size : sizeof(args) - 1] = '\0';
         for (p = strstr(args, "jtgllog="); p != NULL; p = strstr(p + 8, "jtgllog=")) {
            if (p[8] == '1' && (p[9] == '\0' || p[9] == ' ')) {
               console = open("/dev/console", O_WRONLY | O_APPEND);
               break;
            }
         }
      }
   }
   if (console >= 0) {
      char out[600];
      int n = snprintf(out, sizeof(out), "jackpotlte-gl: %s\n", line);

      if (n > 0) {
         (void)write(console, out,
             (size_t)(n < (int)sizeof(out) ? n : (int)sizeof(out) - 1));
      }
   }
}

/* A slot the build exports no _gl* name for was called (gl_slots.c). */
void
jackpotlte_gl_note_unsupported(unsigned offset)
{
   static unsigned noted;

   if (noted < 32) {
      say("slot %#06x has no name in this build: a call to it did nothing", offset);
      noted++;
   }
}

/*
 * Every entry of the table calls this before it passes its call on: the entry's
 * first argument is the context the caller means, and a GL call runs on the
 * thread where its context is current.  CoreAnimation's renderer is not always
 * the thread that made the context.
 */
void
jackpotlte_gl_use(void *context)
{
   struct jackpotlte_eagl *e = context;

   if (e != NULL && e->gl != NULL)
      jackpotlte_gl_make_current(e->gl);
}

/*
 * The dispatch table's drawing entries call this after the call is passed on.
 * Apple's driver submits a draw to the GPU as it is made; the library behind
 * this table keeps it in a batch until a flush, so a renderer that never
 * flushes (the display server, jt60: a full first frame drawn, no job to the
 * GPU) would leave its frame in the library's command stream forever.  One
 * flush per draw is expensive and is exactly what a bring-up wants: a drawn
 * frame becomes a real one, and the driver's own job log says so.
 */
void
jackpotlte_gl_after_draw(void)
{
   void (*flush)(void) = (void (*)(void))jackpotlte_gl_proc("glFlush");

   if (flush != NULL)
      flush();
}

static struct jackpotlte_eagl *
context_of(id object)
{
   unsigned i;

   for (i = 0; i < CONTEXTS; i++) {
      if (contexts[i] != NULL && contexts[i]->object == object)
         return contexts[i];
   }
   return NULL;
}

static int
resolve_helpers(void)
{
   if (helpers.get_integerv != NULL)
      return 1;
   helpers.get_integerv = (void (*)(unsigned, int *))
      jackpotlte_gl_proc("glGetIntegerv");
   helpers.get_string = (const unsigned char *(*)(unsigned))
      jackpotlte_gl_proc("glGetString");
   return helpers.get_integerv != NULL;
}

/* ---- EAGLContext --------------------------------------------------------- */

/* The one initialiser all three selectors end in. */
static id
make_context(id self, unsigned long api)
{
   struct jackpotlte_eagl *e;
   unsigned place;

   for (place = 0; place < CONTEXTS && contexts[place] != NULL; place++)
      ;
   if (api < 2 || api > 3 || place == CONTEXTS) {
      say("no context of API %lu (%u of %u in use)", api, place, (unsigned)CONTEXTS);
      return nil;
   }
   e = calloc(1, sizeof(*e));
   if (e == NULL)
      return nil;
   e->gl = jackpotlte_gl_create((int)api);
   if (e->gl == NULL || !jackpotlte_gl_make_current(e->gl)) {
      say("%s gives no OpenGL ES %lu context", jackpotlte_gl_library(), api);
      jackpotlte_gl_destroy(e->gl);
      free(e);
      return nil;
   }
   e->object = self;
   e->api = api;
   e->table[0] = e;
   jackpotlte_gl_fill_table(e->table);
   contexts[place] = e;

   if (resolve_helpers()) {
      say("an OpenGL ES %lu context for CoreAnimation by %s: %s; %s", api,
          jackpotlte_gl_library(), (const char *)helpers.get_string(GL_VERSION),
          (const char *)helpers.get_string(GL_RENDERER));
   } else {
      say("an OpenGL ES %lu context for CoreAnimation by %s; its glGetString is "
          "missing", api, jackpotlte_gl_library());
   }
   return self;
}

/*
 * Three ways in.  -initWithAPI:properties: is the one CoreAnimation's renderer
 * uses; the other two are the public ones, replaced so that a context made with
 * either is also one of ours - an object made by Apple's initialiser would have
 * none of this file's state, and -getMacroContextPrivate would answer NULL.
 */
static id
my_initWithAPI_properties(id self, SEL selector, unsigned long api, id properties)
{
   (void)selector;
   (void)properties;
   return make_context(self, api);
}

static id
my_initWithAPI(id self, SEL selector, unsigned long api)
{
   (void)selector;
   return make_context(self, api);
}

static id
my_initWithAPI_sharegroup(id self, SEL selector, unsigned long api, id sharegroup)
{
   (void)selector;
   (void)sharegroup;
   return make_context(self, api);
}

/* The third form: the same context, with a flag that asks for a sharegroup
 * that is only a compute context's.  Nothing here shares, so the flag is
 * noted by being ignored. */
static id
my_initWithAPI_sharedWithCompute(id self, SEL selector, unsigned long api,
                                 BOOL compute)
{
   (void)selector;
   (void)compute;
   return make_context(self, api);
}

static unsigned long
my_API(id self, SEL selector)
{
   struct jackpotlte_eagl *e = context_of(self);

   (void)selector;
   return e != NULL ? e->api : 0;
}

/*
 * Nothing here shares a context with anybody, and this object has none of
 * Apple's ivars, so nil is the only answer that is true and safe. CoreImage
 * makes a CIContext over an EAGLContext by asking this first.
 */
static id
my_sharegroup(id self, SEL selector)
{
   static int said;

   (void)self;
   (void)selector;
   if (!said) {
      said = 1;
      say("-sharegroup asked for: nil (nothing here shares a context)");
   }
   return nil;
}

/* The value is not read: what the caller passes it as has not been established
 * on this build, and a guess would be a read through it. */
static unsigned long
my_setParameter(id self, SEL selector, unsigned long parameter, void *value)
{
   static unsigned noted;

   (void)self;
   (void)selector;
   if (noted < 16) {
      say("-setParameter:%#lx to:%p", parameter, value);
      noted++;
   }
   return 0;
}

static void *
my_getMacroContextPrivate(id self, SEL selector)
{
   struct jackpotlte_eagl *e = context_of(self);

   (void)selector;
   return e != NULL ? e->table : NULL;
}

static unsigned long
my_getParameter(id self, SEL selector, unsigned long parameter, void *value)
{
   static unsigned noted;

   (void)self;
   (void)selector;
   (void)value;
   if (noted < 16) {
      say("-getParameter:%#lx to:%p: not answered", parameter, value);
      noted++;
   }
   return 0;
}

/* The name of the texture bound to GL_TEXTURE_2D, or 0. */
static int
bound_texture(struct jackpotlte_eagl *e)
{
   int name = 0;

   if (!resolve_helpers() || !jackpotlte_gl_make_current(e->gl))
      return 0;
   helpers.get_integerv(GL_TEXTURE_BINDING_2D, &name);
   return name;
}

/*
 * True when the surface's own memory can be the texture, with no copy: one
 * plane of 32-bit BGRA pixels.  That is the format the kernel's GPU driver can
 * map (0154's 0x7e) and the one CoreAnimation composites its own draws into, so
 * both -attachImage: and -texImageIOSurface: take the zero-copy path for it.
 */
static int
surface_shareable(IOSurfaceRef surface)
{
   return IOSurfaceGetPixelFormat(surface) == 'BGRA' &&
          IOSurfaceGetPlaneCount(surface) == 0 &&
          IOSurfaceGetBytesPerElement(surface) == 4;
}

/*
 * How many bytes one pixel of (format, type) is, for the formats CoreAnimation
 * passes -texImageIOSurface:.  0: not one of them.
 */
static size_t
bytes_a_pixel(unsigned long format, unsigned long type)
{
   if (type == GL_UNSIGNED_BYTE) {
      switch (format) {
      case GL_RGBA:
      case GL_BGRA:
         return 4;
      case GL_RGB:
         return 3;
      case GL_LUMINANCE_ALPHA:
         return 2;
      case GL_ALPHA:
      case GL_LUMINANCE:
         return 1;
      default:
         return 0;
      }
   }
   if (type == GL_FLOAT) {
      switch (format) {
      case GL_RGBA:
         return 16;
      case GL_LUMINANCE_ALPHA:
         return 8;
      case GL_ALPHA:
      case GL_LUMINANCE:
         return 4;
      default:
         return 0;
      }
   }
   if (type == GL_HALF_FLOAT) {
      switch (format) {
      case GL_RGBA:
         return 8;
      case GL_LUMINANCE_ALPHA:
         return 4;
      case GL_ALPHA:
      case GL_LUMINANCE:
         return 2;
      default:
         return 0;
      }
   }
   if (type == GL_UNSIGNED_SHORT_5_6_5 || type == GL_UNSIGNED_SHORT_4_4_4_4 ||
       type == GL_UNSIGNED_SHORT_5_5_5_1) {
      return 2;
   }
   return 0;
}

/*
 * A surface that cannot be a texture in place, made into one by copying it with
 * the format the caller asks for.  jt64's 12:05 log is why this exists:
 *
 *   -texImageIOSurface:: surface 6 is '..', 0 planes, 2 bytes a pixel: not a
 *       texture here
 *   -texImageIOSurface:: surface 5 is 'L008..', 0 planes, 2 bytes a pixel: not
 *       a texture here
 *
 * Those are not BGRA and so were refused, and a refused texture is not drawn:
 * the lock screen's clock (a 256x1 float gradient ramp) and its camera button
 * (an 8-bit luminance mask) came out as nothing and as a white square, while
 * the plain-BGRA status bar and home indicator drew.  A copy is the wrong thing
 * for a surface the caller draws *into* (-attachImage: keeps refusing), but for
 * one it only samples, the pixels are all that is wanted.
 */
static BOOL
surface_copy_texture(struct jackpotlte_eagl *e, IOSurfaceRef surface,
                     int width, int height, unsigned long internal,
                     unsigned long format, unsigned long type, BOOL invert,
                     const char *method)
{
   static void (*tex_image)(unsigned, int, int, int, int, int, unsigned,
                            unsigned, const void *);
   static void (*tex_parameter)(unsigned, unsigned, int);
   static unsigned (*get_error)(void);
   static unsigned char *packed;
   static size_t capacity;
   static uint32_t logged;
   size_t bpr;
   size_t bpp;
   size_t row;
   size_t total;
   size_t alloc;
   const unsigned char *base;
   const unsigned char *source;
   unsigned error;
   uint32_t seed = 0;
   uint32_t pixels = IOSurfaceGetPixelFormat(surface);
   int locked;

   if (!resolve_helpers() || !jackpotlte_gl_make_current(e->gl))
      return NO;
   if (tex_image == NULL) {
      tex_image = (void (*)(unsigned, int, int, int, int, int, unsigned,
                            unsigned, const void *))
         jackpotlte_gl_proc("glTexImage2D");
      tex_parameter = (void (*)(unsigned, unsigned, int))
         jackpotlte_gl_proc("glTexParameteri");
      get_error = (unsigned (*)(void))jackpotlte_gl_proc("glGetError");
      if (tex_image == NULL || get_error == NULL) {
         say("%s: the library has no glTexImage2D: surface %u is not a texture",
             method, (unsigned)IOSurfaceGetID(surface));
         return NO;
      }
   }
   if (width <= 0) {
      width = (int)IOSurfaceGetWidth(surface);
   }
   if (height <= 0) {
      height = (int)IOSurfaceGetHeight(surface);
   }
   bpr = IOSurfaceGetBytesPerRow(surface);
   bpp = bytes_a_pixel(format, type);
   if (bpp == 0) {
      /* Nothing usable was asked for, so the surface has to say.  In order of
       * how much each answer means: its stride (a row is a whole number of
       * pixels, or it is not this surface's stride at all), then its own bytes
       * a pixel, then the allocation.  jt64's surface 6 - 256x1, bytesPerElement
       * 2, bytesPerRow 672, 4096 allocated, pixelFormat 0 - is the case that
       * needs the second one: 672 is not 256 of anything, and the allocation's
       * 16 bytes a pixel is the page rounding, not the ramp. */
      size_t whole = IOSurfaceGetAllocSize(surface);
      size_t bpe = IOSurfaceGetBytesPerElement(surface);

      if (width > 0 && bpr % (size_t)width == 0) {
         bpp = bpr / (size_t)width;
      }
      if (bpp == 0 && bpe > 0 && bpe <= 16) {
         bpp = bpe;
      }
      if (bpp == 0 && width > 0 && height > 0 && whole != 0 &&
          whole % ((size_t)width * (size_t)height) == 0) {
         bpp = whole / ((size_t)width * (size_t)height);
      }
      if (bpp == 0) {
         say("%s: surface %u is '%.4s' %dx%d, row %zu, %zu B a pixel, %zu "
             "planes, %zu allocated, and asks for format %#lx type %#lx, which "
             "is no texture this knows", method,
             (unsigned)IOSurfaceGetID(surface),
             (const char *)&(uint32_t){ __builtin_bswap32(pixels) }, width,
             height, bpr, bpe, IOSurfaceGetPlaneCount(surface), whole, format,
             type);
         return NO;
      }
   }
   /* The caller did not say what the pixels are, or said something the surface
    * cannot be: a whole number of bytes a pixel is then the decision. */
   if (format == 0 || type == 0) {
      switch (bpp) {
      case 16:
         format = GL_RGBA;
         type = GL_FLOAT;
         break;
      case 8:
         format = GL_RGBA;
         type = GL_HALF_FLOAT;
         break;
      case 4:
         format = GL_RGBA;
         type = GL_UNSIGNED_BYTE;
         break;
      case 2:
         format = GL_LUMINANCE_ALPHA;
         type = GL_UNSIGNED_BYTE;
         break;
      default:
         format = GL_ALPHA;
         type = GL_UNSIGNED_BYTE;
         break;
      }
   }
   row = (size_t)width * bpp;
   if (bpr < row && width > 0 && bpr % (size_t)width == 0) {
      /* The caller's idea of a pixel is wider than a row of the surface can
       * hold: believe the surface. */
      bpp = bpr / (size_t)width;
      row = (size_t)width * bpp;
      if (bpp == 1) {
         format = GL_ALPHA;
         type = GL_UNSIGNED_BYTE;
      }
   }
   if (internal == 0) {
      /* GL_RGBA8 for a byte BGRA surface, else the format itself. */
      internal = (format == GL_BGRA) ? GL_RGBA8 : format;
      if (bpp == 16) {
         internal = GL_RGBA32F;
      } else if (bpp == 8 && format == GL_RGBA) {
         internal = GL_RGBA16F;
      }
   }
   total = row * (size_t)height;

   locked = IOSurfaceLock(surface, SURFACE_LOCK_READ_ONLY, &seed) == 0;
   base = (const unsigned char *)IOSurfaceGetBaseAddress(surface);
   if (base == NULL || total == 0) {
      if (locked) {
         IOSurfaceUnlock(surface, SURFACE_LOCK_READ_ONLY, &seed);
      }
      say("%s: surface %u has no memory to copy", method,
          (unsigned)IOSurfaceGetID(surface));
      return NO;
   }
   if (bpr == 0) {
      bpr = row;
   }
   /* Always pack: the surface's rows and the texture's rows are not guaranteed
    * to be the same width, and a copy that reads past a row is a fault, while
    * one that copies less than a row is only a wrong picture. */
   if (total > capacity) {
      unsigned char *bigger = realloc(packed, total);

      if (bigger == NULL) {
         if (locked) {
            IOSurfaceUnlock(surface, SURFACE_LOCK_READ_ONLY, &seed);
         }
         say("%s: no room to pack surface %u (%zu bytes)", method,
             (unsigned)IOSurfaceGetID(surface), total);
         return NO;
      }
      packed = bigger;
      capacity = total;
   }
   /* Nothing is read past the surface's allocation.  bytesPerRow times the
    * height need not fit in it - jt64's surface 6 is 256x1 with bytesPerRow 672
    * and a 4096-byte allocation, which is more than 672 - so the last row of a
    * short allocation is copied short rather than read past. */
   alloc = IOSurfaceGetAllocSize(surface);
   for (int y = 0; y < height; y++) {
      size_t n = bpr < row ? bpr : row;
      size_t off = (size_t)(invert ? height - 1 - y : y) * bpr;
      unsigned char *out = packed + (size_t)y * row;

      memset(out, 0, row);
      if (alloc != 0 && off >= alloc) {
         continue;
      }
      if (alloc != 0 && n > alloc - off) {
         n = alloc - off;
      }
      memcpy(out, base + off, n);
   }
   source = packed;

   while (get_error() != 0) {
   }
   tex_image(GL_TEXTURE_2D, 0, (int)internal, width, height, 0, (unsigned)format,
             (unsigned)type, source);
   error = get_error();
   if (locked) {
      IOSurfaceUnlock(surface, SURFACE_LOCK_READ_ONLY, &seed);
   }
   if (error != 0) {
      say("%s: surface %u (%dx%d, row %zu, %zu B a pixel, '%.4s') copy left GL "
          "error %#x", method, (unsigned)IOSurfaceGetID(surface), width, height,
          bpr, bpp,
          (const char *)&(uint32_t){ __builtin_bswap32(pixels) }, error);
      return NO;
   }
   /* Complete the texture: without a mipmap level, the default minification
    * filter samples nothing, whatever was uploaded. */
   if (tex_parameter != NULL) {
      tex_parameter(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      tex_parameter(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   }
   if (logged++ < 16) {
      say("%s: surface %u is '%.4s' %dx%d, row %zu of %zu bytes, %zu B a pixel, "
          "%zu planes, %zu allocated, format %#lx type %#lx: copied into the "
          "texture", method, (unsigned)IOSurfaceGetID(surface),
          (const char *)&(uint32_t){ __builtin_bswap32(pixels) }, width, height,
          bpr, IOSurfaceGetBytesPerRow(surface), bpp,
          IOSurfaceGetPlaneCount(surface), IOSurfaceGetAllocSize(surface),
          format, type);
   }
   return YES;
}

/*
 * The texture bound to the target is to be the surface's memory.  Returns NO
 * when the surface is not one this can be done for, and says why.
 */
static BOOL
surface_is_texture(struct jackpotlte_eagl *e, IOSurfaceRef surface,
                   unsigned long target, const char *method)
{
   uint32_t pixels = IOSurfaceGetPixelFormat(surface);
   int texture = bound_texture(e);
   static unsigned shown;

   if (target != GL_TEXTURE_2D) {
      say("%s: target %#lx is not GL_TEXTURE_2D", method, target);
      return NO;
   }
   if (texture == 0) {
      say("%s: no texture is bound", method);
      return NO;
   }
   if (!surface_shareable(surface)) {
      say("%s: surface %u is '%.4s', %zu planes, %zu bytes a pixel: not a "
          "texture here", method, (unsigned)IOSurfaceGetID(surface),
          (const char *)&(uint32_t){ __builtin_bswap32(pixels) },
          IOSurfaceGetPlaneCount(surface), IOSurfaceGetBytesPerElement(surface));
      return NO;
   }
   if (!jackpotlte_gl_surface_texture(e->gl, (uint32_t)IOSurfaceGetID(surface)))
      return NO;
   if (shown++ < 16) {
      say("%s: texture %d is surface %u, %zu x %zu, memory shared", method,
          texture, (unsigned)IOSurfaceGetID(surface), IOSurfaceGetWidth(surface),
          IOSurfaceGetHeight(surface));
   }
   return YES;
}

/*
 * A surface handed to a *sampling* call: in place when it can be (one plane of
 * BGRA), and as a copy in the format the caller asked for when it cannot.
 */
static BOOL
my_texImageIOSurface(id self, SEL selector, IOSurfaceRef surface,
                     unsigned long target, unsigned long internal, int width,
                     int height, unsigned long format, unsigned long type,
                     unsigned int plane, BOOL invert)
{
   struct jackpotlte_eagl *e = context_of(self);

   (void)selector;
   (void)plane;
   (void)invert;
   if (e == NULL || surface == NULL)
      return NO;
   if (target != GL_TEXTURE_2D) {
      say("-texImageIOSurface: target %#lx is not GL_TEXTURE_2D", target);
      return NO;
   }
   if (bound_texture(e) == 0) {
      say("-texImageIOSurface: no texture is bound");
      return NO;
   }
   if (surface_shareable(surface))
      return surface_is_texture(e, surface, target, "-texImageIOSurface:");
   return surface_copy_texture(e, surface, width, height, internal, format, type,
                               invert, "-texImageIOSurface:");
}

static BOOL
my_attachImage(id self, SEL selector, unsigned long target, IOSurfaceRef surface,
               BOOL inverted)
{
   struct jackpotlte_eagl *e = context_of(self);

   (void)selector;
   (void)inverted;
   if (e == NULL || surface == NULL)
      return NO;
   return surface_is_texture(e, surface, target, "-attachImage:");
}

/* The same surface method without `invert`, which the build also has.  The
 * caller means the same thing; the flag only says how the surface's rows are
 * handed to the sampler, and Apple's own header spells both. */
static BOOL
my_texImageIOSurface_plane(id self, SEL selector, IOSurfaceRef surface,
                           unsigned long target, unsigned long internal,
                           int width, int height, unsigned long format,
                           unsigned long type, unsigned int plane)
{
   struct jackpotlte_eagl *e = context_of(self);

   (void)selector;
   (void)plane;
   if (e == NULL || surface == NULL)
      return NO;
   if (target != GL_TEXTURE_2D) {
      say("-texImageIOSurface:...plane: target %#lx is not GL_TEXTURE_2D",
          target);
      return NO;
   }
   if (bound_texture(e) == 0) {
      say("-texImageIOSurface:...plane: no texture is bound");
      return NO;
   }
   if (surface_shareable(surface))
      return surface_is_texture(e, surface, target,
                                "-texImageIOSurface:...plane:");
   return surface_copy_texture(e, surface, width, height, internal, format, type,
                               NO, "-texImageIOSurface:...plane:");
}

/*
 * -presentRenderbuffer: and the two timed forms.  What a present has to do
 * here is the flush: the library's driver submits a batch to the GPU on one,
 * and CoreAnimation draws into an IOSurface that the kernel's display path
 * composites by itself, so there is nothing to swap and nowhere to copy - the
 * frame exists once the GPU has written it.  A draw with no flush after it
 * leaves the batch in the library's command stream, which is what the jt60
 * log ends in (glDrawElements, then nothing, and no JackpotlteMali job).
 *
 * The first few presents are logged with the GL error, if any; after that one
 * in six hundred, which is enough to say a frame rate is going by.
 */
static BOOL
present_target(id self, SEL selector, unsigned long target, const char *method)
{
   struct jackpotlte_eagl *e = context_of(self);
   static unsigned presents;

   (void)selector;
   if (e == NULL) {
      if (presents++ < 8)
         say("%s: no context of ours", method);
      return NO;
   }
   jackpotlte_gl_make_current(e->gl);
   {
      void (*flush)(void) = (void (*)(void))jackpotlte_gl_proc("glFlush");
      unsigned (*get_error)(void) =
          (unsigned (*)(void))jackpotlte_gl_proc("glGetError");

      if (flush != NULL)
         flush();
      if (presents < 8) {
         unsigned error = get_error != NULL ? get_error() : 0;

         if (error != 0)
            say("%s: target %#lx, flush left GL error %#x", method, target, error);
         else
            say("%s: target %#lx, flushed", method, target);
      } else if (presents % 600 == 0) {
         say("%s: %u presents", method, presents + 1);
      }
   }
   presents++;
   return YES;
}

static BOOL
my_presentRenderbuffer(id self, SEL selector, unsigned long target)
{
   return present_target(self, selector, target, "-presentRenderbuffer:");
}

static BOOL
my_presentRenderbuffer_atTime(id self, SEL selector, unsigned long target,
                              double when)
{
   (void)when;
   return present_target(self, selector, target, "-presentRenderbuffer:atTime:");
}

static BOOL
my_presentRenderbuffer_afterMinimumDuration(id self, SEL selector,
                                            unsigned long target,
                                            double duration)
{
   (void)duration;
   return present_target(self, selector, target,
                         "-presentRenderbuffer:afterMinimumDuration:");
}

/*
 * -renderbufferStorage:fromDrawable: is the path where CoreAnimation draws
 * to the screen through the context instead of into a surface.  There is no
 * renderbuffer of Apple's here and saying NO could stop a renderer that would
 * otherwise draw, so this says YES and records the drawable: if the log shows
 * it, the surface path was not the one taken and the pixels of this one have
 * nowhere to go yet.
 */
static BOOL
my_renderbufferStorage(id self, SEL selector, unsigned long target, id drawable)
{
   static unsigned noted;

   (void)self;
   (void)selector;
   if (noted < 8) {
      say("-renderbufferStorage:%#lx fromDrawable:%s", target,
          drawable != nil ? class_getName(object_getClass(drawable)) : "nil");
      noted++;
   }
   return YES;
}

/*
 * +setCurrentContext: and +currentContext.  Apple's pair keeps the context in
 * the thread's own storage; here every GL entry already carries the context it
 * means as the table's first argument, so nothing reads this back - the two
 * only have to be consistent with each other and stay out of Apple's engine.
 */
static _Thread_local id current_context;

static void
my_setCurrentContext(id self, SEL selector, id context)
{
   (void)self;
   (void)selector;
   current_context = context;
}

static id
my_currentContext(id self, SEL selector)
{
   (void)self;
   (void)selector;
   return current_context;
}

/*
 * objc_msgSendSuper, read as what is wanted here.  <objc/message.h> declares it
 * as `void objc_msgSendSuper(void)` - Apple's form, which says the arguments
 * are the caller's to know - so this is the one place the two are written out:
 * the declared function pointer, and the call.
 */
static void
send_to_super(struct objc_super *super, SEL selector)
{
   union {
      void (*as_declared)(void);
      void (*as_called)(struct objc_super *, SEL);
   } send;

   send.as_declared = objc_msgSendSuper;
   send.as_called(super, selector);
}

static void
my_dealloc(id self, SEL selector)
{
   struct objc_super super = { self, class_getSuperclass((Class)objc_getClass("EAGLContext")) };
   struct jackpotlte_eagl *e = context_of(self);
   unsigned i;

   if (e != NULL) {
      for (i = 0; i < CONTEXTS; i++) {
         if (contexts[i] == e)
            contexts[i] = NULL;
      }
      jackpotlte_gl_destroy(e->gl);
      free(e);
   }
   send_to_super(&super, selector);
}

/*
 * The methods and the implementations that replace them.  A union, not a cast:
 * the runtime is handed an IMP and the implementations have their own
 * signatures, and reading one function pointer as another is what this says.
 */
union method_prototype {
   IMP imp;
   id (*initialise)(id, SEL, unsigned long, id);          /* with properties or a sharegroup */
   id (*initialise_simple)(id, SEL, unsigned long);
   id (*initialise_compute)(id, SEL, unsigned long, BOOL);
   unsigned long (*api)(id, SEL);
   id (*sharegroup)(id, SEL);
   unsigned long (*parameter)(id, SEL, unsigned long, void *);
   void *(*macro_context)(id, SEL);
   BOOL (*image)(id, SEL, IOSurfaceRef, unsigned long, unsigned long, int, int,
                 unsigned long, unsigned long, unsigned int, BOOL);
   BOOL (*image_plane)(id, SEL, IOSurfaceRef, unsigned long, unsigned long, int,
                       int, unsigned long, unsigned long, unsigned int);
   BOOL (*attach)(id, SEL, unsigned long, IOSurfaceRef, BOOL);
   BOOL (*present)(id, SEL, unsigned long);
   BOOL (*present_time)(id, SEL, unsigned long, double);
   BOOL (*storage)(id, SEL, unsigned long, id);
   void (*set_context)(id, SEL, id);
   id (*get_context)(id, SEL);
   void (*dispose)(id, SEL);
};

__attribute__((constructor)) static void
install(void)
{
   static const struct {
      const char *name;
      union method_prototype ours;
   } methods[] = {
      { "initWithAPI:properties:", { .initialise = my_initWithAPI_properties } },
      { "initWithAPI:", { .initialise_simple = my_initWithAPI } },
      { "initWithAPI:sharegroup:", { .initialise = my_initWithAPI_sharegroup } },
      { "initWithAPI:sharedWithCompute:",
        { .initialise_compute = my_initWithAPI_sharedWithCompute } },
      { "API", { .api = my_API } },
      { "sharegroup", { .sharegroup = my_sharegroup } },
      { "setParameter:to:", { .parameter = my_setParameter } },
      { "getParameter:to:", { .parameter = my_getParameter } },
      { "getMacroContextPrivate", { .macro_context = my_getMacroContextPrivate } },
      { "texImageIOSurface:target:internalFormat:width:height:format:type:plane:invert:",
        { .image = my_texImageIOSurface } },
      { "texImageIOSurface:target:internalFormat:width:height:format:type:plane:",
        { .image_plane = my_texImageIOSurface_plane } },
      { "attachImage:toCoreSurface:invertedRender:", { .attach = my_attachImage } },
      { "renderbufferStorage:fromDrawable:",
        { .storage = my_renderbufferStorage } },
      { "presentRenderbuffer:", { .present = my_presentRenderbuffer } },
      { "presentRenderbuffer:atTime:",
        { .present_time = my_presentRenderbuffer_atTime } },
      { "presentRenderbuffer:afterMinimumDuration:",
        { .present_time = my_presentRenderbuffer_afterMinimumDuration } },
      { "dealloc", { .dispose = my_dealloc } },
   };
   static const struct {
      const char *name;
      union method_prototype ours;
   } class_methods[] = {
      { "setCurrentContext:", { .set_context = my_setCurrentContext } },
      { "currentContext", { .get_context = my_currentContext } },
   };
   Class eagl = (Class)objc_getClass("EAGLContext");
   unsigned i, installed = 0;

   say("library loaded into the display server");
   /* Bring-up aid: where this file and the OpenGL ES library beside it were
    * loaded.  The kernel's crash report prints the faulting pc but no image
    * base, so these numbers are what turn that pc into an offset into a file
    * built here (a dylib's __TEXT is at vmaddr 0, so the header address is
    * the base).  dlsym is not used to find them: a function pointer cast to
    * void * is a pedantic-C warning, and -Werror is on. */
   for (uint32_t i = 0, n = _dyld_image_count(); i < n; i++) {
      const char *where = _dyld_get_image_name(i);
      const char *base;

      if (where == NULL ||
          (strstr(where, "OSMesa") == NULL && strstr(where, "jackpotlte") == NULL)) {
         continue;
      }
      base = strrchr(where, '/');
      say("image %p %s", (void *)_dyld_get_image_header(i),
          base != NULL ? base + 1 : where);
   }
   if (eagl == NULL) {
      say("no EAGLContext class: this library is loaded where OpenGL ES is not");
      return;
   }
   for (i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
      SEL selector = sel_registerName(methods[i].name);
      Method method = class_getInstanceMethod(eagl, selector);

      /* Only methods that are there: the implementations are installed, and the
       * encodings stay the class's own.  A selector iOS does not have is noted,
       * not added with a guess at its signature. */
      if (method != NULL) {
         method_setImplementation(method, methods[i].ours.imp);
         installed++;
      } else {
         say("EAGLContext has no -%s", methods[i].name);
      }
   }
   for (i = 0; i < sizeof(class_methods) / sizeof(class_methods[0]); i++) {
      SEL selector = sel_registerName(class_methods[i].name);
      Method method = class_getClassMethod(eagl, selector);

      if (method != NULL) {
         method_setImplementation(method, class_methods[i].ours.imp);
         installed++;
      } else {
         say("EAGLContext has no +%s", class_methods[i].name);
      }
   }
   say("EAGLContext is ours: %u of %u methods", installed,
       (unsigned)((sizeof(methods) + sizeof(class_methods)) /
                  sizeof(methods[0])));
}
