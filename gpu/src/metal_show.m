/*
 * metal_show: a picture drawn by Metal, put on the panel at boot, and checked.
 *
 * Every time Metal has been responsible for something a person could look at on this phone, it was black
 * (whole-system Metal, the poster snapshots, the Astronomy editor), while the driver's log said the draws ran.
 * This separates the two halves. It makes an IOSurface the size of the display, has Metal fill it -- a render
 * pass's clear, then texture uploads and blits, which need no shader -- reads the surface's own memory back and
 * says whether each patch holds the colour that was asked for, and hands the surface to the display with the same
 * three IOMobileFramebuffer calls the window server uses. So after one boot:
 *
 *   the log says PASS and the bars are on the screen   Metal draws and the display shows it: look further up
 *   PASS, nothing on the screen                        the swap, not Metal
 *   FAIL                                               Metal itself (which patch, and what it held instead)
 *
 * The process must be on the kernel's sweetmetalfor list (xnu 0128) or there is no device. Run early in the boot:
 * the next frame the window server presents replaces the picture, which is the "then back to iOS".
 *
 *   metal_show [seconds on screen, default 4]
 */
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurfaceRef.h>
#import <Metal/Metal.h>
#import <CoreGraphics/CoreGraphics.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

@protocol ShowDevice    /* RedwoodMetal's, and Apple's private, IOSurface-backed texture */
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d iosurface:(IOSurfaceRef)s plane:(NSUInteger)p;
@end

#define BARS 8
static const uint8_t bar_bgra[BARS][4] = {      /* B, G, R, A: white yellow cyan green magenta red blue grey */
	{ 255, 255, 255, 255 }, { 0, 255, 255, 255 }, { 255, 255, 0, 255 }, { 0, 255, 0, 255 },
	{ 255, 0, 255, 255 }, { 0, 0, 255, 255 }, { 255, 0, 0, 255 }, { 128, 128, 128, 255 },
};
static const char *const bar_name[BARS] = { "white", "yellow", "cyan", "green", "magenta", "red", "blue", "grey" };

static void *
sym(void *lib, const char *name)
{
	void *p = dlsym(lib, name);

	if (p == NULL) {
		printf("metal_show: no %s\n", name);
	}
	return p;
}

static int
rgb_near(const uint8_t *px, const uint8_t *want)
{
	for (int i = 0; i < 3; i++) {
		if (abs((int)px[i] - (int)want[i]) > 6) {
			return 0;
		}
	}
	return 1;
}

