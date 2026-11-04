/*
 * sweet-ext: is an extension point's extensions known to the system, and by which of its registries?
 *
 * PosterBoard's extension store was empty (G9 run 10): it found none of the 11 poster extensions on the root, so
 * the wallpaper picker is empty and no poster ever draws. This asks every registry PosterBoard could be using and
 * prints what each answers, so one boot says where discovery breaks.
 *
 *   sweet-ext [POINT [BUNDLE-ID ...]]   default: com.apple.posterkit.provider and the poster bundle IDs,
 *                                        then com.apple.widgetkit-extension for comparison
 *
 * Every lookup is by name through the runtime (these are SPI), each in its own @try: a missing class or a throw is
 * printed and the next one runs. Signed with PosterBoard's com.apple.private.coreservices.canmaplsdatabase.
 */
#import <Foundation/Foundation.h>
#import <dlfcn.h>
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

int
main(int argc, char **argv)
{
	@autoreleasepool {
		/* LSApplicationWorkspace lives in MobileCoreServices, which Foundation does not load (run 11: "no
		 * LSApplicationWorkspace") */
		say(@"uid %d; MobileCoreServices %s", getuid(),
		    dlopen("/System/Library/Frameworks/MobileCoreServices.framework/MobileCoreServices", RTLD_NOW) ?
		    "loaded" : dlerror());
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
