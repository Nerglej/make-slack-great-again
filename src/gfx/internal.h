// Shared between gfx's own .cpp files; not part of the module's API.
#pragma once

#include "gfx/gfx.h"

#include <cmath>
#include <cstring>

namespace gfx {

// x / 255 rounded to nearest, exact for every x <= 255 * 255.
inline uint32_t div255(uint32_t x) {
    x += 128;
    return (x + (x >> 8)) >> 8;
}

// Every channel of a premultiplied pixel times a/255, rounded, two channels
// per multiply. Rounding (not truncating) is what keeps repeated blends from
// drifting darker.
inline uint32_t mulPx(uint32_t p, uint32_t a) {
    uint32_t rb = (p & 0x00ff00ffu) * a + 0x00800080u;
    rb          = ((rb + ((rb >> 8) & 0x00ff00ffu)) >> 8) & 0x00ff00ffu;
    uint32_t ag = ((p >> 8) & 0x00ff00ffu) * a + 0x00800080u;
    ag          = (ag + ((ag >> 8) & 0x00ff00ffu)) & 0xff00ff00u;
    return rb | ag;
}

inline float clamp01(float v) {
    return v < 0 ? 0 : v > 1 ? 1 : v;
}
inline int clampi(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}
inline int floori(float v) {
    return int(std::floor(v < -1e7f ? -1e7f : v > 1e7f ? 1e7f : v));
}
inline int ceili(float v) {
    return int(std::ceil(v < -1e7f ? -1e7f : v > 1e7f ? 1e7f : v));
}
inline uint8_t toByte(float c) {
    return uint8_t(c * 255.0f + 0.5f);
}

// Premultiplied source-over. Cannot overflow for valid premultiplied input.
inline uint32_t over(uint32_t s, uint32_t d) {
    return s + mulPx(d, 255 - (s >> 24));
}

// Straight colour × opacity → premultiplied pixel.
uint32_t premultiply(Color c, float opacity);

// A rounded rect in physical pixels; r is already clamped to half the size.
struct RR {
    float x0, y0, x1, y1, r;
};
RR makeRR(float x0, float y0, float x1, float y1, float r);

// Coverage of pixel row y, columns [x0, x1), by q → out[0 .. x1-x0).
// Returns true when every pixel is fully covered.
bool rrRow(const RR &q, int y, int x0, int x1, uint8_t *out);

// Flattened geometry in physical pixels.
struct Poly {
    uint32_t start, count;
    bool     closed;
};
struct Flat {
    std::vector<PointF> pts;
    std::vector<Poly>   polys;
};
// Appends `path` mapped through phys = p * scale + (ox, oy).
void flatten(const Path &path, float scale, float ox, float oy, Flat *out);

// Directed edges for the rasterizer (non-zero winding).
struct Seg {
    float x0, y0, x1, y1;
};
void fillSegs(const Flat &f, std::vector<Seg> *segs);
// Round caps + joins: the union of one capsule per segment (every piece wound
// the same way, so the non-zero rule unions them exactly).
void strokeSegs(const Flat &f, float halfWidth, std::vector<Seg> *segs);
// Same with explicit caps/joins (round/round above).
struct StrokeGeom {
    float        hw;
    Stroke::Cap  cap;
    Stroke::Join join;
    float        miterLimit;
};
void strokeSegs(const Flat &f, const StrokeGeom &g, std::vector<Seg> *segs);
// Splits every polyline of `in` into its dashes (lengths in the same units).
// False (out untouched) when the pattern is degenerate: draw solid then.
bool dashFlat(const Flat &in, const float *dashes, int count, float offset, Flat *out);

// Affine helpers: mul(l, r) applies r first, then l.
Affine        mul(const Affine &l, const Affine &r);
bool          invert(const Affine &m, Affine *out);
inline PointF apply(const Affine &m, float x, float y) {
    return {m.a * x + m.c * y + m.e, m.b * x + m.d * y + m.f};
}

// A gradient resolved for one fill: physical pixel centre → t → colour.
struct Shader {
    Affine   m; // physical px → gradient space
    uint8_t  kind, spread;
    float    ax, ay, dx, dy, inv; // linear: t = ((g − a)·d)·inv; radial: t = |g − a|·inv
    uint32_t lut[256];            // premultiplied, opacity applied
};
// False when the gradient paints nothing (no stops).
bool makeShader(const Gradient &g, const Affine &physToLogical, float opacity, Shader *out);
void shaderRow(const Shader &s, int y, int x0, int x1, uint32_t *out);

// gfx's own SVG renderer (svg.cpp); renderSvg() tries the OS first where
// there is one. Exposed for tests on every platform.
bool       renderSvgOwn(std::string_view svg, int width, int height, Bitmap *out);
// Which renderer produced the last renderSvg() result: 0 own, 1 the OS's.
// For tests and diagnostics.
extern int g_svgBackend;

struct PainterImpl {
    // Row span [x0, x1) of row y (already inside the rect clip), optional
    // coverage, through the round clips, blended with premultiplied pm.
    static void span(Painter &p, int y, int x0, int x1, const uint8_t *cov, uint32_t pm);
    // Same with per-pixel premultiplied source and a global alpha 0..255.
    static void spanPx(
        Painter &p, int y, int x0, int x1, const uint8_t *cov, const uint32_t *src, uint32_t alpha
    );
    // Coverage after the round clips (nullptr = fully covered).
    static const uint8_t *clipMask(Painter &p, int y, int x0, int x1, const uint8_t *cov);
    // outer minus inner (inner may be null), clipped.
    static void           fillRR(Painter &p, const RR &outer, const RR *inner, uint32_t pm);
    static void           rasterize(
        Painter                &p,
        const std::vector<Seg> &segs,
        uint32_t                pm,
        FillRule                rule   = FillRule::NonZero,
        const Shader           *shader = nullptr
    );
    static Affine physToLogical(const Painter &p) {
        const float k = 1 / p._scale;
        return {k, 0, 0, k, -p._s.tx, -p._s.ty};
    }
    // A8 mask at absolute physical position.
    static void      maskAt(Painter &p, const Mask8 &m, int x, int y, uint32_t pm);
    static uint8_t  *row8(Painter &p, int which); // scratch row 0..3, width + 2 bytes
    static uint32_t *row32(Painter &p);
    static float     opacity(const Painter &p) { return p._s.opacity; }
    static float     scale(const Painter &p) { return p._scale; }
    static PointF    origin(const Painter &p) { return {p._s.tx * p._scale, p._s.ty * p._scale}; }
};

} // namespace gfx
