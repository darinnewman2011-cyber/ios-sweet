/* Copyright (c) 2026 Darin. MIT License (see LICENSE); keep this notice. Written by Claude (Anthropic) for the ios-sweet Galaxy A8 port. */
/*
 * Call trace of the Metal driver's objects, in the window server only.
 *
 * G9 run 24 (true Metal-only): backboardd opens the device, compiles CoreAnimation's 14 pipelines, runs a warm-up
 * and then presents frames without making one more Metal call that this driver logs. So log every message its
 * objects receive: the first calls of each selector as they happen, each -respondsToSelector: answered NO, and
 * every 5 s the calls since the last summary. The last calls before it goes quiet say where CoreAnimation gave up.
 *
 * How: every instance method of a class (its own and its superclasses' up to NSObject) is kept under an alias
 * selector and replaced by _objc_msgForward; -forwardInvocation: logs the selector and invokes the alias. Slow,
 * which is fine for a window server that draws nothing. Not in any other process.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "m2m.h"

#define RWT_PREFIX      "rwtrace_"
static unsigned rwt_first = 2;  /* calls of one selector logged as they happen (AegirPoster: 6, it builds and drops renderers) */
#define RWT_SELECTORS   1024
#define RWT_LINES       700     /* of the kernel's 4000 m2m lines a boot */
#define RWT_RING        24

static pthread_mutex_t rwt_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { SEL sel; Class cls; unsigned calls, reported; } rwt_seen[RWT_SELECTORS];
static unsigned rwt_nseen, rwt_lines, rwt_total, rwt_total_reported;
static const char *rwt_ring[RWT_RING];
static unsigned rwt_ring_at;

static void rwt_say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void
rwt_say(const char *fmt, ...)
{
	va_list ap;

	if (__sync_fetch_and_add(&rwt_lines, 1) >= RWT_LINES) {
		return;
	}
	va_start(ap, fmt);
	m2m_vlog('I', "CALL", fmt, ap);
	va_end(ap);
}

static int
rwt_skip(const char *name)
{
	static const char *const skip[] = {
		"retain", "release", "autorelease", "dealloc", "retainCount", "class", "superclass", "self", "zone",
		"hash", "isEqual:", "isKindOfClass:", "isMemberOfClass:", "respondsToSelector:",
		"conformsToProtocol:", "methodSignatureForSelector:", "forwardInvocation:",
		"forwardingTargetForSelector:", "doesNotRecognizeSelector:", "description", "debugDescription",
		".cxx_destruct", ".cxx_construct", "_tryRetain", "_isDeallocating", "allowsWeakReference",
		"retainWeakReference", "isProxy", "copyWithZone:", "mutableCopyWithZone:", "finalize", NULL
	};

	if (strncmp(name, RWT_PREFIX, sizeof(RWT_PREFIX) - 1) == 0 || strncmp(name, "performSelector", 15) == 0) {
		return 1;
	}
	for (unsigned i = 0; skip[i] != NULL; i++) {
		if (strcmp(name, skip[i]) == 0) {
			return 1;
		}
	}
	return 0;
}

static SEL
rwt_alias(SEL sel)
{
	char name[256];

	snprintf(name, sizeof(name), RWT_PREFIX "%s", sel_getName(sel));
	return sel_registerName(name);
}

/* count one call; returns its number for this selector (1 = first), 0 when the table is full */
static unsigned
rwt_count(Class cls, SEL sel)
{
	unsigned n = 0;

	pthread_mutex_lock(&rwt_lock);
	rwt_total++;
	rwt_ring[rwt_ring_at++ % RWT_RING] = sel_getName(sel);
	for (unsigned i = 0; i < rwt_nseen; i++) {
		if (rwt_seen[i].sel == sel && rwt_seen[i].cls == cls) {
			n = ++rwt_seen[i].calls;
			break;
		}
	}
	if (n == 0 && rwt_nseen < RWT_SELECTORS) {
		rwt_seen[rwt_nseen].sel = sel;
		rwt_seen[rwt_nseen].cls = cls;
		n = rwt_seen[rwt_nseen++].calls = 1;
	}
	pthread_mutex_unlock(&rwt_lock);
	return n;
}

