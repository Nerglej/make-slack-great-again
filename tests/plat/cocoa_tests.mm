// Cocoa-only checks: data transfer against a private, uniquely named
// NSPasteboard (the general pasteboard, the user's clipboard, is never
// touched) and the IOSurface present path. Needs a login session (pasteboard
// and window server), so like plat_win32_tests it is not a ctest test — run
// it directly on a Mac.
#include "cocoa/cocoa_internal.h"
#include "core/backends.h"
#include "test_util.h"

#include <algorithm>
#include <cstring>
#include <set>

using namespace plat;
using namespace plat_test;

namespace {

bool has(const std::vector<std::string> &v, const char *s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

// A 2×2 picture with distinct opaque pixels, as many apps copy one: TIFF only.
NSData *tiffPicture() {
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
                                                                    pixelsWide:2
                                                                    pixelsHigh:2
                                                                 bitsPerSample:8
                                                               samplesPerPixel:4
                                                                      hasAlpha:YES
                                                                      isPlanar:NO
                                                                colorSpaceName:NSDeviceRGBColorSpace
                                                                   bytesPerRow:8
                                                                  bitsPerPixel:32];
    const unsigned char px[16] = {
        255,
        0,
        0,
        255,
        /**/ 0,
        255,
        0,
        255,
        /**/ 0,
        0,
        255,
        255,
        /**/ 255,
        255,
        255,
        255,
    };
    std::memcpy(rep.bitmapData, px, sizeof px);
    return [rep TIFFRepresentation];
}

void caseTiffOnlyPictureReadsAsPng() {
    @autoreleasepool {
        NSPasteboard *pb = [NSPasteboard pasteboardWithUniqueName];
        [pb clearContents];
        NSData *tiff = tiffPicture();
        CHECK(tiff.length > 0);
        CHECK([pb setData:tiff forType:NSPasteboardTypeTIFF]);

        // Offered as image/png too, though only TIFF is on the pasteboard.
        const std::vector<std::string> mimes = cocoa::pasteboardMimes(pb);
        CHECK(has(mimes, "image/tiff"));
        CHECK(has(mimes, "image/png"));
        CHECK([pb dataForType:NSPasteboardTypePNG] == nil);

        // Read as PNG: converted, same pixels.
        const std::optional<std::string> png = cocoa::readPasteboard(pb, "image/png");
        CHECK(png && png->size() > 8 && png->compare(0, 4, "\x89PNG") == 0);
        if (png) {
            NSData           *d   = [NSData dataWithBytes:png->data() length:png->size()];
            NSBitmapImageRep *rep = [NSBitmapImageRep imageRepWithData:d];
            CHECK(rep && rep.pixelsWide == 2 && rep.pixelsHigh == 2);
            if (rep) {
                NSUInteger p[4] = {};
                [rep getPixel:p atX:0 y:0];
                CHECK(p[0] == 255 && p[1] == 0 && p[2] == 0);
                [rep getPixel:p atX:1 y:1];
                CHECK(p[0] == 255 && p[1] == 255 && p[2] == 255);
            }
        }
        // The TIFF itself is still there as it was.
        const std::optional<std::string> raw = cocoa::readPasteboard(pb, "image/tiff");
        CHECK(raw && raw->size() == tiff.length);

        // A real PNG on the pasteboard is returned as is, no extra mime.
        [pb clearContents];
        [pb setData:[NSData dataWithBytes:png->data() length:png->size()]
            forType:NSPasteboardTypePNG];
        const std::vector<std::string> pngOnly = cocoa::pasteboardMimes(pb);
        CHECK(has(pngOnly, "image/png") && !has(pngOnly, "image/tiff"));
        const std::optional<std::string> same = cocoa::readPasteboard(pb, "image/png");
        CHECK(same && *same == *png);

        // Nothing usable: no PNG.
        [pb clearContents];
        [pb setString:@"text" forType:NSPasteboardTypeString];
        CHECK(!cocoa::readPasteboard(pb, "image/png"));
        CHECK(!has(cocoa::pasteboardMimes(pb), "image/png"));

        [pb releaseGlobally];
    }
}

// Every pixel of the surface the layer shows equals `want` (w×h, tight rows).
bool presentedEquals(cocoa::CocoaWindow &win, const std::vector<uint32_t> &want, int w, int h) {
    IOSurfaceRef s = win.presentedSurface();
    if (!s || int(IOSurfaceGetWidth(s)) != w || int(IOSurfaceGetHeight(s)) != h)
        return false;
    IOSurfaceLock(s, kIOSurfaceLockReadOnly, nullptr);
    const auto *base = static_cast<const uint8_t *>(IOSurfaceGetBaseAddress(s));
    bool        same = true;
    for (int y = 0; y < h && same; ++y)
        same =
            std::memcmp(
                base + size_t(y) * IOSurfaceGetBytesPerRow(s), &want[size_t(y) * w], size_t(w) * 4
            ) == 0;
    IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, nullptr);
    return same;
}

