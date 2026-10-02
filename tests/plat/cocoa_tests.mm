// Cocoa-only data-transfer checks against a private, uniquely named
// NSPasteboard: the general pasteboard (the user's clipboard) is never
// touched. Needs a login session's pasteboard server, so like
// plat_win32_tests it is not a ctest test — run it directly on a Mac.
#include "cocoa/cocoa_internal.h"
#include "test_util.h"

#include <algorithm>
#include <cstring>

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

} // namespace

int main() {
    runCase("a TIFF-only picture is offered and read as PNG", caseTiffOnlyPictureReadsAsPng);
    return summary();
}
