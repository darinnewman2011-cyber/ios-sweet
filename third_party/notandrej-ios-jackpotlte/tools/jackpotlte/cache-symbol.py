#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Name an address inside a dylib of the phone's dyld shared cache.

   cache-symbol.py CACHE_DIR --images            list images with an address
   cache-symbol.py CACHE_DIR IMAGE_VMADDR ADDR   the export at or below ADDR
   cache-symbol.py CACHE_DIR --offset NAME OFF   same, by image name and offset

CACHE_DIR holds the cache files of `dyld_shared_cache_arm64*` (split caches are
fine: eagl-slots.py's own reader is reused, which is what reads the cache in
place).  The export trie only carries global symbols, so a static function has
no name here; what this does give is the exported function an offset lands in
or just after, which is what turns `IOSurface + 0x4ffc` into a name.
"""
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def load_module():
    """The cache reader eagl-slots.py already has (dir_nodes, Cache, Image)."""
    path = os.path.join(HERE, "eagl-slots.py")
    spec = importlib.util.spec_from_file_location("eagl_slots", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main(argv):
    if len(argv) < 3:
        return __doc__
    es = load_module()
    cache_dir, rest = argv[1], argv[2:]
    cache = es.Cache(es.dir_nodes(cache_dir))

    if rest[0] == "--images":
        for name, addr in sorted(cache.images.items(), key=lambda kv: kv[1]):
            print("%#012x %s" % (addr, name))
        print("%d images, %d mapped ranges" % (len(cache.images), len(cache.maps)))
        return 0

    offset_mode = rest[0] == "--offset"
    if offset_mode:
        name, offset = rest[1], int(rest[2], 0)
        if name not in cache.images:
            return "no image named %s" % name
        image = es.Image(cache, name)
    else:
        wanted, address = int(rest[0], 0), int(rest[1], 0)
        name = None
        for candidate, base in cache.images.items():
            if base == wanted:
                name = candidate
                break
        if name is None:
            return "no image at %#x (try --images)" % wanted
        image = es.Image(cache, name)
        offset = address - wanted

    exports = image.export_list()
    at = image.base + offset
    print("image  %s" % name)
    print("base   %#x" % image.base)
    print("address %#x (offset %#x)" % (at, offset))
    for seg, (vm, vs, fo) in sorted(image.segs.items()):
        if vm <= at < vm + vs:
            print("in     %s (+%#x)" % (seg, at - vm))

    below = [(addr, nm) for nm, addr in exports.items() if addr <= at]
    below.sort()
    if not below:
        print("no export at or below")
        return 0
    addr, nm = below[-1]
    print("export %s (+%#x from image base)" % (nm, addr - image.base))
    if at != addr:
        print("       %#x bytes into it; next export %s" % (
            at - addr, min((a, n) for a, n in below if a > addr)[1]
            if any(a > addr for a, _ in below) else "(none above)"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))