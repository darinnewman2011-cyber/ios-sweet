/*
 * Copyright © 2026 Darwin on redwood contributors
 * SPDX-License-Identifier: MIT
 *
 * A persistent cache in front of the AIR -> NIR translator.
 *
 * Mesa's own disk cache keeps the *backend* compile (NIR -> GPU binary), but on a hit the AIR
 * module was still read, verified and translated again every time a pipeline was made: 11 s of
 * backboardd's start-up for its catch-all fragment shader on the A8 even when Mesa's cache had
 * the binary. Here the translator's whole output -- the NIR (nir_serialize) and the
 * air_shader_info the runtime builds its bindings from -- is stored under a key made of
 * everything that decides it: the module's bytes, the entry point, every function constant and
 * the wrapper flag, plus a version and this build's time (any new driver build starts a new
 * cache). On a hit nothing of the module is parsed, and the NIR that comes back is the same NIR,
 * so Mesa's backend cache hits as well.
 *
 * Entries are plain files under MESA_SHADER_CACHE_DIR/air2nir-<build> (/private/var/tmp/sweet-mesa-cache
 * on the phone; see store_get). AIR_NIR_CACHE=0 turns it off.
 */
#include "air_private.h"

#include <stdlib.h>
#include <string.h>

#include "compiler/nir/nir_serialize.h"
#include "util/blob.h"
#include "util/mesa-sha1.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include "util/simple_mtx.h"
#include "util/u_atomic.h"
#include "util/macros.h"

#define AIR_CACHE_VERSION 1u

typedef unsigned char cache_key[20];

static simple_mtx_t cache_lock = SIMPLE_MTX_INITIALIZER;
static char cache_dir[512];        /* "" = off */
static bool cache_tried;
static uint64_t stat_hits, stat_misses, stat_bad;

/* Our own one-file-per-entry store: Mesa's disk cache runs as a database here (every get opens and
 * locks its files and walks an index: ~4 ms per lookup even on a Mac), far too slow for a cache
 * that is asked for every pipeline. <dir>/air2nir-<build>/<2 hex>/<38 hex>, written via rename. */
static void
remove_tree2(const char *path)
{
   DIR *d = opendir(path);
   if (d) {
      struct dirent *e;
      while ((e = readdir(d))) {
         if (e->d_name[0] == '.')
            continue;
         char sub[1024];
         snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
         DIR *d2 = opendir(sub);
         if (d2) {
            struct dirent *f;
            while ((f = readdir(d2))) {
               if (f->d_name[0] == '.')
                  continue;
               char file[1280];
               snprintf(file, sizeof(file), "%s/%s", sub, f->d_name);
               unlink(file);
            }
            closedir(d2);
            rmdir(sub);
         } else {
            unlink(sub);
         }
      }
      closedir(d);
   }
   rmdir(path);
}

static const char *
get_dir(void)
{
   simple_mtx_lock(&cache_lock);
   if (!cache_tried) {
      cache_tried = true;
      const char *env = getenv("AIR_NIR_CACHE");
      const char *base = getenv("MESA_SHADER_CACHE_DIR");
      if ((!env || strcmp(env, "0") != 0) && base && *base) {
         /* this build's tag: a new driver build starts a new cache and removes the old ones */
         unsigned char h[20];
         char tag[17];
         static const char id[] = "air2nir " __DATE__ " " __TIME__;
         _mesa_sha1_compute(id, sizeof(id), h);
         for (unsigned k = 0; k < 8; k++)
            snprintf(tag + 2 * k, 3, "%02x", h[k]);
         mkdir(base, 0755);
         DIR *d = opendir(base);
         if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
               if (!strncmp(e->d_name, "air2nir-", 8) && strcmp(e->d_name + 8, tag) != 0) {
                  char old[600];
                  snprintf(old, sizeof(old), "%s/%s", base, e->d_name);
                  remove_tree2(old);
               }
            }
            closedir(d);
         }
         snprintf(cache_dir, sizeof(cache_dir), "%s/air2nir-%s", base, tag);
         if (mkdir(cache_dir, 0755) != 0 && errno != EEXIST)
            cache_dir[0] = 0;
      }
   }
   simple_mtx_unlock(&cache_lock);
   return cache_dir[0] ? cache_dir : NULL;
}