bool canvasEquals(const Canvas &c, const std::vector<uint32_t> &want) {
    for (int y = 0; y < c.height; ++y)
        if (std::memcmp(
                c.pixels + size_t(y) * c.stride, &want[size_t(y) * c.width], size_t(c.width) * 4
            ))
            return false;
    return true;
}

void pumpRunLoop(double seconds) {
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);
}

// Partial repaints go into a surface other than the one on screen, which must
// first catch up with the frames it missed: the canvas always starts as the
// last presented frame, and the presented frame is exactly what was painted.
void casePartialPresentsKeepEveryFrame() {
    @autoreleasepool {
        std::string          err;
        std::unique_ptr<App> app = createCocoaApp(&err);
        if (!app) {
            skip("no window-server session: " + err);
            return;
        }
        std::unique_ptr<Window> win =
            app->createWindow({.title = "plat present", .size = {160, 120}});
        auto &cw = static_cast<cocoa::CocoaWindow &>(*win);
        pumpRunLoop(0.2);

        Canvas c = win->beginPaint();
        CHECK(c.pixels && c.width > 0 && c.height > 0 && c.stride >= c.width);
        if (!c.pixels)
            return;
        const int             w = c.width, h = c.height;
        std::vector<uint32_t> model(size_t(w) * h, 0xff102030);
        for (int y = 0; y < h; ++y)
            std::memcpy(c.pixels + size_t(y) * c.stride, &model[size_t(y) * w], size_t(w) * 4);
        win->endPaint({});
        CHECK(presentedEquals(cw, model, w, h));

        std::set<IOSurfaceRef> seen{cw.presentedSurface()};
        IOSurfaceRef           prev = cw.presentedSurface();
        uint32_t               seed = 1;
        auto                   next = [&](int n) {
            seed = seed * 1103515245u + 12345u;
            return int((seed >> 8) % uint32_t(n));
        };
        for (int f = 0; f < 40; ++f) {
            c = win->beginPaint();
            CHECK(c.pixels && c.width == w && c.height == h);
            if (!c.pixels)
                return;
            CHECK(canvasEquals(c, model));
            // One to three small rects, some partly outside the canvas.
            std::vector<plat::Rect> damage;
            for (int k = 1 + next(3); k > 0; --k) {
                const plat::Rect r{
                    next(w + 10) - 5, next(h + 10) - 5, 1 + next(w / 3), 1 + next(h / 3)
                };
                damage.push_back(r);
                const uint32_t col = 0xff000000u | (uint32_t(f * 40503 + k * 977) & 0xffffff);
                for (int y = std::max(0, r.y); y < std::min(h, r.y + r.h); ++y)
                    for (int x = std::max(0, r.x); x < std::min(w, r.x + r.w); ++x) {
                        c.pixels[size_t(y) * c.stride + x] = col;
                        model[size_t(y) * w + x]           = col;
                    }
            }
            win->endPaint(damage);
            CHECK(presentedEquals(cw, model, w, h));
            CHECK(cw.presentedSurface() != prev); // never repaints the one on screen
            prev = cw.presentedSurface();
            seen.insert(prev);
            if (f % 4 == 0)
                pumpRunLoop(0.03); // let the window server take frames
        }
        CHECK(seen.size() >= 2 && seen.size() <= 4);

        // A resize keeps what fits of the old frame; the rest starts black.
        win->setSize({200, 100});
        pumpRunLoop(0.1);
        c = win->beginPaint();
        CHECK(c.pixels && c.width != w);
        if (c.pixels) {
            bool kept = true;
            for (int y = 0; y < c.height; ++y)
                for (int x = 0; x < c.width; ++x) {
                    const uint32_t want = x < w && y < h ? model[size_t(y) * w + x] : 0xff000000u;
                    kept                = kept && c.pixels[size_t(y) * c.stride + x] == want;
                }
            CHECK(kept);
            win->endPaint({});
        }
        win.reset();
        pumpRunLoop(0.05);
    }
}

} // namespace

int main() {
    runCase("a TIFF-only picture is offered and read as PNG", caseTiffOnlyPictureReadsAsPng);
    runCase("partial presents keep every frame", casePartialPresentsKeepEveryFrame);
    return summary();
}
