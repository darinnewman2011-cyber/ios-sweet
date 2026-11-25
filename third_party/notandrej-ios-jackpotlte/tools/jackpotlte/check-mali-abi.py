#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check the M4 userspace ABI against the kernel's own declarations.

   check-mali-abi.py [REPO]

userspace/libjackpotlte/jackpotlte_gpu.h repeats the selectors, the structures
and the constants that iokit/Kernel/arm/JackpotlteMali.cpp (in
xnu/patches/0154) answers and reads.  Two copies of the same ABI drift: that is
not a risk, it is a certainty, and the failure mode is a wrong request on the
phone, which costs a flash to find.

So this does not grep for the numbers it hopes to see.  It lifts the enum
values, the struct bodies and the two #defines out of the patch, puts them in a
file next to our header, and compiles a translation unit of `_Static_assert`s
that every selector, every structure's size and every field's offset are the
same on both sides.  A moved field, a changed width or a renumbered request
stops the compile.

The get_param numbers are checked the other way, because they are a switch in
0153 and a runtime question there: the literals the kernel answers must be the
ones the header names (0153:251-295).
"""
import os
import re
import subprocess
import sys
import tempfile

PATCH_0154 = "xnu/patches/0154-jackpotlte-mali-user-client.patch"
PATCH_0153 = "xnu/patches/0153-jackpotlte-mali-engine.patch"
HEADER = "userspace/libjackpotlte/jackpotlte_gpu.h"

# The kernel's structure -> ours.  The fields keep their names, which is what
# lets the offsets be compared one by one.
STRUCTS = [
    ("drm_gem_close", "jt_gem_close"),
    ("drm_panfrost_submit", "jt_submit_req"),
    ("drm_panfrost_wait_bo", "jt_wait_bo_req"),
    ("drm_panfrost_create_bo", "jt_create_bo_req"),
    ("drm_panfrost_mmap_bo", "jt_mmap_bo_req"),
    ("drm_panfrost_get_param", "jt_get_param_req"),
    ("drm_panfrost_get_bo_offset", "jt_get_bo_offset_req"),
    ("drm_panfrost_madvise", "jt_madvise_req"),
    ("jackpotlte_surface", "jt_surface_req"),
    ("jackpotlte_wait", "jt_wait_req"),
]

# The kernel's selector -> ours, so the two enums must agree value for value.
SELECTORS = [
    ("kDrmGemClose", "JT_DRM_GEM_CLOSE"),
    ("kPanfrostSubmit", "JT_PANFROST_SUBMIT"),
    ("kPanfrostWaitBO", "JT_PANFROST_WAIT_BO"),
    ("kPanfrostCreateBO", "JT_PANFROST_CREATE_BO"),
    ("kPanfrostMmapBO", "JT_PANFROST_MMAP_BO"),
    ("kPanfrostGetParam", "JT_PANFROST_GET_PARAM"),
    ("kPanfrostGetBOOffset", "JT_PANFROST_GET_BO_OFFSET"),
    ("kPanfrostMadvise", "JT_PANFROST_MADVISE"),
    ("kJackpotlteWait", "JT_WAIT"),
    ("kJackpotlteSurface", "JT_SURFACE"),
]

DEFINES = [
    ("PANFROST_JD_REQ_FS", "JT_JD_REQ_FS"),
    ("kSurfaceDescribe", "JT_SURFACE_DESCRIBE"),
]

# Header enum name -> the number 0153's switch answers it at.
PARAMS = [
    ("JT_PARAM_GPU_ID_HI", "case 0:"),
    ("JT_PARAM_GPU_ID_LO", "case 1:"),
    ("JT_PARAM_SHADER_PRESENT", "case 2:"),
    ("JT_PARAM_SHADER_READY", "case 3:"),
    ("JT_PARAM_TILER_PRESENT", "case 4:"),
    ("JT_PARAM_NR_CORE_GROUPS", "case 38:"),
    ("JT_PARAM_THREAD_TLS_ALLOC", "case 39:"),
    ("JT_PARAM_AFBC_FEATURES", "case 40:"),
    ("JT_PARAM_FIRST_REGISTER", "simple[which]"),
    ("JT_PARAM_FIRST_READ", "which >= 22 && which <= 37"),
]


def added_lines(path):
    """The '+' side of a patch, with the marker stripped."""
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        return [line[1:] for line in handle.read().splitlines() if line.startswith("+")]


def find_struct(lines, name):
    """The body of `struct NAME { ... };` as lines, or None."""
    start = None
    for i, line in enumerate(lines):
        if re.match(r"\s*struct\s+%s\s*\{" % re.escape(name), line):
            start = i
            break
    if start is None:
        return None
    body = []
    for line in lines[start:]:
        body.append(line)
        if line.rstrip().endswith("};"):
            return body
    return None


def find_enum(lines, first_name):
    """The body of the enum holding `first_name`, as lines, or None."""
    start = None
    for i, line in enumerate(lines):
        if re.match(r"\s*enum\s*\{", line):
            start = i
            break
    if start is None:
        return None
    body = []
    for line in lines[start:]:
        body.append(line)
        if line.rstrip().endswith("};"):
            return body if any(first_name in l for l in body) else None
    return None


def find_define(lines, name):
    pattern = re.compile(r"^\s*#define\s+%s\b" % re.escape(name))
    for line in lines:
        if pattern.match(line):
            return line.strip()
    return None


def fields_of(struct_lines):
    """The declared field names in a struct body, in order."""
    body = " ".join(l.strip() for l in struct_lines)
    body = body[body.index("{") + 1:]
    body = body[:body.rindex("}")]
    # Comments first: theirs hold semicolons and commas, which would otherwise
    # be read as declarators.
    body = re.sub(r"/\*.*?\*/", " ", body)
    names = []
    for declarator in body.split(";"):
        declarator = declarator.strip()
        if not declarator:
            continue
        # No struct here has a pointer or a nested type, so the names are the
        # identifiers after the type's last token.
        tokens = re.findall(r"[A-Za-z_][A-Za-z0-9_]*", declarator)
        tail = []
        for token in reversed(tokens):
            if token in ("uint32_t", "uint64_t", "int64_t", "int32_t", "unsigned", "int"):
                break
            tail.append(token)
        names.extend(reversed(tail))
    return names


def main(repo):
    p154 = os.path.join(repo, PATCH_0154)
    p153 = os.path.join(repo, PATCH_0153)
    header = os.path.join(repo, HEADER)
    for path in (p154, p153, header):
        if not os.path.exists(path):
            print("error: %s is not there" % path)
            return 2

    lines = added_lines(p154)

    # 1. The kernel's own declarations, put in a file of their own.  Only what
    #    is asserted about is copied, so the file stays readable.
    abi = ["#include <stdint.h>", "#include <stddef.h>", ""]
    enum_body = find_enum(lines, "kDrmGemClose")
    if enum_body is None:
        print("error: the selector enum is not in %s" % PATCH_0154)
        return 2
    abi += enum_body
    abi.append("")
    for kernel_name, _ in STRUCTS:
        body = find_struct(lines, kernel_name)
        if body is None:
            print("error: struct %s is not in %s" % (kernel_name, PATCH_0154))
            return 2
        abi += body
        abi.append("")
    for kernel_name, _ in DEFINES:
        define = find_define(lines, kernel_name)
        if define is None:
            print("error: #define %s is not in %s" % (kernel_name, PATCH_0154))
            return 2
        abi.append(define)

    checks = [
        "/* Generated by tools/jackpotlte/check-mali-abi.py - do not edit. */",
        "#include \"kernel_abi.h\"",
        "#include \"jackpotlte_gpu.h\"",
        "",
    ]
    for kernel, ours in SELECTORS:
        # The cast is not decoration: both are unnamed enums, and comparing two
        # of them is a warning, which this build turns into an error.
        checks.append('_Static_assert((long) %s == (long) %s, "%s");' % (kernel, ours, kernel))
    for kernel, ours in DEFINES:
        checks.append('_Static_assert(%s == %s, "%s");' % (kernel, ours, kernel))
    checks.append("")
    for kernel, ours in STRUCTS:
        checks.append('_Static_assert(sizeof(struct %s) == sizeof(struct %s), "sizeof %s");'
                      % (kernel, ours, kernel))
        body = find_struct(lines, kernel)
        for field in fields_of(body):
            checks.append('_Static_assert(offsetof(struct %s, %s) == offsetof(struct %s, %s), "%s.%s");'
                          % (kernel, field, ours, field, kernel, field))
        checks.append("")

    with tempfile.TemporaryDirectory(prefix="mali-abi-") as tmp:
        with open(os.path.join(tmp, "kernel_abi.h"), "w") as handle:
            handle.write("\n".join(abi) + "\n")
        with open(os.path.join(tmp, "check.c"), "w") as handle:
            handle.write("\n".join(checks) + "\nint main(void) { return 0; }\n")

        compiler = os.environ.get("CC", "cc")
        command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                   "-fsyntax-only", "-I" + tmp, "-I" + os.path.dirname(header),
                   os.path.join(tmp, "check.c")]
        done = subprocess.run(command, capture_output=True, text=True)
        if done.returncode != 0:
            print("FAIL: the userspace ABI no longer matches %s" % PATCH_0154)
            print(done.stderr.strip())
            for line in checks:
                print("   " + line)
            return 1

    # 2. The get_param numbers, against the switch that answers them.
    with open(p153, "r", encoding="utf-8", errors="replace") as handle:
        kernel_0153 = handle.read()
    with open(header, "r", encoding="utf-8", errors="replace") as handle:
        our_header = handle.read()

    bad = []
    for name, literal in PARAMS:
        if literal not in kernel_0153:
            bad.append("%s: 0153 has no %r" % (name, literal))
        if not re.search(r"\b%s\b" % re.escape(name), our_header):
            bad.append("%s: the header no longer names it" % name)
    if bad:
        print("FAIL: the get_param numbers and 0153 disagree")
        for line in bad:
            print("   " + line)
        return 1

    print("OK: %d selectors, %d structures and %d constants are identical in"
          " %s and %s; %d get_param numbers agree with %s"
          % (len(SELECTORS), len(STRUCTS), len(DEFINES),
             os.path.basename(header), os.path.basename(p154), len(PARAMS),
             os.path.basename(p153)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "."))