static void
rwt_forward(id self, SEL _cmd, NSInvocation *invocation)
{
	SEL sel = [invocation selector], alias = rwt_alias(sel);
	Class cls = object_getClass(self);

	(void)_cmd;
	if (!class_respondsToSelector(cls, alias)) {
		rwt_say("%s: -%s is not a method here (forwarded and unknown)", class_getName(cls), sel_getName(sel));
		[self doesNotRecognizeSelector:sel];
		return;
	}
	unsigned n = rwt_count(cls, sel);

	if (n >= 1 && n <= rwt_first) {
		rwt_say("%s -%s%s", class_getName(cls), sel_getName(sel), n == 1 ? "" : " (again)");
	}
	[invocation setSelector:alias];

	/* How long the driver took, and whether a new... came back empty: a renderer that waits on a slow upload
	 * or is handed nil stops without a word of its own, and neither shows in a list of selectors. */
	static unsigned slow_lines, nil_lines, tex_lines;

	if (sel == @selector(newTextureWithDescriptor:) && tex_lines < 40) {
		MTLTextureDescriptor *d = nil;

		[invocation getArgument:&d atIndex:2];
		if (d != nil && [d respondsToSelector:@selector(pixelFormat)]) {
			tex_lines++;
			rwt_say("texture asked for: format %lu, %lu x %lu x %lu, %lu levels, type %lu, usage 0x%lx, storage %lu",
			        (unsigned long)[d pixelFormat], (unsigned long)[d width], (unsigned long)[d height],
			        (unsigned long)[d depth], (unsigned long)[d mipmapLevelCount], (unsigned long)[d textureType],
			        (unsigned long)[d usage], (unsigned long)[d storageMode]);
		}
	}
	uint64_t began = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);

	[invocation invokeWithTarget:self];

	uint64_t ms = (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - began) / 1000000;
	const char *name = sel_getName(sel);

	if (ms >= 100 && slow_lines < 60) {
		slow_lines++;
		rwt_say("%s -%s took %llu ms", class_getName(cls), name, (unsigned long long)ms);
	}
	if (strncmp(name, "new", 3) == 0 && [[invocation methodSignature] methodReturnType][0] == '@' && nil_lines < 40) {
		void *made = NULL;

		[invocation getReturnValue:&made];
		if (made == NULL) {
			nil_lines++;
			rwt_say("%s -%s returned nil", class_getName(cls), name);
		}
	}
}

static BOOL
rwt_responds(id self, SEL _cmd, SEL asked)
{
	SEL alias = rwt_alias(_cmd);
	BOOL yes = ((BOOL (*)(id, SEL, SEL))objc_msgSend)(self, alias, asked);

	if (!yes && asked != NULL && strncmp(sel_getName(asked), RWT_PREFIX, sizeof(RWT_PREFIX) - 1) != 0) {
		static SEL said[256];
		static unsigned nsaid;
		int known = 0;

		pthread_mutex_lock(&rwt_lock);
		for (unsigned i = 0; i < nsaid; i++) {
			known |= said[i] == asked;
		}
		if (!known && nsaid < 256) {
			said[nsaid++] = asked;
		}
		pthread_mutex_unlock(&rwt_lock);
		if (!known) {
			rwt_say("%s answers NO to -respondsToSelector:%s", class_getName(object_getClass(self)), sel_getName(asked));
		}
	}
	return yes;
}