static void
entry_path(const char *dir, const cache_key key, char *out, size_t n, bool make_dir)
{
   char hex[41];
   for (unsigned k = 0; k < 20; k++)
      snprintf(hex + 2 * k, 3, "%02x", key[k]);
   snprintf(out, n, "%s/%.2s", dir, hex);
   if (make_dir)
      mkdir(out, 0755);
   snprintf(out, n, "%s/%.2s/%s", dir, hex, hex + 2);
}

static void *
store_get(const char *dir, const cache_key key, size_t *size)
{
   char path[700];
   entry_path(dir, key, path, sizeof(path), false);
   int fd = open(path, O_RDONLY | O_CLOEXEC);
   if (fd < 0)
      return NULL;
   struct stat st;
   void *buf = NULL;
   if (fstat(fd, &st) == 0 && st.st_size > 0 && st.st_size < (64 << 20)) {
      buf = malloc(st.st_size);
      size_t got = 0;
      while (buf && got < (size_t)st.st_size) {
         ssize_t r = read(fd, (char *)buf + got, st.st_size - got);
         if (r <= 0)
            break;
         got += r;
      }
      if (buf && got != (size_t)st.st_size) {
         free(buf);
         buf = NULL;
      }
      *size = got;
   }
   close(fd);
   return buf;
}

static void
store_put(const char *dir, const cache_key key, const void *data, size_t size)
{
   char path[700], tmp[760];
   entry_path(dir, key, path, sizeof(path), true);
   snprintf(tmp, sizeof(tmp), "%s.%d.%p", path, (int)getpid(), (void *)pthread_self());
   int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
   if (fd < 0)
      return;
   size_t put = 0;
   while (put < size) {
      ssize_t w = write(fd, (const char *)data + put, size - put);
      if (w <= 0)
         break;
      put += w;
   }
   close(fd);
   if (put == size)
      rename(tmp, path);
   else
      unlink(tmp);
}

static void
blob_write_str(struct blob *b, const char *s)
{
   blob_write_uint8(b, s != NULL);
   if (s)
      blob_write_string(b, s);
}

static const char *
blob_read_str(struct blob_reader *r, void *mem_ctx)
{
   if (!blob_read_uint8(r))
      return NULL;
   const char *s = blob_read_string(r);
   return r->overrun ? NULL : ralloc_strdup(mem_ctx, s);
}

/* One metallib module holds many functions and every pipeline passes the whole module, so its hash
 * is remembered by (address, size, first and last bytes) instead of hashing megabytes per call. */
#define MODULE_MEMO 64
static struct { const void *ptr; size_t size; uint64_t head, tail; cache_key hash; } memo[MODULE_MEMO];
static unsigned memo_next;

static void
module_hash(const void *bitcode, size_t size, cache_key out)
{
   uint64_t head = 0, tail = 0;
   memcpy(&head, bitcode, MIN2(size, sizeof(head)));
   if (size >= sizeof(tail))
      memcpy(&tail, (const uint8_t *)bitcode + size - sizeof(tail), sizeof(tail));
   simple_mtx_lock(&cache_lock);
   for (unsigned i = 0; i < MODULE_MEMO; i++) {
      if (memo[i].ptr == bitcode && memo[i].size == size && memo[i].head == head && memo[i].tail == tail) {
         memcpy(out, memo[i].hash, sizeof(cache_key));
         simple_mtx_unlock(&cache_lock);
         return;
      }
   }
   simple_mtx_unlock(&cache_lock);
   _mesa_sha1_compute(bitcode, size, out);
   simple_mtx_lock(&cache_lock);
   unsigned i = memo_next++ % MODULE_MEMO;
   memo[i].ptr = bitcode;
   memo[i].size = size;
   memo[i].head = head;
   memo[i].tail = tail;
   memcpy(memo[i].hash, out, sizeof(cache_key));
   simple_mtx_unlock(&cache_lock);
}

