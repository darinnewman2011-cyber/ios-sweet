#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Write the C table of the EAGL shim, from the OpenGL ES dispatch table of a build.

  eagl-glue.py SLOTS GL.XML > gl_slots.c
  eagl-glue.py SLOTS GL.XML --header > gl_slots.h

Why this exists: iOS does not call OpenGL ES by name.  -[EAGLContext
getMacroContextPrivate] hands out a table of function pointers (one line per
entry in SLOTS, `userspace/jackpotlte-gl/eagl_slots.txt`), and the caller runs
`entry(table[0], arguments...)`.  Every entry has to be a function of ours that
drops the first argument (the context) and calls the library's function of that
name.

SLOTS is that file.  GL.XML is the Khronos registry (it ships with Mesa:
src/mapi/glapi/registry/gl.xml); it is where the arguments of each entry come
from, because the table only has names.  The two outputs are generated together
and both are checked in: this runs on a host with the Mesa tarball, and the
build that matters (arm64 iOS) has no Mesa source at hand.

The entry does not call the library's function by name, it asks jackpotlte_gl_proc
(the library's own OSMesaGetProcAddress) for it, once.  By name would bind the
wrong function: iOS's own OpenGLES.framework is in the shared cache and defines
every gl* symbol, so a direct call reaches Apple's driver, which knows nothing
of this context, not the library beside us.

The table is filled with two sorts of function:

  - one per named entry, which makes the entry's the calling thread's context
    (a GL call has to run where its context is current; the display server's
    renderer is not always the thread that made the context), resolves the name
    the entry carries (the first the registry knows, so an EXT/APPLE alias
    reaches the core call) and passes the arguments on.  If the library has
    nothing of that name the entry does nothing and returns zero;
  - an "unsupported" function for every other entry, which notes the entry and
    returns NULL.  iOS's table has entries no _gl* name is exported for, and a
    call to one is something to find out about rather than to crash on (the
    caller is handed a function, not a null pointer).
"""
import argparse
import re
import sys
import xml.etree.ElementTree as ET

GL_TYPEDEFS = """/* The OpenGL ES types, as the ARM64 ABI has them. */
typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef signed char GLbyte;
typedef short GLshort;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLubyte;
typedef unsigned short GLushort;
typedef unsigned int GLuint;
typedef float GLfloat;
typedef float GLclampf;
typedef double GLdouble;
typedef double GLclampd;
typedef void GLvoid;
typedef char GLchar;
typedef long GLintptr;
typedef long GLsizeiptr;
typedef long long GLint64;
typedef unsigned long long GLuint64;
typedef struct __GLsync *GLsync;
typedef unsigned short GLhalf;
typedef int GLfixed;
typedef int GLclampx;
typedef void *GLeglImageOES;
typedef void (*GLDEBUGPROC)(GLenum source, GLenum type, GLuint id, GLenum severity,
                            GLsizei length, const GLchar *message, const void *userParam);
typedef GLDEBUGPROC GLDEBUGPROCKHR;
"""

# Entries whose name is not in the registry, with the arguments and the name of
# the function that actually does it (they are APPLE spellings of core calls).
EXTRA = {
    "glFramebufferParameteriAPPLE":
        ("void", [("GLenum", "target"), ("GLenum", "pname"), ("GLint", "param")],
         "glFramebufferParameteri"),
    "glGetFramebufferParameteriAPPLE":
        ("void", [("GLenum", "target"), ("GLenum", "pname"), ("GLint *", "params")],
         "glGetFramebufferParameteri"),
}

# The calls that put pixels into a framebuffer.  Apple's driver hands each one
# to the GPU as it is made; the library behind this table keeps them in a batch
# and submits that on a flush.  A renderer that draws but never flushes would
# therefore have its frame drawn nowhere -- which is what jt60 showed
# (backboardd drew a whole first frame and no job ever reached the driver) --
# so these entries flush after the call.  It costs the batching and is a
# bring-up aid: what it buys is that a drawn frame becomes a real one.
SUBMIT = {
    "glBlitFramebuffer",
    "glClear",
    "glClearBufferfi",
    "glClearBufferfv",
    "glClearBufferiv",
    "glClearBufferuiv",
    "glDrawArrays",
    "glDrawArraysInstanced",
    "glDrawElements",
    "glDrawElementsInstanced",
    "glDrawRangeElements",
}


def parse_registry(path):
    """name -> (return type, [(type, name)])."""
    root = ET.parse(path).getroot()
    out = {}
    for cmd in root.iter("command"):
        proto = cmd.find("proto")
        if proto is None:
            continue
        name = (proto.findtext("name") or "").strip()
        if not name:
            continue
        ret = "".join(proto.itertext()).replace(name, "").strip()
        params = []
        for p in cmd.findall("param"):
            pname = (p.findtext("name") or "").strip()
            full = "".join(p.itertext()).strip()
            ptype = re.sub(r"\b%s\b" % re.escape(pname), "", full).strip()
            params.append((re.sub(r"\s+", " ", ptype), pname))
        out[name] = (ret, params)
    return out


def read_slots(path):
    """offset -> [names], from the generated table."""
    entries = {}
    for line in open(path):
        line = line.split("#")[0].split()
        if not line or not line[0].startswith("0x"):
            continue
        entries[int(line[0], 16)] = line[1:]
    if not entries:
        sys.exit("eagl-glue.py: no entries in the slots file")
    return entries


def param_decl(ptype, pname):
    return ptype + ("" if ptype.endswith("*") else " ") + pname if pname else ptype


def arg_types(params):
    return ", ".join(t for t, _ in params) or "void"


def gl_types(entries, chosen):
    """The GL type names the prototypes refer to, for the header's typedefs."""
    types = set()
    for off in entries:
        _, _, ret, params = chosen[off]
        for text in [ret] + [t for t, _ in params]:
            types.update(re.findall(r"\b(GL\w+)\b", text))
    return types


def choose(entries, registry):
    """offset -> (name the caller sees, name to resolve, return type, params)."""
    chosen = {}
    for off, names in sorted(entries.items()):
        for name in names:
            if name in registry:
                ret, params = registry[name]
                chosen[off] = (name, name, ret, params)
                break
            if name in EXTRA:
                ret, params, call = EXTRA[name]
                chosen[off] = (name, call, ret, params)
                break
        else:
            sys.exit("eagl-glue.py: no prototype for %s (entry %#x)" %
                     (" ".join(names), off))
    return chosen


def write_header(f, entries, chosen, slots):
    known = set(re.findall(r"\b(GL\w+)\b", GL_TYPEDEFS))
    unknown = sorted(gl_types(entries, chosen) - known)
    if unknown:
        sys.exit("eagl-glue.py: unknown GL type(s): %s" % ", ".join(unknown))
    f.write("""/* Generated by tools/jackpotlte/eagl-glue.py -- do not edit.               -*- c -*-
 *
 * The types and names of the OpenGL ES entry points of the phone's build, for
 * the dispatch table of -[EAGLContext getMacroContextPrivate]
 * (userspace/jackpotlte-gl/eagl_slots.txt).  The table's entries are called as
 * entry(table[0], arguments...), so the definitions of jackpotlte_gl_fill_table
 * wrap each one to drop the first argument.
 */
#ifndef JACKPOTLTE_GL_SLOTS_H
#define JACKPOTLTE_GL_SLOTS_H

#include <stdint.h>

""")
    f.write(GL_TYPEDEFS)
    f.write("""
/* Entries of the table: entry 0 is the context, the rest are the API.  The
 * offsets read from the build run to %#06x. */
#define JACKPOTLTE_GL_SLOTS %d

/* Fill TABLE with the entries of the table, all of them.  Entry 0 (the
 * context) is the caller's to set. */
void jackpotlte_gl_fill_table(void *table[JACKPOTLTE_GL_SLOTS]);

/* A slot the build exports no _gl* name for was called.  */
void jackpotlte_gl_note_unsupported(unsigned offset);

#endif
""" % (max(entries), slots))


def write_source(f, entries, chosen, slots, gaps):
    f.write("""/* Generated by tools/jackpotlte/eagl-glue.py -- do not edit.               -*- c -*-
 *
 * The dispatch table of -[EAGLContext getMacroContextPrivate] for the phone's
 * build: one function per entry, which resolves the library's function of that
 * name and passes the call on (see gl_slots.h and tools/jackpotlte/eagl-glue.py).
 */
#include "jackpotlte_gl.h"

#include <stddef.h>

""")
    f.write("/* An entry iOS exports no _gl* name for: note it, hand the caller nothing. */\n")
    f.write("#define JGL_GAP(off)                                                          \\\n")
    f.write("   static void *jgl_gap_##off(void *context, ...)                             \\\n")
    f.write("   {                                                                          \\\n")
    f.write("      (void)context;                                                          \\\n")
    f.write("      jackpotlte_gl_note_unsupported(off);                                    \\\n")
    f.write("      return NULL;                                                            \\\n")
    f.write("   }\n")
    for off in gaps:
        f.write("JGL_GAP(%#06x)\n" % off)

    for off in sorted(entries):
        name, call, ret, params = chosen[off]
        args = ", ".join(n for _, n in params)
        index = off // 8
        f.write("""
typedef %s (*jgl_t_%04x)(%s);

static %s
jgl_%04x(void *context%s%s)
{
   static jgl_t_%04x real;

   jackpotlte_gl_use(context);
   if (real == NULL)
      real = (jgl_t_%04x)jackpotlte_gl_proc(%s);%s
   if (real == NULL)
      %s
   %sreal(%s);%s
}
""" % (ret if ret else "void", index, arg_types(params),
       ret if ret else "void", index,
       ", " if params else "", ", ".join(param_decl(t, n) for t, n in params),
       index, index, '"%s"' % call,
       "" if call == name else "\n   /* %s is spelled %s here */" % (name, call),
       "return;" if not ret or ret == "void" else "return 0;",
       "" if not ret or ret == "void" else "return ",
       args,
       "\n   /* %s draws: hand it to the GPU (see SUBMIT) */\n   "
       "jackpotlte_gl_after_draw();" % name if call in SUBMIT else ""))

    f.write("\nvoid\njackpotlte_gl_fill_table(void *table[JACKPOTLTE_GL_SLOTS])\n{\n")
    for off in sorted(entries):
        f.write("   table[%#x] = (void *)jgl_%04x;%s\n" % (
            off // 8, off // 8, "   /* %#06x %s */" % (off, " ".join(entries[off]))))
    f.write("\n")
    for off in gaps:
        f.write("   table[%#x] = (void *)jgl_gap_%#06x;\n" % (off // 8, off))
    f.write("}\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("slots", help="eagl_slots.txt (offset names...)")
    ap.add_argument("registry", help="GL.XML (the Khronos registry, from Mesa)")
    ap.add_argument("--header", action="store_true", help="write gl_slots.h instead")
    a = ap.parse_args()

    entries = read_slots(a.slots)
    chosen = choose(entries, parse_registry(a.registry))
    slots = max(entries) // 8 + 1
    named = set(entries)
    gaps = [i * 8 for i in range(1, slots) if i * 8 not in named]

    if a.header:
        write_header(sys.stdout, entries, chosen, slots)
    else:
        write_source(sys.stdout, entries, chosen, slots, gaps)
    print("eagl-glue.py: %d entries, %d slots, %d unsupported" % (
        len(entries), slots, len(gaps)), file=sys.stderr)


if __name__ == "__main__":
    main()
