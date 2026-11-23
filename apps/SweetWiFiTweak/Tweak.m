/* Copyright (c) 2026 Darin. MIT License (see LICENSE); keep this notice. Written by Claude (Anthropic) for the ios-sweet Galaxy A8 port. */
/*
 * SweetWiFi: Wi-Fi and Bluetooth in iOS Settings and the status bar on the Galaxy A8 (jackpotlte), the "tweak route".
 *
 * The A8's radio is not an IO80211 device, so wifid/bluetoothd never see it. The kernel driver (sweet_a8wlbt.c) is
 * controlled through the text sysctls kern.sweet_wifi / kern.sweet_bt (xnu 0140, format in bsd/dev/sweet_a8net.c), the
 * same ones Sweet Connect uses. Loaded by ElleKit's TweakLoader (xnu 0142, sweettweaks=1) into:
 *
 *   Settings (com.apple.Preferences): the Wi-Fi and Bluetooth rows show the radio's state / the joined network, and
 *     open a network (device) list driven by the kernel instead of WiFiKitUI / BluetoothSettings.
 *   SpringBoard: every 3 s reads kern.sweet_wifi; publishes en2's address + DNS to configd (so iOS is online even when
 *     Sweet Connect is closed) and shows the Wi-Fi icon in the status bar while connected.
 *
 * Only Objective-C runtime swizzling; every hook checks that its class and method exist first, so a missing class
 * on another iOS build just leaves that part off (logged with NSLog "SweetWiFi:").
 */
#import <UIKit/UIKit.h>
/* SystemConfiguration exports these on iOS, but its headers mark them unavailable there (as in Sweet Connect). */
typedef const struct __SCDynamicStore *SCDynamicStoreRef;
extern SCDynamicStoreRef SCDynamicStoreCreate(CFAllocatorRef allocator, CFStringRef name, void *callout, void *context);
extern Boolean SCDynamicStoreSetMultiple(SCDynamicStoreRef store, CFDictionaryRef set, CFArrayRef remove, CFArrayRef notify);
extern int SCError(void);
extern const char *SCErrorString(int status);
#import <objc/runtime.h>
#import <objc/message.h>
#include <sys/sysctl.h>

#define SWLog(...) NSLog(@"SweetWiFi: " __VA_ARGS__)

/* ---------------------------------------------------------------- kernel interface */

static NSString *SWRead(const char *name)
{
	size_t len = 0;
	if (sysctlbyname(name, NULL, &len, NULL, 0) != 0 || len == 0) {
		return @"state missing\n";
	}
	NSMutableData *d = [NSMutableData dataWithLength:len + 1];
	if (sysctlbyname(name, d.mutableBytes, &len, NULL, 0) != 0) {
		return @"state missing\n";
	}
	return [NSString stringWithUTF8String:d.bytes] ?: @"state missing\n";
}

static NSString *SWWrite(const char *name, NSString *cmd)
{
	const char *c = cmd.UTF8String;
	if (sysctlbyname(name, NULL, NULL, (void *)c, strlen(c)) == 0) {
		return nil;
	}
	if (errno == EAGAIN) {
		return @"The radio isn't running yet. Try again in a few seconds.";
	}
	return [NSString stringWithFormat:@"The kernel refused it: %s", strerror(errno)];
}

typedef struct {
	NSString *state, *detail, *ip;
	NSArray<NSArray<NSString *> *> *items;
} SWStatus;

static SWStatus SWParse(NSString *text, NSString *tag)
{
	SWStatus s = { @"missing", @"", nil, @[] };
	NSMutableArray *out = [NSMutableArray array];
	for (NSString *line in [text componentsSeparatedByString:@"\n"]) {
		if ([line hasPrefix:@"state "]) {
			NSString *rest = [line substringFromIndex:6];
			NSRange sp = [rest rangeOfString:@" "];
			s.state = sp.location == NSNotFound ? rest : [rest substringToIndex:sp.location];
			s.detail = sp.location == NSNotFound ? @"" : [rest substringFromIndex:sp.location + 1];
		} else if ([line hasPrefix:@"ip "]) {
			s.ip = [line substringFromIndex:3];
		} else if ([line hasPrefix:[tag stringByAppendingString:@" "]]) {
			[out addObject:[[line substringFromIndex:tag.length + 1] componentsSeparatedByString:@"\t"]];
		}
	}
	s.items = out;
	return s;
}

