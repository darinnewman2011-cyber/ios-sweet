/*
 * sweet-ext: is an extension point's extensions known to the system, and by which of its registries?
 *
 * PosterBoard's extension store was empty (G9 run 10): it found none of the 11 poster extensions on the root, so
 * the wallpaper picker is empty and no poster ever draws. This asks every registry PosterBoard could be using and
 * prints what each answers, so one boot says where discovery breaks.
 *
 *   sweet-ext --cache                   why ExtensionFoundation drops /System/Library/xpc/extensions.plist
 *   sweet-ext --platform                does iOS's own lookup (extensionPointForIdentifier:platform:) find an entry, and for
 *                                        which platform value? (installd says "Extension cache does not have an entry")
 *   sweet-ext [POINT [BUNDLE-ID ...]]   default: com.apple.posterkit.provider and the poster bundle IDs,
 *                                        then com.apple.widgetkit-extension for comparison
 *
 * Every lookup is by name through the runtime (these are SPI), each in its own @try: a missing class or a throw is
 * printed and the next one runs. Signed with PosterBoard's com.apple.private.coreservices.canmaplsdatabase.
 */
#import <Foundation/Foundation.h>
#import <dlfcn.h>
#import <sys/stat.h>
#import <objc/message.h>
#import <objc/runtime.h>

static void
say(NSString *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	NSString *s = [[NSString alloc] initWithFormat:fmt arguments:ap];
	va_end(ap);
	printf("sweet-ext: %s\n", s.UTF8String);
	fflush(stdout);
}

static id
call0(id target, const char *sel)
{
	SEL s = sel_registerName(sel);

	if (target == nil || ![target respondsToSelector:s]) {
		return nil;
	}
	return ((id (*)(id, SEL))objc_msgSend)(target, s);
}

static NSString *
describe(id o)
{
	NSString *d = [o description];

	return d.length > 300 ? [[d substringToIndex:300] stringByAppendingString:@"..."] : d;
}

/* 1. LaunchServices: every plugin LS has registered, and the ones of this point */
static void
via_launchservices(NSString *point)
{
	@try {
		id ws = call0((id)objc_getClass("LSApplicationWorkspace"), "defaultWorkspace");
		NSArray *all = call0(ws, "installedPlugins");
		NSUInteger mine = 0;

		if (ws == nil) {
			say(@"LS: no LSApplicationWorkspace");
			return;
		}
		for (id p in all) {
			NSString *pt = call0(p, "protocol");

			if ([pt isEqualToString:point]) {
				say(@"LS: %@ (%@)", call0(p, "pluginIdentifier"), call0(p, "bundleURL"));
				mine++;
			}
		}
		say(@"LS: %lu plugins registered in all, %lu for %@", (unsigned long)all.count, (unsigned long)mine, point);
	} @catch (NSException *e) {
		say(@"LS: threw %@: %@", e.name, e.reason);
	}
}

/* 2. NSExtension, the PlugInKit path (pkd) */
static void
via_nsextension(NSString *point)
{
	@try {
		Class c = objc_getClass("NSExtension");
		SEL s = sel_registerName("extensionsWithMatchingAttributes:error:");
		NSError *err = nil;
		NSArray *found;

		if (c == nil || ![c respondsToSelector:s]) {
			say(@"NSExtension: no +extensionsWithMatchingAttributes:error:");
			return;
		}
		found = ((id (*)(id, SEL, id, NSError **))objc_msgSend)(c, s, @{ @"NSExtensionPointName": point }, &err);
		say(@"NSExtension: %lu for %@%@%@", (unsigned long)found.count, point, err ? @"; error " : @"",
		    err ? describe(err) : @"");
		for (id x in found) {
			say(@"NSExtension:   %@", call0(x, "identifier"));
		}
	} @catch (NSException *e) {
		say(@"NSExtension: threw %@: %@", e.name, e.reason);
	}
}

