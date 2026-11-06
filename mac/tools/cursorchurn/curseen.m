// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// curseen <secs>: frames of the screen WITH the real cursor (CGDisplayStream, kCGDisplayStreamShowCursor), and
// in each a 44x44 pt box at the pointer's desktop spot, where the real cursor is drawn. Over cursorchurn's plain
// grey window, a frame with many pixels far from grey there has the real cursor in it. Prints frames, frames
// with it, and the longest run. Starting it shows macOS's recording indicator.
#import <Cocoa/Cocoa.h>
#import <IOSurface/IOSurface.h>
#import <dlfcn.h>
typedef void (^H)(int, uint64_t, IOSurfaceRef, void *);
typedef void *(*CreateFn)(uint32_t, size_t, size_t, int32_t, CFDictionaryRef, dispatch_queue_t, H);
typedef int (*StartFn)(void *);
int main(int argc, char **argv) {
  @autoreleasepool {
    double secs = atof(argv[1]);
    void *cg = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", RTLD_NOW);
    CreateFn create = (CreateFn)dlsym(cg, "CGDisplayStreamCreateWithDispatchQueue");
    StartFn start = (StartFn)dlsym(cg, "CGDisplayStreamStart");
    __block long frames = 0, seen = 0, run = 0, longest = 0;
    NSDictionary *props = @{ @"kCGDisplayStreamShowCursor": @YES, @"kCGDisplayStreamMinimumFrameTime": @(1.0/240) };
    void *s = create(CGMainDisplayID(), 1920, 1080, 'BGRA', (__bridge CFDictionaryRef)props, dispatch_queue_create("cap", 0),
      ^(int status, uint64_t dt, IOSurfaceRef surf, void *upd) {
        if (status != 0 || !surf) return;
        CGEventRef e = CGEventCreate(NULL); CGPoint p = CGEventGetLocation(e); CFRelease(e);
        IOSurfaceLock(surf, kIOSurfaceLockReadOnly, NULL);
        uint8_t *b = IOSurfaceGetBaseAddress(surf); size_t r = IOSurfaceGetBytesPerRow(surf);
        int w = (int)IOSurfaceGetWidth(surf), h = (int)IOSurfaceGetHeight(surf), far = 0;
        for (int y = (int)p.y - 6; y < (int)p.y + 38; y++) for (int x = (int)p.x - 6; x < (int)p.x + 38; x++) {
          if (x < 0 || y < 0 || x >= w || y >= h) continue;
          uint8_t *q = b + y * r + x * 4; int v = q[1];
          if ((v < 50 || v > 215) && abs(q[0] - q[2]) < 30 && abs(q[1] - q[2]) < 30) far++; }
        const char *dump = getenv("CURSEEN_DUMP");
        static int dumped = 0;
        if (dump && far > 25 && dumped < 3) { dumped++;
          CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
          CGContextRef c = CGBitmapContextCreate(b, w, h, 8, r, cs, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
          CGImageRef im = CGBitmapContextCreateImage(c);
          NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithCGImage:im];
          NSString *f = [NSString stringWithFormat:@"%s-%d-%d-%d.png", dump, dumped, (int)p.x, (int)p.y];
          [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:f atomically:NO];
          CGImageRelease(im); CGContextRelease(c); CGColorSpaceRelease(cs); }
        IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, NULL);
        frames++;
        if (far > 25) { seen++; run++; if (run > longest) longest = run; } else run = 0;
      });
    if (!s || start(s) != 0) return 5;
    puts("ready"); fflush(stdout);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(secs * 1e9)), dispatch_get_main_queue(), ^{ printf("frames %ld seen %ld longest %ld\n", frames, seen, longest); exit(0); });
    dispatch_main();
  }
}
