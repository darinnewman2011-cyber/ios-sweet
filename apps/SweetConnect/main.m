/* Copyright (c) 2026 Darin. MIT License (see LICENSE); keep this notice. Written by Claude (Anthropic) for the ios-sweet Galaxy A8 port. */
/*
 * Sweet Connect: Wi-Fi and Bluetooth for the Galaxy A8 port while iOS's own Settings panes cannot drive the radio.
 * Talks to the kernel through the text sysctls kern.sweet_wifi and kern.sweet_bt (xnu 0140, bsd/dev/sweet_a8net.c);
 * the formats are described there. Plain UIKit, no storyboards.
 */
#import <UIKit/UIKit.h>
#include <sys/sysctl.h>
#include <errno.h>
#include <string.h>

/* ---- kernel interface ---- */

static NSString *SCRead(const char *name)
{
	size_t len = 0;
	if (sysctlbyname(name, NULL, &len, NULL, 0) != 0 || len == 0) {
		return [NSString stringWithFormat:@"state missing the kernel has no %s (needs xnu 0140): %s\n", name, strerror(errno)];
	}
	NSMutableData *d = [NSMutableData dataWithLength:len + 1];
	if (sysctlbyname(name, d.mutableBytes, &len, NULL, 0) != 0) {
		return [NSString stringWithFormat:@"state error %s\n", strerror(errno)];
	}
	((char *)d.mutableBytes)[len] = 0;
	return [NSString stringWithUTF8String:d.bytes] ?: @"state error unreadable\n";
}

/* Returns nil on success, else a message for the user. */
static NSString *SCWrite(const char *name, NSString *cmd)
{
	const char *c = cmd.UTF8String;
	if (sysctlbyname(name, NULL, NULL, (void *)c, strlen(c)) == 0) {
		return nil;
	}
	if (errno == EAGAIN) {
		return @"The radio isn't running yet. The command was handed to the kernel and will work once the Wi-Fi/Bluetooth driver is up.";
	}
	return [NSString stringWithFormat:@"The kernel refused it: %s", strerror(errno)];
}

/* Parses "state <word> <detail>" and the tab-separated item lines starting with `tag`. */
static void SCParse(NSString *text, NSString *tag, NSString **state, NSString **detail, NSArray<NSArray<NSString *> *> **items)
{
	NSMutableArray *out = [NSMutableArray array];
	*state = @"unknown";
	*detail = @"";
	for (NSString *line in [text componentsSeparatedByString:@"\n"]) {
		if ([line hasPrefix:@"state "]) {
			NSString *rest = [line substringFromIndex:6];
			NSRange sp = [rest rangeOfString:@" "];
			*state = sp.location == NSNotFound ? rest : [rest substringToIndex:sp.location];
			*detail = sp.location == NSNotFound ? @"" : [rest substringFromIndex:sp.location + 1];
		} else if ([line hasPrefix:[tag stringByAppendingString:@" "]]) {
			[out addObject:[[line substringFromIndex:tag.length + 1] componentsSeparatedByString:@"\t"]];
		}
	}
	*items = out;
}

static UIImage *SCSymbol(NSString *name) { return [UIImage systemImageNamed:name]; }

/* ---- a list screen shared by Wi-Fi and Bluetooth ---- */

@interface SCListController : UITableViewController
@property (nonatomic) BOOL bluetooth;
@property (nonatomic, copy) NSString *state, *detail;
@property (nonatomic, copy) NSArray<NSArray<NSString *> *> *items;
@property (nonatomic, strong) NSTimer *timer;
@end

@implementation SCListController

- (const char *)node { return self.bluetooth ? "kern.sweet_bt" : "kern.sweet_wifi"; }