/* 3. ExtensionKit's query, what EXAppExtensionAttributes extensions are found by */
static void
via_extensionkit(NSString *point)
{
	@try {
		Class qc = objc_getClass("_EXQuery");
		Class ctl = objc_getClass("_EXQueryController");
		SEL init = sel_registerName("initWithExtensionPointIdentifier:");
		SEL exec = sel_registerName("executeQuery:");
		id q;
		NSArray *found;

		if (qc == nil || ctl == nil || ![ctl respondsToSelector:exec]) {
			say(@"ExtensionKit: no _EXQuery/_EXQueryController (%@, %@)", qc, ctl);
			return;
		}
		q = ((id (*)(id, SEL, id))objc_msgSend)([qc alloc], init, point);
		found = ((id (*)(id, SEL, id))objc_msgSend)(ctl, exec, q);
		say(@"ExtensionKit: %lu for %@", (unsigned long)found.count, point);
		for (id x in found) {
			say(@"ExtensionKit:   %@", describe(x));
		}
	} @catch (NSException *e) {
		say(@"ExtensionKit: threw %@: %@", e.name, e.reason);
	}
}

/* 4. the LS record of one extension by bundle ID: present, and what it says, or the error */
static void
via_record(NSString *bundle)
{
	@try {
		Class c = objc_getClass("LSApplicationExtensionRecord");
		SEL s = sel_registerName("initWithBundleIdentifier:error:");
		NSError *err = nil;
		id r;

		if (c == nil) {
			say(@"record: no LSApplicationExtensionRecord");
			return;
		}
		if (![c instancesRespondToSelector:s]) {
			say(@"record: LSApplicationExtensionRecord has no -initWithBundleIdentifier:error:");
			return;
		}
		r = ((id (*)(id, SEL, id, NSError **))objc_msgSend)([c alloc], s, bundle, &err);
		if (r == nil) {
			say(@"record %@: none; %@", bundle, err ? describe(err) : @"no error");
			return;
		}
		say(@"record %@: point %@, url %@", bundle, call0(call0(r, "extensionPointRecord"), "identifier"),
		    call0(r, "URL"));
	} @catch (NSException *e) {
		say(@"record %@: threw %@: %@", bundle, e.name, e.reason);
	}
}

static void
probe(NSString *point, NSArray<NSString *> *bundles)
{
	say(@"--- %@", point);
	via_launchservices(point);
	via_nsextension(point);
	via_extensionkit(point);
}

/* the records last and each announced first: run 11's crashed (SIGSEGV, far 0x20) and took the rest with it */
static void
records(NSArray<NSString *> *bundles)
{
	for (NSString *b in bundles) {
		say(@"record %@: asking", b);
		via_record(b);
	}
}

/*
 * --cache: why does ExtensionFoundation drop /System/Library/xpc/extensions.plist? (G9: every extension point
 * "not in the cache" although the file has them). -[EXExtensionPointEnumerator initWithSDKDictionary:urls:config:]
 * loads it with +[NSDictionary _EX_dictionaryWithSignedURL:error:] when enforceXPCCacheCodeSigning is on, and drops
 * the cache without a word when that fails. This calls the same thing and prints the error.
 */
static void
describe_error(NSError *e, int depth)
{
	for (; e != nil && depth < 5; depth++) {
		say(@"  error[%d] %@ %ld: %@", depth, e.domain, (long)e.code, e.localizedDescription);
		say(@"  error[%d] userInfo %@", depth, describe(e.userInfo));
		e = e.userInfo[NSUnderlyingErrorKey];
	}
}