static BOOL SWIsOn(NSString *state) { return ![state isEqualToString:@"off"] && ![state isEqualToString:@"missing"]; }

/* Wi-Fi: the joined network ("net ssid\trssi\tsecure\t1"), or nil */
static NSArray<NSString *> *SWJoined(SWStatus s)
{
	for (NSArray<NSString *> *f in s.items) {
		if (f.count > 3 && [f[3] isEqualToString:@"1"]) {
			return f;
		}
	}
	return nil;
}

/* Bluetooth: names of connected devices */
static NSString *SWBTConnected(SWStatus s)
{
	NSMutableArray *names = [NSMutableArray array];
	for (NSArray<NSString *> *f in s.items) {
		if (f.count > 4 && [f[4] isEqualToString:@"1"]) {
			[names addObject:f[1].length ? f[1] : f[0]];
		}
	}
	return names.count ? [names componentsJoinedByString:@", "] : nil;
}

static void SWSwizzle(NSString *cls, NSString *sel, IMP imp, IMP *orig)
{
	Class c = NSClassFromString(cls);
	Method m = c ? class_getInstanceMethod(c, NSSelectorFromString(sel)) : NULL;
	if (m == NULL) {
		SWLog(@"no -[%@ %@], that part stays off", cls, sel);
		return;
	}
	*orig = method_setImplementation(m, imp);
}

/* ---------------------------------------------------------------- list screen (Settings) */

@interface SWListController : UITableViewController
@property (nonatomic) BOOL bluetooth;
@property (nonatomic) SWStatus status;
@property (nonatomic, strong) NSTimer *timer;
@end

@implementation SWListController

- (const char *)node { return self.bluetooth ? "kern.sweet_bt" : "kern.sweet_wifi"; }

- (instancetype)init { return [super initWithStyle:UITableViewStyleInsetGrouped]; }

- (void)viewDidLoad
{
	[super viewDidLoad];
	self.title = self.bluetooth ? @"Bluetooth" : @"Wi-Fi";
	self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:@"Scan" style:UIBarButtonItemStylePlain
	                                                                         target:self action:@selector(scan)];
	self.refreshControl = [UIRefreshControl new];
	[self.refreshControl addTarget:self action:@selector(scan) forControlEvents:UIControlEventValueChanged];
	[self reload];
}

- (void)viewWillAppear:(BOOL)animated
{
	[super viewWillAppear:animated];
	[self.timer invalidate];
	self.timer = [NSTimer scheduledTimerWithTimeInterval:2.0 target:self selector:@selector(reload) userInfo:nil repeats:YES];
}

- (void)viewWillDisappear:(BOOL)animated
{
	[super viewWillDisappear:animated];
	[self.timer invalidate];
	self.timer = nil;
}

- (void)reload
{
	self.status = SWParse(SWRead(self.node), self.bluetooth ? @"dev" : @"net");
	[self.tableView reloadData];
}