- (void)viewDidLoad
{
	[super viewDidLoad];
	self.title = self.bluetooth ? @"Bluetooth" : @"Wi-Fi";
	self.tableView = [[UITableView alloc] initWithFrame:CGRectZero style:UITableViewStyleInsetGrouped];
	self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:@"Scan" style:UIBarButtonItemStylePlain
	                                                                         target:self action:@selector(scan)];
	self.refreshControl = [UIRefreshControl new];
	[self.refreshControl addTarget:self action:@selector(scan) forControlEvents:UIControlEventValueChanged];
	[self reload];
}

- (void)viewWillAppear:(BOOL)animated
{
	[super viewWillAppear:animated];
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
	NSString *s, *d;
	NSArray *items;
	SCParse(SCRead(self.node), self.bluetooth ? @"dev" : @"net", &s, &d, &items);
	self.state = s;
	self.detail = d;
	self.items = items;
	[self.tableView reloadData];
}

- (void)send:(NSString *)cmd
{
	NSString *err = SCWrite(self.node, cmd);
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

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tv { return 2; }

- (NSInteger)tableView:(UITableView *)tv numberOfRowsInSection:(NSInteger)section
{
	return section == 0 ? 1 : (NSInteger)MAX(self.items.count, 1);
}

- (NSString *)tableView:(UITableView *)tv titleForHeaderInSection:(NSInteger)section
{
	return section == 0 ? nil : (self.bluetooth ? @"Devices" : @"Networks");
}

- (NSString *)tableView:(UITableView *)tv titleForFooterInSection:(NSInteger)section
{
	return section == 0 ? self.detail : nil;
}

- (UITableViewCell *)tableView:(UITableView *)tv cellForRowAtIndexPath:(NSIndexPath *)ip
{
	UITableViewCell *c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
	if (ip.section == 0) {
		UISwitch *sw = [UISwitch new];
		sw.on = ![self.state isEqualToString:@"off"] && ![self.state isEqualToString:@"missing"];
		[sw addTarget:self action:@selector(toggle:) forControlEvents:UIControlEventValueChanged];
		c.textLabel.text = self.title;
		c.detailTextLabel.text = self.state;
		c.accessoryView = sw;
		c.selectionStyle = UITableViewCellSelectionStyleNone;
		return c;
	}
	if (self.items.count == 0) {
		c.textLabel.text = self.bluetooth ? @"No devices yet. Tap Scan." : @"No networks yet. Tap Scan.";
		c.textLabel.textColor = UIColor.secondaryLabelColor;
		c.selectionStyle = UITableViewCellSelectionStyleNone;
		return c;
	}
	NSArray<NSString *> *f = self.items[(NSUInteger)ip.row];
	if (self.bluetooth) {      /* addr, name, rssi, paired, connected */
		BOOL paired = f.count > 3 && [f[3] isEqualToString:@"1"], conn = f.count > 4 && [f[4] isEqualToString:@"1"];
		c.textLabel.text = f.count > 1 && f[1].length ? f[1] : f[0];
		c.detailTextLabel.text = conn ? @"Connected" : paired ? @"Paired" : f[0];
		c.imageView.image = SCSymbol(conn ? @"headphones.circle.fill" : @"headphones");
		c.accessoryType = conn ? UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone;
	} else {                   /* ssid, rssi, secure, connected */
		BOOL secure = f.count > 2 && [f[2] isEqualToString:@"1"], conn = f.count > 3 && [f[3] isEqualToString:@"1"];
		int rssi = f.count > 1 ? f[1].intValue : -100;
		c.textLabel.text = f[0];
		c.detailTextLabel.text = [NSString stringWithFormat:@"%@%d dBm%@", conn ? @"Connected · " : @"", rssi,
		                          secure ? @" · secured" : @""];
		c.imageView.image = SCSymbol(rssi > -60 ? @"wifi" : rssi > -75 ? @"wifi.exclamationmark" : @"wifi.slash");
		c.accessoryType = conn ? UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone;
		if (secure && !conn) {
			c.accessoryView = [[UIImageView alloc] initWithImage:SCSymbol(@"lock.fill")];
		}
	}
	return c;
}

- (void)tableView:(UITableView *)tv didSelectRowAtIndexPath:(NSIndexPath *)ip
{
	[tv deselectRowAtIndexPath:ip animated:YES];
	if (ip.section != 1 || self.items.count == 0) {
		return;
	}
	NSArray<NSString *> *f = self.items[(NSUInteger)ip.row];
	if (self.bluetooth) {
		BOOL paired = f.count > 3 && [f[3] isEqualToString:@"1"], conn = f.count > 4 && [f[4] isEqualToString:@"1"];
		NSString *addr = f[0], *name = f.count > 1 && f[1].length ? f[1] : f[0];
		UIAlertController *a = [UIAlertController alertControllerWithTitle:name message:addr
		                                                    preferredStyle:UIAlertControllerStyleActionSheet];
		if (!paired) {
			[a addAction:[UIAlertAction actionWithTitle:@"Pair" style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) {
				[self send:[@"pair " stringByAppendingString:addr]];
			}]];
		} else if (!conn) {
			[a addAction:[UIAlertAction actionWithTitle:@"Connect" style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) {
				[self send:[@"connect " stringByAppendingString:addr]];
			}]];
		} else {
			[a addAction:[UIAlertAction actionWithTitle:@"Disconnect" style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) {
				[self send:[@"disconnect " stringByAppendingString:addr]];
			}]];
		}
		if (paired) {
			[a addAction:[UIAlertAction actionWithTitle:@"Forget This Device" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *x) {
				[self send:[@"forget " stringByAppendingString:addr]];
			}]];
		}
		[a addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
		a.popoverPresentationController.sourceView = [tv cellForRowAtIndexPath:ip];
		[self presentViewController:a animated:YES completion:nil];
		return;
	}
	NSString *ssid = f[0];
	BOOL secure = f.count > 2 && [f[2] isEqualToString:@"1"], conn = f.count > 3 && [f[3] isEqualToString:@"1"];
	if (conn) {
		UIAlertController *a = [UIAlertController alertControllerWithTitle:ssid message:@"Connected"
		                                                    preferredStyle:UIAlertControllerStyleAlert];
		[a addAction:[UIAlertAction actionWithTitle:@"Disconnect" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *x) {
			[self send:@"leave"];
		}]];
		[a addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
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
		NSString *pw = a.textFields.firstObject.text ?: @"";
		[self send:[NSString stringWithFormat:@"join %@\t%@", ssid, pw]];
	}]];
	[self presentViewController:a animated:YES completion:nil];
}