static void
make_key(const void *bitcode, size_t size, const char *entry_point,
         const struct air_to_nir_options *options, cache_key key)
{
   struct blob b;
   cache_key mod;
   module_hash(bitcode, size, mod);
   blob_init(&b);
   blob_write_uint32(&b, AIR_CACHE_VERSION);
   blob_write_uint64(&b, size);
   blob_write_bytes(&b, mod, sizeof(mod));
   blob_write_str(&b, entry_point);
   blob_write_uint8(&b, options ? options->library_wrapper : 0);
   unsigned nlinked = options ? options->num_linked : 0;   /* linked functions change the translation */
   blob_write_uint32(&b, nlinked);
   for (unsigned l = 0; l < nlinked; l++) {
      cache_key lk;
      module_hash(options->linked[l], options->linked_size[l], lk);
      blob_write_bytes(&b, lk, sizeof(lk));
   }
   unsigned n = options ? options->num_constants : 0;
   blob_write_uint32(&b, n);
   for (unsigned i = 0; i < n; i++) {
      const struct air_constant_value *v = &options->constants[i];
      blob_write_uint32(&b, v->index);
      blob_write_uint64(&b, v->bits);
      for (unsigned k = 0; k < 3; k++)
         blob_write_uint64(&b, v->hi[k]);
      blob_write_str(&b, v->name);
      blob_write_uint8(&b, v->undefined);
   }
   _mesa_sha1_compute(b.data, b.size, key);
   blob_finish(&b);
}

static void
write_info(struct blob *b, const struct air_shader_info *in)
{
   blob_write_uint32(b, in->stage);
   blob_write_str(b, in->entry_point);
   blob_write_uint32(b, in->num_resources);
   for (unsigned i = 0; i < in->num_resources; i++) {
      struct air_resource r = in->resources[i];
      const char *name = r.name, *type_name = r.type_name;
      r.name = r.type_name = NULL;
      blob_write_bytes(b, &r, sizeof(r));
      blob_write_str(b, name);
      blob_write_str(b, type_name);
   }
   blob_write_uint32(b, in->num_varyings);
   for (unsigned i = 0; i < in->num_varyings; i++) {
      struct air_varying v = in->varyings[i];
      const char *name = v.name;
      v.name = NULL;
      blob_write_bytes(b, &v, sizeof(v));
      blob_write_str(b, name);
   }
   blob_write_uint32(b, in->num_vertex_attributes);
   for (unsigned i = 0; i < in->num_vertex_attributes; i++) {
      struct air_vertex_attribute a = in->vertex_attributes[i];
      const char *name = a.name, *type_name = a.type_name, *fc = a.function_constant;
      a.name = a.type_name = a.function_constant = NULL;
      blob_write_bytes(b, &a, sizeof(a));
      blob_write_str(b, name);
      blob_write_str(b, type_name);
      blob_write_str(b, fc);
   }
   blob_write_uint32(b, in->vertex_attributes_read);
   blob_write_uint32(b, in->num_render_targets);
   blob_write_uint32(b, in->fbfetch_targets);
   blob_write_uint32(b, in->raster_order_groups);
   blob_write_uint32(b, in->num_notes);
   for (unsigned i = 0; i < in->num_notes; i++)
      blob_write_str(b, in->notes[i]);
   blob_write_uint32(b, in->threadgroup_bytes);
   blob_write_uint32(b, in->null_query_textures);
   blob_write_uint32(b, in->null_query_ubo);
}

