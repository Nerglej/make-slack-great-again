// Image decoding: stb_image (PNG, JPEG, GIF) and optionally libwebp, into
// premultiplied ARGB32. The only file that sees decoder types, so the
// Windows/macOS builds can swap it for WIC/ImageIO.
#include "gfx/internal.h"

#include "stb/stb_config.h"
#include "stb/stb_image.h"

#ifdef MSGA_GFX_WEBP
#include <webp/decode.h>
#endif

#include <cstdlib>

namespace gfx {

namespace {

// Straight RGBA bytes → premultiplied 0xAARRGGBB.
void premultiplyRgba(const uint8_t *src, uint32_t *dst, size_t n) {
    for (size_t i = 0; i < n; ++i, src += 4) {
        const uint32_t a = src[3];
        if (a == 255)
            dst[i] = 0xff000000u | (uint32_t(src[0]) << 16) | (uint32_t(src[1]) << 8) | src[2];
        else if (a == 0)
            dst[i] = 0;
        else
            dst[i] = (a << 24) | (div255(src[0] * a) << 16) | (div255(src[1] * a) << 8) |
                     div255(src[2] * a);
    }
}

bool tooBig(int w, int h) {
    return w <= 0 || h <= 0 || int64_t(w) * h > kMaxImagePixels;
}

bool isGif(std::string_view b) {
    return b.size() >= 6 && b.substr(0, 4) == "GIF8";
}

#ifdef MSGA_GFX_WEBP
bool decodeWebp(std::string_view bytes, Bitmap *out) {
    int         w = 0, h = 0;
    const auto *d = reinterpret_cast<const uint8_t *>(bytes.data());
    if (!WebPGetInfo(d, bytes.size(), &w, &h) || tooBig(w, h))
        return false;
    uint8_t *rgba = WebPDecodeRGBA(d, bytes.size(), &w, &h);
    if (!rgba)
        return false;
    *out = Bitmap(w, h);
    premultiplyRgba(rgba, out->pixels(), size_t(w) * size_t(h));
    WebPFree(rgba);
    return true;
}
#endif

} // namespace

bool canDecodeImage(std::string_view head) {
    const auto starts = [&](std::string_view m) {
        return head.size() >= m.size() && head.compare(0, m.size(), m) == 0;
    };
    if (starts("\x89PNG") || starts("\xFF\xD8\xFF") || isGif(head))
        return true;
#ifdef MSGA_GFX_WEBP
    WebPBitstreamFeatures f;
    if (head.size() >= 12 && starts("RIFF") && head.compare(8, 4, "WEBP") == 0)
        return WebPGetFeatures(reinterpret_cast<const uint8_t *>(head.data()), head.size(), &f) ==
                   VP8_STATUS_OK &&
               !f.has_animation; // WebPDecodeRGBA refuses those
#endif
    return false;
}

bool decodeImage(std::string_view bytes, Bitmap *out) {
    if (bytes.empty() || bytes.size() > 0x7fffffff)
        return false;
#ifdef MSGA_GFX_WEBP
    if (bytes.size() >= 12 && bytes.substr(0, 4) == "RIFF" && bytes.substr(8, 4) == "WEBP")
        return decodeWebp(bytes, out);
#endif
    const auto *d = reinterpret_cast<const stbi_uc *>(bytes.data());
    const int   n = int(bytes.size());
    int         w = 0, h = 0, comp = 0;
    // Check the header first so a decompression bomb never gets allocated.
    if (!stbi_info_from_memory(d, n, &w, &h, &comp) || tooBig(w, h))
        return false;
    stbi_uc *rgba = stbi_load_from_memory(d, n, &w, &h, &comp, 4);
    if (!rgba)
        return false;
    *out = Bitmap(w, h);
    premultiplyRgba(rgba, out->pixels(), size_t(w) * size_t(h));
    stbi_image_free(rgba);
    return true;
}

bool decodeAnimation(std::string_view bytes, std::vector<AnimFrame> *out) {
    out->clear();
    if (!isGif(bytes) || bytes.size() > 0x7fffffff) {
        Bitmap b;
        if (!decodeImage(bytes, &b))
            return false;
        out->push_back({std::move(b)});
        return true;
    }
    const auto *d = reinterpret_cast<const stbi_uc *>(bytes.data());
    int         w = 0, h = 0, frames = 0, comp = 0;
    int        *delays = nullptr;
    if (!stbi_info_from_memory(d, int(bytes.size()), &w, &h, &comp) || tooBig(w, h))
        return false;
    // stb composites every frame onto the canvas, honouring the disposal
    // methods (none / background / previous).
    stbi_uc *all =
        stbi_load_gif_from_memory(d, int(bytes.size()), &delays, &w, &h, &frames, &comp, 4);
    if (!all)
        return false;
    const size_t px = size_t(w) * size_t(h);
    out->reserve(size_t(frames));
    for (int i = 0; i < frames; ++i) {
        AnimFrame f{Bitmap(w, h), 100};
        premultiplyRgba(all + px * 4 * size_t(i), f.frame.pixels(), px);
        // Browsers treat 0 and 10 ms as "unspecified" and play them at
        // 100 ms; anything else is kept, so the effective minimum is 20 ms.
        const int dm = delays ? delays[i] : 0;
        f.delayMs    = dm <= 10 ? 100 : dm;
        out->push_back(std::move(f));
    }
    std::free(delays);
    stbi_image_free(all);
    return !out->empty();
}

} // namespace gfx