- (void)send:(NSString *)cmd
{
	NSString *err = SWWrite(self.node, cmd);
	if (err) {
		UIAlertController *a = [UIAlertController alertControllerWithTitle:self.title message:err
		                                                    preferredStyle:UIAlertControllerStyleAlert];
		[a addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
		[self presentViewController:a animated:YES completion:nil];
	}
	[self reload];
}

- (void)scan
{
	[self.refreshControl endRefreshing];
	[self send:@"scan"];
}

- (void)toggle:(UISwitch *)sw { [self send:sw.on ? @"on" : @"off"]; }

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tv { return SWIsOn(self.status.state) ? 2 : 1; }

- (NSInteger)tableView:(UITableView *)tv numberOfRowsInSection:(NSInteger)section
{
	return section == 0 ? 1 : (NSInteger)MAX(self.status.items.count, 1);
}

- (NSString *)tableView:(UITableView *)tv titleForHeaderInSection:(NSInteger)section
{
	return section == 0 ? nil : (self.bluetooth ? @"Devices" : @"Networks");
}

- (NSString *)tableView:(UITableView *)tv titleForFooterInSection:(NSInteger)section
{
	if (section != 0) {
		return nil;
	}
	if (!self.bluetooth && self.status.ip) {
		NSArray<NSString *> *f = [self.status.ip componentsSeparatedByString:@"\t"];
		return [NSString stringWithFormat:@"IP address %@ · router %@", f[0], f.count > 2 ? f[2] : @"?"];
	}
	return self.status.detail;
}

- (UITableViewCell *)tableView:(UITableView *)tv cellForRowAtIndexPath:(NSIndexPath *)ip
{
	UITableViewCell *c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
	SWStatus s = self.status;
	if (ip.section == 0) {
		UISwitch *sw = [UISwitch new];
		sw.on = SWIsOn(s.state);
		[sw addTarget:self action:@selector(toggle:) forControlEvents:UIControlEventValueChanged];
		c.textLabel.text = self.title;
		c.accessoryView = sw;
		c.selectionStyle = UITableViewCellSelectionStyleNone;
		return c;
	}
	if (s.items.count == 0) {
		c.textLabel.text = @"Searching…";
		c.textLabel.textColor = UIColor.secondaryLabelColor;
		c.selectionStyle = UITableViewCellSelectionStyleNone;
		return c;
	}
	NSArray<NSString *> *f = s.items[(NSUInteger)ip.row];
	if (self.bluetooth) {      /* addr, name, rssi, paired, connected */
		BOOL paired = f.count > 3 && [f[3] isEqualToString:@"1"], conn = f.count > 4 && [f[4] isEqualToString:@"1"];
		c.textLabel.text = f.count > 1 && f[1].length ? f[1] : f[0];
		c.detailTextLabel.text = conn ? @"Connected" : paired ? @"Not Connected" : nil;
		c.accessoryType = conn ? UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone;
	} else {                   /* ssid, rssi, secure, connected */
		BOOL secure = f.count > 2 && [f[2] isEqualToString:@"1"], conn = f.count > 3 && [f[3] isEqualToString:@"1"];
		int rssi = f.count > 1 ? f[1].intValue : -100;
		c.textLabel.text = f[0];
		c.imageView.image = conn ? [UIImage systemImageNamed:@"checkmark"] : nil;
		if (!conn) {
			c.imageView.image = [[UIImage alloc] init];
		}
		UIImageSymbolConfiguration *cfg = [UIImageSymbolConfiguration configurationWithScale:UIImageSymbolScaleSmall];
		UIImage *signal = [UIImage systemImageNamed:@"wifi" withConfiguration:cfg];
		UIStackView *acc = [[UIStackView alloc] init];
		acc.spacing = 6;
		if (secure) {
			[acc addArrangedSubview:[[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"lock.fill" withConfiguration:cfg]]];
		}
		UIImageView *sv = [[UIImageView alloc] initWithImage:signal];
		sv.alpha = rssi > -60 ? 1.0 : rssi > -72 ? 0.7 : 0.4;
		[acc addArrangedSubview:sv];
		for (UIView *v in acc.arrangedSubviews) {
			v.tintColor = UIColor.labelColor;
		}
		[acc sizeToFit];
		acc.frame = CGRectMake(0, 0, secure ? 44 : 22, 22);
		c.accessoryView = acc;
	}
	return c;
}

- (void)tableView:(UITableView *)tv didSelectRowAtIndexPath:(NSIndexPath *)ip
{
	[tv deselectRowAtIndexPath:ip animated:YES];
	if (ip.section != 1 || self.status.items.count == 0) {
		return;
	}
	NSArray<NSString *> *f = self.status.items[(NSUInteger)ip.row];
	if (self.bluetooth) {
		BOOL paired = f.count > 3 && [f[3] isEqualToString:@"1"], conn = f.count > 4 && [f[4] isEqualToString:@"1"];
		NSString *addr = f[0];
		[self send:[(conn ? @"disconnect " : paired ? @"connect " : @"pair ") stringByAppendingString:addr]];
		return;
	}
	NSString *ssid = f[0];
	BOOL secure = f.count > 2 && [f[2] isEqualToString:@"1"], conn = f.count > 3 && [f[3] isEqualToString:@"1"];
	if (conn) {
		UIAlertController *a = [UIAlertController alertControllerWithTitle:ssid message:nil
		                                                    preferredStyle:UIAlertControllerStyleActionSheet];
		[a addAction:[UIAlertAction actionWithTitle:@"Disconnect" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *x) {
			[self send:@"leave"];
		}]];
		[a addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
		a.popoverPresentationController.sourceView = [tv cellForRowAtIndexPath:ip];
		[self presentViewController:a animated:YES completion:nil];
		return;
	}
	if (!secure) {
		[self send:[NSString stringWithFormat:@"join %@\t", ssid]];
		return;
	}
	UIAlertController *a = [UIAlertController alertControllerWithTitle:[NSString stringWithFormat:@"Enter the password for “%@”", ssid]
	                                                            message:nil preferredStyle:UIAlertControllerStyleAlert];
	[a addTextFieldWithConfigurationHandler:^(UITextField *t) {
		t.placeholder = @"Password";
		t.secureTextEntry = YES;
	}];
	[a addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
	[a addAction:[UIAlertAction actionWithTitle:@"Join" style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) {
		[self send:[NSString stringWithFormat:@"join %@\t%@", ssid, a.textFields.firstObject.text ?: @""]];
	}]];
	[self presentViewController:a animated:YES completion:nil];
}

@end

/* ---------------------------------------------------------------- Settings hooks */

static NSString *SWSpecifierID(id spec)
{
	return [spec respondsToSelector:@selector(identifier)] ? ((NSString * (*)(id, SEL))objc_msgSend)(spec, @selector(identifier)) : nil;
}

static BOOL SWIsWiFi(NSString *ident) { return [ident isEqualToString:@"WIFI"]; }
static BOOL SWIsBT(NSString *ident) { return [ident isEqualToString:@"BLUETOOTH"] || [ident isEqualToString:@"Bluetooth"]; }

/* value text for a row: our getter, installed on the WIFI / BLUETOOTH specifiers */
static NSString *SWRowValue(BOOL bt)
{
	if (bt) {
		SWStatus s = SWParse(SWRead("kern.sweet_bt"), @"dev");
		return !SWIsOn(s.state) ? @"Off" : (SWBTConnected(s) ?: @"On");
	}
	SWStatus s = SWParse(SWRead("kern.sweet_wifi"), @"net");
	NSArray<NSString *> *j = SWJoined(s);
	return !SWIsOn(s.state) ? @"Off" : j ? j[0] : @"Not Connected";
}

@interface NSObject (SWGetters)
- (id)sweetWiFiValue:(id)spec;
- (id)sweetBTValue:(id)spec;
@end

static id SWWiFiValueIMP(id self, SEL _cmd, id spec) { return SWRowValue(NO); }
static id SWBTValueIMP(id self, SEL _cmd, id spec) { return SWRowValue(YES); }

/* point a PSSpecifier's target/getter (ivars) at our value method */
static void SWSetGetter(id spec, id target, SEL getter)
{
	Ivar it = class_getInstanceVariable(object_getClass(spec), "target");
	Ivar ig = class_getInstanceVariable(object_getClass(spec), "getter");
	if (it == NULL || ig == NULL) {
		Class c = object_getClass(spec);
		while (c && (it == NULL || ig == NULL)) {
			c = class_getSuperclass(c);
			if (c) {
				it = it ?: class_getInstanceVariable(c, "target");
				ig = ig ?: class_getInstanceVariable(c, "getter");
			}
		}
	}
	if (it == NULL || ig == NULL) {
		SWLog(@"PSSpecifier has no target/getter ivars");
		return;
	}
	object_setIvar(spec, it, target);   /* PSSpecifier's target is a weak/unretained id */
	*(SEL *)((uint8_t *)(__bridge void *)spec + ivar_getOffset(ig)) = getter;
}

static IMP origSpecifiers, origDidSelect;

static id SWSpecifiers(id self, SEL _cmd)
{
	NSArray *specs = ((id (*)(id, SEL))origSpecifiers)(self, _cmd);
	for (id spec in specs) {
		NSString *ident = SWSpecifierID(spec);
		if (SWIsWiFi(ident)) {
			SWSetGetter(spec, self, @selector(sweetWiFiValue:));
		} else if (SWIsBT(ident)) {
			SWSetGetter(spec, self, @selector(sweetBTValue:));
		}
	}
	return specs;
}

static void SWDidSelect(id self, SEL _cmd, UITableView *tv, NSIndexPath *ip)
{
	id spec = nil;
	if ([self respondsToSelector:@selector(specifierAtIndexPath:)]) {
		spec = ((id (*)(id, SEL, id))objc_msgSend)(self, @selector(specifierAtIndexPath:), ip);
	}
	NSString *ident = SWSpecifierID(spec);
	if (SWIsWiFi(ident) || SWIsBT(ident)) {
		[tv deselectRowAtIndexPath:ip animated:YES];
		SWListController *vc = [SWListController new];
		vc.bluetooth = SWIsBT(ident);
		[(UIViewController *)self showViewController:vc sender:self];
		return;
	}
	((void (*)(id, SEL, id, id))origDidSelect)(self, _cmd, tv, ip);
}

static void SWSettingsInit(void)
{
	Class c = NSClassFromString(@"PSUIPrefsListController");
	if (c == Nil) {
		SWLog(@"no PSUIPrefsListController");
		return;
	}
	class_addMethod(c, @selector(sweetWiFiValue:), (IMP)SWWiFiValueIMP, "@@:@");
	class_addMethod(c, @selector(sweetBTValue:), (IMP)SWBTValueIMP, "@@:@");
	SWSwizzle(@"PSUIPrefsListController", @"specifiers", (IMP)SWSpecifiers, &origSpecifiers);
	SWSwizzle(@"PSUIPrefsListController", @"tableView:didSelectRowAtIndexPath:", (IMP)SWDidSelect, &origDidSelect);
	/* refresh the row values while Settings is open (joined network can change) */
	[NSTimer scheduledTimerWithTimeInterval:3.0 repeats:YES block:^(NSTimer *t) {
		UIViewController *root = UIApplication.sharedApplication.windows.firstObject.rootViewController;
		NSMutableArray *stack = [NSMutableArray arrayWithObject:root ?: (id)NSNull.null];
		while (stack.count) {
			id vc = stack.lastObject;
			[stack removeLastObject];
			if ([vc isKindOfClass:c]) {
				for (NSString *ident in @[ @"WIFI", @"BLUETOOTH" ]) {
					if ([vc respondsToSelector:@selector(reloadSpecifierID:)]) {
						((void (*)(id, SEL, id))objc_msgSend)(vc, @selector(reloadSpecifierID:), ident);
					}
				}
			} else if ([vc isKindOfClass:UIViewController.class]) {
				[stack addObjectsFromArray:((UIViewController *)vc).childViewControllers];
			}
		}
	}];
	SWLog(@"Settings hooks in");
}

/* ---------------------------------------------------------------- SpringBoard: configd + status bar */

static NSString *SWPosted;
static NSArray<NSString *> *SWJoinedNet;   /* current joined network, for the status-bar hooks */

static void SWPublish(NSString *ip)
{
	if ([(ip ?: @"") isEqualToString:(SWPosted ?: @"")]) {
		return;
	}
	SCDynamicStoreRef st = SCDynamicStoreCreate(NULL, CFSTR("SweetWiFi"), NULL, NULL);
	if (st == NULL) {
		return;
	}
	NSString *svc = @"State:/Network/Service/com.iossweet.wifi";
	NSString *k4 = [svc stringByAppendingString:@"/IPv4"], *kd = [svc stringByAppendingString:@"/DNS"];
	Boolean ok;
	NSArray<NSString *> *f = [ip componentsSeparatedByString:@"\t"];
	if (ip && f.count >= 5) {
		NSMutableArray *dns = [NSMutableArray array];
		if (![f[3] isEqualToString:@"0.0.0.0"]) {
			[dns addObject:f[3]];
		}
		[dns addObjectsFromArray:@[ @"1.1.1.1", @"8.8.8.8" ]];
		NSDictionary *v4 = @{ @"Addresses": @[ f[0] ], @"SubnetMasks": @[ f[1] ], @"Router": f[2], @"InterfaceName": f[4] };
		ok = SCDynamicStoreSetMultiple(st, (__bridge CFDictionaryRef)@{ k4: v4, kd: @{ @"ServerAddresses": dns } }, NULL, NULL);
	} else {
		ok = SCDynamicStoreSetMultiple(st, NULL, (__bridge CFArrayRef)@[ k4, kd ], NULL);
	}
	if (ok) {
		SWPosted = ip ?: @"";
	} else {
		SWLog(@"configd refused: %s", SCErrorString(SCError()));
	}
	CFRelease(st);
}

/* SBWiFiManager answers for the status bar: associated / bars / name */
static IMP origIsAssociated, origBars, origName, origPowered, origIsPrimary;

static BOOL SWIsAssociated(id self, SEL _cmd) { return SWJoinedNet != nil; }
static BOOL SWPoweredIMP(id self, SEL _cmd) { return SWJoinedNet != nil || ((BOOL (*)(id, SEL))origPowered)(self, _cmd); }
static int SWBarsIMP(id self, SEL _cmd)
{
	if (SWJoinedNet == nil) {
		return 0;
	}
	int rssi = SWJoinedNet.count > 1 ? SWJoinedNet[1].intValue : -100;
	return rssi > -60 ? 3 : rssi > -72 ? 2 : 1;
}
static id SWNameIMP(id self, SEL _cmd) { return SWJoinedNet ? SWJoinedNet[0] : nil; }
static BOOL SWPrimaryIMP(id self, SEL _cmd) { return SWJoinedNet != nil; }

static void SWStatusBarRefresh(void)
{
	Class agg = NSClassFromString(@"SBStatusBarStateAggregator");
	id a = [agg respondsToSelector:@selector(sharedInstance)] ? ((id (*)(id, SEL))objc_msgSend)(agg, @selector(sharedInstance)) : nil;
	for (NSString *sel in @[ @"_updateDataNetworkItem", @"_updateWifiItem", @"updateStatusBarItem:" ]) {
		SEL s = NSSelectorFromString(sel);
		if ([a respondsToSelector:s] && ![sel hasSuffix:@":"]) {
			((void (*)(id, SEL))objc_msgSend)(a, s);
		}
	}
	[[NSNotificationCenter defaultCenter] postNotificationName:@"SBWifiManagerLinkDidChangeNotification" object:nil];
}

static void SWSpringBoardInit(void)
{
	SWSwizzle(@"SBWiFiManager", @"isAssociated", (IMP)SWIsAssociated, &origIsAssociated);
	SWSwizzle(@"SBWiFiManager", @"isAssociatedToIOSHotspot", (IMP)SWIsAssociated, &origIsAssociated);
	SWSwizzle(@"SBWiFiManager", @"isPowered", (IMP)SWPoweredIMP, &origPowered);
	SWSwizzle(@"SBWiFiManager", @"signalStrengthBars", (IMP)SWBarsIMP, &origBars);
	SWSwizzle(@"SBWiFiManager", @"currentNetworkName", (IMP)SWNameIMP, &origName);
	SWSwizzle(@"SBWiFiManager", @"isPrimaryInterface", (IMP)SWPrimaryIMP, &origIsPrimary);
	dispatch_queue_t q = dispatch_queue_create("com.iossweet.wifi", DISPATCH_QUEUE_SERIAL);
	dispatch_source_t t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
	dispatch_source_set_timer(t, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), 3 * NSEC_PER_SEC, NSEC_PER_SEC / 2);
	dispatch_source_set_event_handler(t, ^{
		SWStatus s = SWParse(SWRead("kern.sweet_wifi"), @"net");
		SWPublish(s.ip);
		NSArray<NSString *> *j = SWIsOn(s.state) && s.ip ? SWJoined(s) : nil;
		BOOL changed = !((j == nil && SWJoinedNet == nil) || [j isEqualToArray:SWJoinedNet]);
		if (changed) {
			dispatch_async(dispatch_get_main_queue(), ^{
				SWJoinedNet = j;
				SWStatusBarRefresh();
			});
		}
	});
	dispatch_resume(t);
	static dispatch_source_t keep;
	keep = t;
	(void)keep;
	SWLog(@"SpringBoard hooks in");
}

__attribute__((constructor)) static void SWInit(void)
{
	NSString *bid = NSBundle.mainBundle.bundleIdentifier;
	if ([bid isEqualToString:@"com.apple.Preferences"]) {
		SWSettingsInit();
	} else if ([bid isEqualToString:@"com.apple.springboard"]) {
		SWSpringBoardInit();
	}
}