static void
rwt_hook(const char *class_name)
{
	Class cls = objc_getClass(class_name), root = objc_getClass("NSObject");
	unsigned hooked = 0, skipped = 0;

	if (cls == Nil) {
		rwt_say("no class %s", class_name);
		return;
	}
	for (Class c = cls; c != Nil && c != root; c = class_getSuperclass(c)) {
		unsigned count = 0;
		Method *methods = class_copyMethodList(c, &count);

		for (unsigned i = 0; i < count; i++) {
			SEL sel = method_getName(methods[i]);
			const char *name = sel_getName(sel), *types = method_getTypeEncoding(methods[i]);
			SEL alias;
			int ok = 0;

			if (rwt_skip(name) || types == NULL) {
				continue;
			}
			alias = rwt_alias(sel);
			if (class_getInstanceMethod(cls, alias) != NULL) {
				continue;       /* a subclass's method of this name is already hooked */
			}
			@try {
				ok = [NSMethodSignature signatureWithObjCTypes:types] != nil;
			} @catch (id e) {
				ok = 0;
			}
			if (!ok) {
				skipped++;      /* NSInvocation cannot carry it (a vector argument, say) */
				continue;
			}
			class_addMethod(cls, alias, method_getImplementation(methods[i]), types);
			class_replaceMethod(cls, sel, (IMP)_objc_msgForward, types);
			hooked++;
		}
		free(methods);
	}
	/* -respondsToSelector: keeps its direct path and tells every NO */
	Method responds = class_getInstanceMethod(cls, @selector(respondsToSelector:));

	class_addMethod(cls, rwt_alias(@selector(respondsToSelector:)), method_getImplementation(responds),
	    method_getTypeEncoding(responds));
	class_replaceMethod(cls, @selector(respondsToSelector:), (IMP)rwt_responds, method_getTypeEncoding(responds));
	class_replaceMethod(cls, @selector(forwardInvocation:), (IMP)rwt_forward, "v@:@");
	rwt_say("%s: %u methods traced, %u left alone (signature)", class_name, hooked, skipped);
}

static void *
rwt_summary(void *unused)
{
	(void)unused;
	for (unsigned tick = 1; tick <= 120; tick++) {
		char line[900];
		size_t at = 0;
		unsigned total;

		sleep(5);
		pthread_mutex_lock(&rwt_lock);
		total = rwt_total;
		line[0] = '\0';
		for (unsigned i = 0; i < rwt_nseen && at + 80 < sizeof(line); i++) {
			if (rwt_seen[i].calls != rwt_seen[i].reported) {
				at += (size_t)snprintf(line + at, sizeof(line) - at, " %s x%u", sel_getName(rwt_seen[i].sel),
				    rwt_seen[i].calls - rwt_seen[i].reported);
				rwt_seen[i].reported = rwt_seen[i].calls;
			}
		}
		pthread_mutex_unlock(&rwt_lock);
		if (total != rwt_total_reported) {
			rwt_say("%u s: %u calls (%u new):%s", tick * 5, total, total - rwt_total_reported, line);
			rwt_total_reported = total;
		} else if (tick == 2 || tick % 12 == 0) {
			char last[600];
			size_t l = 0;

			last[0] = '\0';
			pthread_mutex_lock(&rwt_lock);
			for (unsigned i = 0; i < RWT_RING && l + 70 < sizeof(last); i++) {
				const char *s = rwt_ring[(rwt_ring_at + i) % RWT_RING];

				if (s != NULL) {
					l += (size_t)snprintf(last + l, sizeof(last) - l, " %s", s);
				}
			}
			pthread_mutex_unlock(&rwt_lock);
			rwt_say("%u s: quiet since %u calls; the last ones, oldest first:%s", tick * 5, total, last);
		}
	}
	return NULL;
}

__attribute__((constructor)) static void
rwt_start(void)
{
	static const char *const classes[] = {
		"RedwoodMetalDevice", "RedwoodMetalCommandQueue", "RedwoodMetalCommandBuffer", "RedwoodMetalTexture",
		"RedwoodMetalBuffer", "RedwoodMetalRenderEncoder", "RedwoodMetalBlitEncoder",
	};
	const char *off = getenv("RWA_CALLTRACE");
	pthread_t thread;

	/* AegirPoster too (G9 run 34): with ASTC textures served it makes its queues and then no command buffer */
	if ((strcmp(getprogname(), "backboardd") != 0 && strcmp(getprogname(), "AegirPoster") != 0) ||
	    (off != NULL && off[0] == '0')) {
		return;
	}
	if (strcmp(getprogname(), "backboardd") != 0) {
		rwt_first = 6;
	}
	for (unsigned i = 0; i < sizeof(classes) / sizeof(classes[0]); i++) {
		rwt_hook(classes[i]);
	}
	if (pthread_create(&thread, NULL, rwt_summary, NULL) == 0) {
		pthread_detach(thread);
	}
}