static bool
read_info(struct blob_reader *r, void *mem_ctx, struct air_shader_info *out)
{
   memset(out, 0, sizeof(*out));
   out->stage = (gl_shader_stage)blob_read_uint32(r);
   out->entry_point = blob_read_str(r, mem_ctx);
   out->num_resources = blob_read_uint32(r);
   if (r->overrun || out->num_resources > 65536)
      return false;
   if (out->num_resources) {
      out->resources = rzalloc_array(mem_ctx, struct air_resource, out->num_resources);
      for (unsigned i = 0; i < out->num_resources; i++) {
         blob_copy_bytes(r, &out->resources[i], sizeof(out->resources[i]));
         out->resources[i].name = blob_read_str(r, mem_ctx);
         out->resources[i].type_name = blob_read_str(r, mem_ctx);
      }
   }
   out->num_varyings = blob_read_uint32(r);
   if (r->overrun || out->num_varyings > 65536)
      return false;
   if (out->num_varyings) {
      out->varyings = rzalloc_array(mem_ctx, struct air_varying, out->num_varyings);
      for (unsigned i = 0; i < out->num_varyings; i++) {
         blob_copy_bytes(r, &out->varyings[i], sizeof(out->varyings[i]));
         out->varyings[i].name = blob_read_str(r, mem_ctx);
      }
   }
   out->num_vertex_attributes = blob_read_uint32(r);
   if (r->overrun || out->num_vertex_attributes > 65536)
      return false;
   if (out->num_vertex_attributes) {
      out->vertex_attributes =
         rzalloc_array(mem_ctx, struct air_vertex_attribute, out->num_vertex_attributes);
      for (unsigned i = 0; i < out->num_vertex_attributes; i++) {
         struct air_vertex_attribute *a = &out->vertex_attributes[i];
         blob_copy_bytes(r, a, sizeof(*a));
         a->name = blob_read_str(r, mem_ctx);
         a->type_name = blob_read_str(r, mem_ctx);
         a->function_constant = blob_read_str(r, mem_ctx);
      }
   }
   out->vertex_attributes_read = blob_read_uint32(r);
   out->num_render_targets = blob_read_uint32(r);
   out->fbfetch_targets = blob_read_uint32(r);
   out->raster_order_groups = blob_read_uint32(r);
   out->num_notes = blob_read_uint32(r);
   if (r->overrun || out->num_notes > 65536)
      return false;
   if (out->num_notes) {
      out->notes = ralloc_array(mem_ctx, const char *, out->num_notes);
      for (unsigned i = 0; i < out->num_notes; i++)
         out->notes[i] = blob_read_str(r, mem_ctx);
   }
   out->threadgroup_bytes = blob_read_uint32(r);
   out->null_query_textures = blob_read_uint32(r);
   out->null_query_ubo = blob_read_uint32(r);
   return !r->overrun;
}

void
air_nir_cache_stats(uint64_t out[3])
{
   out[0] = stat_hits;
   out[1] = stat_misses;
   out[2] = stat_bad;
}

nir_shader *
air_to_nir(void *mem_ctx, const void *bitcode, size_t size, const char *entry_point,
           const struct air_to_nir_options *options,
           const nir_shader_compiler_options *nir_options,
           struct air_shader_info *info, const char **error)
{
   const char *dc = bitcode && size ? get_dir() : NULL;
   cache_key key;

   if (dc) {
      make_key(bitcode, size, entry_point, options, key);
      size_t got = 0;
      void *data = store_get(dc, key, &got);
      if (data) {
         struct blob_reader r;
         struct air_shader_info cached;
         void *ctx = ralloc_context(mem_ctx);
         blob_reader_init(&r, data, got);
         bool ok = blob_read_uint32(&r) == AIR_CACHE_VERSION && read_info(&r, ctx, &cached);
         nir_shader *nir = ok ? nir_deserialize(ctx, nir_options, &r) : NULL;
         free(data);
         if (nir && !r.overrun) {
            ralloc_steal(mem_ctx, ctx);       /* info strings/arrays and the NIR live as long as mem_ctx */
            if (info)
               *info = cached;
            if (error)
               *error = NULL;
            p_atomic_inc(&stat_hits);
            return nir;
         }
         ralloc_free(ctx);
         p_atomic_inc(&stat_bad);             /* unreadable entry: translate and overwrite it */
      }
   }

   struct air_shader_info local = { 0 };
   nir_shader *nir = air_to_nir_uncached(mem_ctx, bitcode, size, entry_point, options, nir_options,
                                         info ? info : &local, error);
   if (dc && nir) {
      p_atomic_inc(&stat_misses);
      struct blob b;
      blob_init(&b);
      blob_write_uint32(&b, AIR_CACHE_VERSION);
      write_info(&b, info ? info : &local);
      nir_serialize(&b, nir, false);
      if (!b.out_of_memory)
         store_put(dc, key, b.data, b.size);
      blob_finish(&b);
   }
   return nir;
}
