// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// dispcap <out.png> [secs]: a CGDisplayStream frame of the screen after `secs` (through dlsym: the Swift and
// ObjC declarations are unavailable now). ScreenCaptureKit never shows a CABackdropLayer's magnification, so
// this is how Backdrop is seen. Running it shows macOS's recording indicator itself.
#import <Cocoa/Cocoa.h>
#import <IOSurface/IOSurface.h>
#import <dlfcn.h>
typedef void (^H)(int, uint64_t, IOSurfaceRef, void *);
typedef void *(*CreateFn)(uint32_t, size_t, size_t, int32_t, CFDictionaryRef, dispatch_queue_t, H);
typedef int (*StartFn)(void *);
int main(int argc, char **argv) {
  @autoreleasepool {
    const char *out = argv[1]; double secs = argc > 2 ? atof(argv[2]) : 1;
    void *cg = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", RTLD_NOW);
    CreateFn create = (CreateFn)dlsym(cg, "CGDisplayStreamCreateWithDispatchQueue");
    StartFn start = (StartFn)dlsym(cg, "CGDisplayStreamStart");
    if (!create || !start) return 3;
    NSDictionary *props = @{ @"kCGDisplayStreamShowCursor": @NO, @"kCGDisplayStreamMinimumFrameTime": @(1.0/240) };
    double t0 = CFAbsoluteTimeGetCurrent();
    void *s = create(CGMainDisplayID(), 1920, 1080, 'BGRA', (__bridge CFDictionaryRef)props, dispatch_queue_create("cap", 0),
      ^(int status, uint64_t dt, IOSurfaceRef surf, void *upd) {
        if (status != 0 || !surf || CFAbsoluteTimeGetCurrent() - t0 < secs) return;
        IOSurfaceLock(surf, kIOSurfaceLockReadOnly, NULL);
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGContextRef c = CGBitmapContextCreate(IOSurfaceGetBaseAddress(surf), IOSurfaceGetWidth(surf), IOSurfaceGetHeight(surf), 8, IOSurfaceGetBytesPerRow(surf), cs, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
        CGImageRef im = CGBitmapContextCreateImage(c);
        NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithCGImage:im];
        [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(out) atomically:NO];
        exit(0);
      });
    if (!s || start(s) != 0) return 5;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)((secs + 3) * 1e9)), dispatch_get_main_queue(), ^{ puts("no frame"); exit(1); });
    dispatch_main();
  }
}
