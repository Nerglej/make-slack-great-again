// Font index, face loading, style → face resolution, fallback and glyph
// rasterisation (Linux: own sfnt index + HarfBuzz + FreeType). Internal to
// text/; nothing outside this directory sees these types.
//
// Not thread-safe: like the rest of the toolkit, text runs on the UI thread.
#pragma once

#include "text/text.h"

#include <cstdint>
#include <string>

struct hb_font_t;

namespace text::fonts {

// A resolved font: face index | variation weight | synthetic styles, packed
// so runs and cache keys carry one integer.
using FontKey             = uint32_t;
constexpr FontKey kNoFont = 0xFFFFFFFFu;
inline uint32_t   faceOf(FontKey k) {
    return k & 0xFFFF;
}
// Variation weight; 0 = the face's default instance.
inline uint32_t wghtOf(FontKey k) {
    return (k >> 16) & 0x3FF;
}
inline bool synthBold(FontKey k) {
    return k & (1u << 26);
}
inline bool synthOblique(FontKey k) {
    return k & (1u << 27);
}
inline FontKey makeKey(uint32_t face, uint32_t wght, bool bold, bool oblique) {
    return face | (wght << 16) | (bold ? 1u << 26 : 0) | (oblique ? 1u << 27 : 0);
}

bool init(std::string *error);
// Frees every loaded face and file mapping (text::shutdown).
void shutdown();

// The UI family (or mono when s.mono) in the requested weight/italic.
FontKey primary(const Style &s);
// The colour emoji face, kNoFont when none is installed.
FontKey emoji();
// Some installed face that has a glyph for cp, styled like s; kNoFont if none.
FontKey fallback(uint32_t cp, const Style &s);

bool       hasGlyph(FontKey k, uint32_t cp);
hb_font_t *hbFont(FontKey k); // at scale = units per em, variations applied
float      upem(FontKey k);
bool       isColor(FontKey k);

// Per-em metrics (multiply by the pixel size). Descent is positive (below).
struct FaceMetrics {
    float ascent = 0.9f, descent = 0.25f, lineGap = 0;
    float capHeight    = 0.7f;                         // flat-topped capitals and digits
    float underlinePos = 0.1f, underlineThick = 0.06f; // pos: below the baseline
    float strikePos = 0.3f, strikeThick = 0.06f;       // pos: above the baseline
};
const FaceMetrics &metrics(FontKey k);

// One rasterised glyph. For masks `a8` points at `h` rows of `pitch` bytes;
// for colour glyphs `argb` points at premultiplied pixels (pitch in pixels).
// Valid until the next rasterize() call.
struct Raster {
    int             w = 0, h = 0, left = 0, top = 0, pitch = 0; // top: above the baseline
    bool            color = false;
    const uint8_t  *a8    = nullptr;
    const uint32_t *argb  = nullptr;
};
// ppem64: physical pixel size in 26.6; phase: horizontal subpixel offset in
// quarter pixels (0..3).
bool rasterize(FontKey k, uint32_t glyph, uint32_t ppem64, int phase, Raster *out);

// Diagnostics / tests: the family name (lowercase) a key resolves to.
std::string familyName(FontKey k);
size_t      faceCount();

} // namespace text::fonts