int
main(int argc, char **argv)
{
	unsigned seconds = argc > 1 ? (unsigned)atoi(argv[1]) : 4;
	void *fbl = dlopen("/System/Library/PrivateFrameworks/IOMobileFramebuffer.framework/IOMobileFramebuffer", RTLD_NOW);
	int (*get_main)(void **) = fbl ? sym(fbl, "IOMobileFramebufferGetMainDisplay") : NULL;
	int (*get_size)(void *, CGSize *) = fbl ? sym(fbl, "IOMobileFramebufferGetDisplaySize") : NULL;
	int (*swap_begin)(void *, int *) = fbl ? sym(fbl, "IOMobileFramebufferSwapBegin") : NULL;
	int (*swap_layer)(void *, int, IOSurfaceRef, CGRect, CGRect, int) = fbl ? sym(fbl, "IOMobileFramebufferSwapSetLayer") : NULL;
	int (*swap_end)(void *) = fbl ? sym(fbl, "IOMobileFramebufferSwapEnd") : NULL;
	void *fb = NULL;
	CGSize size = CGSizeMake(1080, 2220);
	int r;

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (get_main == NULL || (r = get_main(&fb)) != 0 || fb == NULL) {
		printf("metal_show: no main display (the picture is still drawn and checked)\n");
		fb = NULL;
	} else if (get_size != NULL && get_size(fb, &size) == 0) {
		printf("metal_show: display %.0f x %.0f\n", size.width, size.height);
	}
	unsigned w = (unsigned)size.width, h = (unsigned)size.height;

	if (w < 64 || h < 64 || w > 4096 || h > 4096) {
		w = 1080;
		h = 2220;
	}
	unsigned row = (w * 4 + 63) & ~63u;

	@autoreleasepool {
		id<MTLDevice> device = MTLCreateSystemDefaultDevice();

		if (device == nil) {
			printf("metal_show: FAIL no Metal device (is metal_show on the sweetmetalfor list?)\n");
			return 1;
		}
		printf("metal_show: device %s\n", [[device name] UTF8String]);
		NSDictionary *props = @{
			(id)kIOSurfaceWidth: @(w), (id)kIOSurfaceHeight: @(h), (id)kIOSurfaceBytesPerElement: @4,
			(id)kIOSurfaceBytesPerRow: @(row), (id)kIOSurfaceAllocSize: @((size_t)row * h),
			(id)kIOSurfacePixelFormat: @((unsigned)'BGRA'),
		};
		IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)props);

		if (surface == NULL) {
			printf("metal_show: FAIL no IOSurface of %u x %u\n", w, h);
			return 1;
		}
		MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
											 width:w height:h mipmapped:NO];

		d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
		if (![device respondsToSelector:@selector(newTextureWithDescriptor:iosurface:plane:)]) {
			printf("metal_show: FAIL the device makes no texture of an IOSurface\n");
			return 1;
		}
		id<MTLTexture> target = [(id<ShowDevice>)device newTextureWithDescriptor:d iosurface:surface plane:0];

		if (target == nil) {
			printf("metal_show: FAIL no texture of the IOSurface\n");
			return 1;
		}
		id<MTLCommandQueue> queue = [device newCommandQueue];
		id<MTLCommandBuffer> cb = [queue commandBuffer];

		/* 1: the render pass's clear, a dark blue that no bar has */
		MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];

		pass.colorAttachments[0].texture = target;
		pass.colorAttachments[0].loadAction = MTLLoadActionClear;
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.25, 1.0);
		[[cb renderCommandEncoderWithDescriptor:pass] endEncoding];

		/* 2: eight bars, each a texture filled from the processor and blitted in; and a ramp under them */
		unsigned bw = (w - 80) / BARS, bh = h / 3;
		uint8_t *texels = malloc((size_t)bw * bh * 4);
		id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
		MTLTextureDescriptor *bd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
											  width:bw height:bh mipmapped:NO];
		NSMutableArray *keep = [NSMutableArray array];

		for (unsigned b = 0; b <= BARS; b++) {
			unsigned tw = b < BARS ? bw : bw * BARS, th = b < BARS ? bh : bh / 2;
			MTLTextureDescriptor *td = b < BARS ? bd :
			    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
									       width:tw height:th mipmapped:NO];
			id<MTLTexture> t = [device newTextureWithDescriptor:td];

			if (t == nil) {
				printf("metal_show: FAIL no texture for patch %u\n", b);
				return 1;
			}
			if (b < BARS) {
				for (size_t i = 0; i < (size_t)bw * bh; i++) {
					memcpy(texels + i * 4, bar_bgra[b], 4);
				}
			} else {        /* a grey ramp, left to right */
				texels = realloc(texels, (size_t)tw * th * 4);
				for (unsigned y = 0; y < th; y++) {
					for (unsigned x = 0; x < tw; x++) {
						uint8_t v = (uint8_t)(x * 255u / (tw - 1));
						uint8_t px[4] = { v, v, v, 255 };

						memcpy(texels + ((size_t)y * tw + x) * 4, px, 4);
					}
				}
			}
			[t replaceRegion:MTLRegionMake2D(0, 0, tw, th) mipmapLevel:0 withBytes:texels bytesPerRow:tw * 4];
			[keep addObject:t];
			[blit copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
				   sourceSize:MTLSizeMake(tw, th, 1) toTexture:target destinationSlice:0 destinationLevel:0
			    destinationOrigin:MTLOriginMake(40 + (b < BARS ? b * bw : 0), b < BARS ? h / 6 : h / 6 + bh + 40, 0)];
		}
		[blit endEncoding];
		[cb commit];
		[cb waitUntilCompleted];
		printf("metal_show: command buffer status %lu%s\n", (unsigned long)[cb status],
		    [cb error] ? [[[cb error] description] UTF8String] : "");
		free(texels);

		/* 3: what the surface's own memory holds now */
		IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
		const uint8_t *base = IOSurfaceGetBaseAddress(surface);
		size_t srow = IOSurfaceGetBytesPerRow(surface);
		const uint8_t clear[4] = { 64, 0, 0, 255 };
		const uint8_t *px = base + (size_t)20 * srow + 20 * 4;
		int ok = rgb_near(px, clear), all = ok;
		size_t nonblack = 0, samples = 0;

		printf("metal_show: clear   %s (B G R = %u %u %u, wanted 64 0 0)\n", ok ? "ok  " : "FAIL", px[0], px[1], px[2]);
		for (unsigned b = 0; b < BARS; b++) {
			px = base + (size_t)(h / 6 + bh / 2) * srow + (size_t)(40 + b * bw + bw / 2) * 4;
			ok = rgb_near(px, bar_bgra[b]);
			all &= ok;
			printf("metal_show: %-7s %s (B G R = %u %u %u)\n", bar_name[b], ok ? "ok  " : "FAIL", px[0], px[1], px[2]);
		}
		px = base + (size_t)(h / 6 + bh + 40 + bh / 4) * srow + (size_t)(40 + bw * BARS / 2) * 4;
		ok = px[0] > 110 && px[0] < 145 && px[0] == px[1] && px[1] == px[2];
		all &= ok;
		printf("metal_show: ramp    %s (middle = %u %u %u, wanted about 127)\n", ok ? "ok  " : "FAIL", px[0], px[1], px[2]);
		for (unsigned y = 0; y < h; y += 16) {
			for (unsigned x = 0; x < w; x += 16) {
				const uint8_t *p = base + (size_t)y * srow + (size_t)x * 4;

				samples++;
				nonblack += (p[0] | p[1] | p[2]) != 0;
			}
		}
		IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
		printf("metal_show: %zu of %zu sampled texels are not black\n", nonblack, samples);
		printf("metal_show: %s: Metal %s the picture into the surface\n", all ? "PASS" : "FAIL", all ? "drew" : "did not draw");

		/* 4: on the panel, the way the window server puts a frame there */
		if (fb != NULL && swap_begin != NULL && swap_layer != NULL && swap_end != NULL) {
			CGRect whole = CGRectMake(0, 0, w, h);
			int token = 0, r1 = swap_begin(fb, &token), r2 = swap_layer(fb, 0, surface, whole, whole, 0), r3 = swap_end(fb);

			printf("metal_show: swap begin %d (id %d) layer %d end %d; on the screen for %u s\n", r1, token, r2, r3, seconds);
			sleep(seconds);
		} else {
			printf("metal_show: not shown (no display or no swap calls)\n");
		}
		CFRelease(surface);
		return all ? 0 : 2;
	}
}
