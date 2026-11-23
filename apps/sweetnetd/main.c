/* Copyright (c) 2026 Darin. MIT License (see LICENSE); keep this notice. Written by Claude (Anthropic) for the ios-sweet Galaxy A8 port. */
/*
 * sweetnetd: gives iOS the A8's Wi-Fi address from boot, without Sweet Connect open.
 *
 * en2 is a plain kernel interface (no IONetworkingFamily), so configd's IPConfiguration never runs DHCP on it;
 * the kernel's Wi-Fi driver does (bsd/dev/sweet_a8wlbt.c) and reports "ip ADDR\tMASK\tROUTER\tDNS\ten2" in the
 * text sysctl kern.sweet_wifi once it has a lease. Every 2 s this daemon posts that as a state-only network
 * service (IPv4 + DNS, like VPN agents do; IPMonitor accepts services without a Setup: entry), and removes it
 * when the line disappears (link lost). Same logic as Sweet Connect's SCPublishNetwork (apps/SweetConnect).
 * Runs as root from launchd (com.iossweet.sweetnetd), needs SCDynamicStore-write-access.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/sysctl.h>

typedef const struct __SCDynamicStore *SCDynamicStoreRef;
extern SCDynamicStoreRef SCDynamicStoreCreate(CFAllocatorRef, CFStringRef, void *, void *);
extern Boolean SCDynamicStoreSetMultiple(SCDynamicStoreRef, CFDictionaryRef, CFArrayRef, CFArrayRef);

#define SVC "State:/Network/Service/com.iossweet.wifi"

static CFStringRef S(const char *s) { return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8); }

static CFArrayRef
one(const char *s)
{
	CFStringRef v = S(s);
	CFArrayRef a = CFArrayCreate(NULL, (const void **)&v, 1, &kCFTypeArrayCallBacks);
	CFRelease(v);
	return a;
}

static void
publish(SCDynamicStoreRef st, const char *ip)        /* ip = "ADDR\tMASK\tROUTER\tDNS\tIF" or NULL */
{
	CFStringRef k4 = S(SVC "/IPv4"), kd = S(SVC "/DNS");
	if (ip == NULL) {
		CFStringRef keys[2] = { k4, kd };
		CFArrayRef rm = CFArrayCreate(NULL, (const void **)keys, 2, &kCFTypeArrayCallBacks);
		SCDynamicStoreSetMultiple(st, NULL, rm, NULL);
		CFRelease(rm);
		printf("sweetnetd: Wi-Fi address removed\n");
	} else {
		char f[5][64] = { "", "", "", "", "" };
		const char *p = ip;
		for (int i = 0; i < 5 && p; i++) {
			const char *t = strchr(p, '\t');
			size_t n = t ? (size_t)(t - p) : strlen(p);
			if (n >= sizeof(f[i])) {
				n = sizeof(f[i]) - 1;
			}
			memcpy(f[i], p, n);
			f[i][n] = 0;
			p = t ? t + 1 : NULL;
		}
		CFMutableDictionaryRef v4 = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		CFArrayRef a = one(f[0]), m = one(f[1]);
		CFStringRef r = S(f[2]), ifn = S(f[4][0] ? f[4] : "en2");
		CFDictionarySetValue(v4, CFSTR("Addresses"), a);
		CFDictionarySetValue(v4, CFSTR("SubnetMasks"), m);
		CFDictionarySetValue(v4, CFSTR("Router"), r);
		CFDictionarySetValue(v4, CFSTR("InterfaceName"), ifn);
		CFMutableArrayRef dns = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
		if (f[3][0] && strcmp(f[3], "0.0.0.0") != 0) {
			CFStringRef d = S(f[3]);
			CFArrayAppendValue(dns, d);
			CFRelease(d);
		}
		CFArrayAppendValue(dns, CFSTR("1.1.1.1"));
		CFArrayAppendValue(dns, CFSTR("8.8.8.8"));
		CFMutableDictionaryRef dd = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		CFDictionarySetValue(dd, CFSTR("ServerAddresses"), dns);
		const void *keys[2] = { k4, kd }, *vals[2] = { v4, dd };
		CFDictionaryRef set = CFDictionaryCreate(NULL, keys, vals, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		Boolean ok = SCDynamicStoreSetMultiple(st, set, NULL, NULL);
		printf("sweetnetd: Wi-Fi address %s router %s on %s: %s\n", f[0], f[2], f[4], ok ? "posted" : "REFUSED");
		CFRelease(set); CFRelease(dd); CFRelease(dns); CFRelease(v4); CFRelease(a); CFRelease(m); CFRelease(r); CFRelease(ifn);
	}
	fflush(stdout);
	CFRelease(k4);
	CFRelease(kd);
}

int
main(void)
{
	SCDynamicStoreRef st = SCDynamicStoreCreate(NULL, CFSTR("sweetnetd"), NULL, NULL);
	char last[256] = "", text[8192];

	if (st == NULL) {
		fprintf(stderr, "sweetnetd: no SCDynamicStore\n");
		return 1;
	}
	printf("sweetnetd: watching kern.sweet_wifi\n");
	fflush(stdout);
	for (;;) {
		size_t len = sizeof(text) - 1;
		char cur[256] = "";
		if (sysctlbyname("kern.sweet_wifi", text, &len, NULL, 0) == 0) {
			text[len] = 0;
			char *ip = strstr(text, "\nip ");
			if (ip == NULL && strncmp(text, "ip ", 3) == 0) {
				ip = text - 1;
			}
			if (ip) {
				char *e = strchr(ip + 1, '\n');
				size_t n = e ? (size_t)(e - (ip + 4)) : strlen(ip + 4);
				if (n >= sizeof(cur)) {
					n = sizeof(cur) - 1;
				}
				memcpy(cur, ip + 4, n);
				cur[n] = 0;
			}
		}
		if (strcmp(cur, last) != 0) {
			publish(st, cur[0] ? cur : NULL);
			strlcpy(last, cur, sizeof(last));
		}
		sleep(2);
	}
}