@end

/* ---- app ---- */

@interface SCAppDelegate : UIResponder <UIApplicationDelegate>
@property (nonatomic, strong) UIWindow *window;
@end

@implementation SCAppDelegate

- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)opts
{
	SCListController *wifi = [SCListController new], *bt = [SCListController new];
	bt.bluetooth = YES;
	UINavigationController *n1 = [[UINavigationController alloc] initWithRootViewController:wifi];
	UINavigationController *n2 = [[UINavigationController alloc] initWithRootViewController:bt];
	n1.navigationBar.prefersLargeTitles = n2.navigationBar.prefersLargeTitles = YES;
	n1.tabBarItem = [[UITabBarItem alloc] initWithTitle:@"Wi-Fi" image:SCSymbol(@"wifi") tag:0];
	n2.tabBarItem = [[UITabBarItem alloc] initWithTitle:@"Bluetooth" image:SCSymbol(@"dot.radiowaves.left.and.right") tag:1];
	UITabBarController *tabs = [UITabBarController new];
	tabs.viewControllers = @[ n1, n2 ];
	self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
	self.window.rootViewController = tabs;
	[self.window makeKeyAndVisible];
	return YES;
}

@end

int main(int argc, char *argv[])
{
	@autoreleasepool {
		return UIApplicationMain(argc, argv, nil, NSStringFromClass([SCAppDelegate class]));
	}
}