static void
cache_probe(void)
{
	static const char *const paths[] = {
		"/System/Library/PrivateFrameworks/ExtensionFoundation.framework/ExtensionFoundation",
		"/System/Library/Frameworks/ExtensionFoundation.framework/ExtensionFoundation",
		"/usr/lib/swift/libswiftExtensionFoundation.dylib",
	};
	NSString *file = @"/System/Library/xpc/extensions.plist";
	struct stat st;

	for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		say(@"dlopen %s: %s", paths[i], dlopen(paths[i], RTLD_NOW) ? "ok" : dlerror());
	}
	for (NSString *f in @[ file, [file stringByAppendingString:@".sig"], @"/System/Library/xpc/launchd.plist" ]) {
		say(@"stat %@: %s, %lld bytes", f, stat(f.UTF8String, &st) == 0 ? "ok" : strerror(errno), (long long)st.st_size);
	}
	/* who answers enforceXPCCacheCodeSigning, and with what */
	SEL enforce = sel_registerName("enforceXPCCacheCodeSigning");
	unsigned count = 0;
	Class *all = objc_copyClassList(&count);

	for (unsigned i = 0; i < count; i++) {
		BOOL inst = class_getInstanceMethod(all[i], enforce) != NULL;
		BOOL cls = class_getClassMethod(all[i], enforce) != NULL;

		if (!inst && !cls) {
			continue;
		}
		say(@"enforceXPCCacheCodeSigning is implemented by %s (%s%s)", class_getName(all[i]), cls ? "class" : "",
		    inst ? " instance" : "");
		@try {
			if (cls) {
				say(@"  [%s enforceXPCCacheCodeSigning] = %d", class_getName(all[i]),
				    ((BOOL (*)(id, SEL))objc_msgSend)((id)all[i], enforce));
			} else {
				id shared = call0((id)all[i], "sharedInstance");

				if (shared != nil) {
					say(@"  [[%s sharedInstance] enforceXPCCacheCodeSigning] = %d", class_getName(all[i]),
					    ((BOOL (*)(id, SEL))objc_msgSend)(shared, enforce));
				}
			}
		} @catch (NSException *e) {
			say(@"  threw %@: %@", e.name, e.reason);
		}
	}
	free(all);
	/* the signed load, as the enumerator does it */
	@try {
		SEL signed_ = sel_registerName("_EX_dictionaryWithSignedURL:error:");
		NSError *err = nil;
		id d;

		if (![NSDictionary respondsToSelector:signed_]) {
			say(@"NSDictionary has no +_EX_dictionaryWithSignedURL:error: (ExtensionFoundation not loaded?)");
			return;
		}
		d = ((id (*)(id, SEL, id, NSError **))objc_msgSend)([NSDictionary class], signed_,
		    [NSURL fileURLWithPath:file], &err);
		say(@"signed load of %@: %@ (%lu keys)", file, d ? @"LOADED" : @"FAILED", (unsigned long)[(NSDictionary *)d count]);
		describe_error(err, 0);
		d = [NSDictionary dictionaryWithContentsOfURL:[NSURL fileURLWithPath:file]];
		say(@"plain load of %@: %lu keys", file, (unsigned long)[(NSDictionary *)d count]);
	} @catch (NSException *e) {
		say(@"signed load threw %@: %@", e.name, e.reason);
	}
}

/*
 * --platform: installd's MICopyExtensionCacheEntryWithPlatform(identifier, platform, &error) asks the extension
 * point cache through +extensionPointForIdentifier:platform: and fails when the result has no SDKDictionary. The
 * cache itself loads fine (--cache: 138 keys, signed load OK), so try the lookup with every platform value that
 * could be the right one, on the identifiers that fail in the log.
 */
static void platform_probe(void)
{
	static NSString *const ids[] = { @"com.apple.widgetkit-extension", @"com.apple.posterkit.provider" };
	static NSString *const strs[] = { @"iphoneos", @"iOS", @"ios", @"iPhoneOS", @"iphone", @"" };
	SEL sel = sel_registerName("extensionPointForIdentifier:platform:");
	unsigned count = 0, found = 0;
	Class *all;

	dlopen("/System/Library/PrivateFrameworks/ExtensionFoundation.framework/ExtensionFoundation", RTLD_NOW);
	dlopen("/System/Library/PrivateFrameworks/InstalledContentLibrary.framework/InstalledContentLibrary", RTLD_NOW);
	all = objc_copyClassList(&count);
	for (unsigned i = 0; i < count; i++) {
		Method m = class_getClassMethod(all[i], sel);
		char *t;

		if (m == NULL) {
			continue;
		}
		found++;
		t = method_copyArgumentType(m, 3);
		say(@"+[%s extensionPointForIdentifier:platform:] platform argument type '%s', return '%s'", class_getName(all[i]),
		    t, method_getTypeEncoding(m));
		for (unsigned n = 0; n < sizeof(ids) / sizeof(ids[0]); n++) {
			if (t[0] == '@') {
				for (unsigned k = 0; k < sizeof(strs) / sizeof(strs[0]); k++) {
					@try {
						id r = ((id (*)(id, SEL, id, id))objc_msgSend)((id)all[i], sel, ids[n], strs[k]);
						id sdk = call0(r, "SDKDictionary");

						say(@"  %@ platform \"%@\": point %@, SDKDictionary %@", ids[n], strs[k], r ? @"yes" : @"nil",
						    sdk ? [NSString stringWithFormat:@"%lu keys", (unsigned long)[sdk count]] : @"nil");
					} @catch (NSException *e) {
						say(@"  %@ platform \"%@\": threw %@", ids[n], strs[k], e.name);
					}
				}
			} else {
				for (unsigned long long v = 0; v <= 12; v++) {
					@try {
						id r = ((id (*)(id, SEL, id, unsigned long long))objc_msgSend)((id)all[i], sel, ids[n], v);
						id sdk = call0(r, "SDKDictionary");

						say(@"  %@ platform %llu: point %@, SDKDictionary %@", ids[n], v, r ? @"yes" : @"nil",
						    sdk ? [NSString stringWithFormat:@"%lu keys", (unsigned long)[sdk count]] : @"nil");
					} @catch (NSException *e) {
						say(@"  %@ platform %llu: threw %@", ids[n], v, e.name);
					}
				}
			}
		}
		free(t);
	}
	free(all);
	if (found == 0) {
		say(@"no class implements +extensionPointForIdentifier:platform: (libraries loaded? %d classes seen)", (int)count);
	}
	/* and the C function itself, as installd calls it */
	void *fn = dlsym(RTLD_DEFAULT, "MICopyExtensionCacheEntryWithPlatform");

	say(@"MICopyExtensionCacheEntryWithPlatform is %s", fn ? "exported" : "not exported");
	if (fn != NULL) {
		for (unsigned long long v = 0; v <= 6; v++) {
			NSError *err = nil;
			id r = ((id (*)(id, unsigned long long, NSError **))fn)(@"com.apple.widgetkit-extension", v, &err);

			say(@"  MICopyExtensionCacheEntryWithPlatform(widgetkit, %llu): %@ %@", v, r ? @"entry" : @"nil",
			    err ? describe(err) : @"");
		}
	}
}

int
main(int argc, char **argv)
{
	@autoreleasepool {
		/* LSApplicationWorkspace lives in MobileCoreServices, which Foundation does not load (run 11: "no
		 * LSApplicationWorkspace") */
		say(@"uid %d; MobileCoreServices %s", getuid(),
		    dlopen("/System/Library/Frameworks/MobileCoreServices.framework/MobileCoreServices", RTLD_NOW) ?
		    "loaded" : dlerror());
		if (argc == 2 && strcmp(argv[1], "--platform") == 0) {
			platform_probe();
			return 0;
		}
		if (argc == 2 && strcmp(argv[1], "--cache") == 0) {
			cache_probe();
			return 0;
		}
		if (argc > 1) {
			NSMutableArray *b = [NSMutableArray array];

			for (int i = 2; i < argc; i++) {
				[b addObject:@(argv[i])];
			}
			probe(@(argv[1]), b);
			records(b);
			return 0;
		}
		probe(@"com.apple.posterkit.provider", @[]);
		probe(@"com.apple.widgetkit-extension", @[]);
		records(@[
		    @"com.apple.WallpaperKit.CollectionsPoster",
		    @"com.apple.GradientPoster.GradientPosterExtension",
		    @"com.apple.EmojiPoster.EmojiPosterExtension",
		    @"com.apple.PhotosUIPrivate.PhotosPosterProvider",
		]);
	}
	return 0;
}
