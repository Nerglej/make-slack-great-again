// gfx — the CPU painter every pixel of the app goes through, plus images.
//
// Contract shared by text/ (draws glyphs through blitMask/blitColor), ui/ and
// app/. Additive changes are fine; changing a signature needs the lead.
//
// Pixels: premultiplied ARGB32 in native endianness (0xAARRGGBB as uint32_t),
// the same format as plat::Canvas. Colors in the API are *straight* (not
// premultiplied) 0xAARRGGBB — the painter premultiplies.
//
// Units: the Painter works in logical pixels and applies `scale` (plat's
// physical/logical ratio) itself, so callers never think about DPI. Glyphs and
// other pre-rasterised masks are placed in physical pixels (blitMask/blitColor)
// because they were rasterised at physical size.
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace gfx {

using Color = uint32_t; // straight 0xAARRGGBB

constexpr Color rgb(uint32_t rgb) {
    return 0xff000000u | rgb;
}
constexpr Color rgba(uint32_t rgb, uint8_t a) {
    return (uint32_t(a) << 24) | (rgb & 0xffffff);
}
Color withAlpha(Color c, float opacity); // multiplies the alpha

struct PointF {
    float x = 0, y = 0;
};
struct RectF {
    float x = 0, y = 0, w = 0, h = 0;
    float right() const { return x + w; }
    float bottom() const { return y + h; }
    bool  contains(PointF p) const { return p.x >= x && p.y >= y && p.x < x + w && p.y < y + h; }
};

// A view onto pixels owned elsewhere (a plat::Canvas, a Bitmap, a cache page).
struct BitmapView {
    uint32_t *pixels = nullptr; // premultiplied ARGB32
    int       width = 0, height = 0;
    int       stride = 0; // in pixels
};

class Bitmap {
public:
    Bitmap() = default;
    Bitmap(int w, int h); // cleared to transparent
    int             width() const { return _w; }
    int             height() const { return _h; }
    bool            empty() const { return _w <= 0 || _h <= 0; }
    uint32_t       *pixels() { return _px.data(); }
    const uint32_t *pixels() const { return _px.data(); }
    BitmapView      view() { return {_px.data(), _w, _h, _w}; }
    BitmapView      view() const { return {const_cast<uint32_t *>(_px.data()), _w, _h, _w}; }

private:
    int                   _w = 0, _h = 0;
    std::vector<uint32_t> _px;
};

// 8-bit coverage (a glyph, an anti-aliased shape).
struct Mask8 {
    const uint8_t *data  = nullptr;
    int            width = 0, height = 0, stride = 0; // stride in bytes
};

// Vector path in logical units (icons, custom shapes). Non-zero fill rule.
class Path {
public:
    void moveTo(float x, float y);
    void lineTo(float x, float y);
    void quadTo(float cx, float cy, float x, float y);
    void cubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y);
    void close();
    // A full circle as four cubics (clockwise in y-down coordinates).
    void addCircle(float cx, float cy, float r);
    bool empty() const { return _cmds.empty(); }

    // Flattened representation the rasterizer consumes.
    enum Cmd : uint8_t { Move, Line, Quad, Cubic, Close };
    const std::vector<uint8_t> &cmds() const { return _cmds; }
    const std::vector<float>   &pts() const { return _pts; }

private:
    std::vector<uint8_t> _cmds;
    std::vector<float>   _pts;
};

enum class Sampling : uint8_t { Nearest, Bilinear, Smooth /* area-average when shrinking */ };

enum class FillRule : uint8_t { NonZero, EvenOdd };

// A 2D affine transform in SVG order: x' = a·x + c·y + e, y' = b·x + d·y + f.
struct Affine {
    float a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
};

// Linear or radial gradient paint. Colours are interpolated premultiplied
// (like browsers); outside [0, 1] the spread method applies.
struct GradientStop {
    float offset = 0; // 0..1, ascending
    Color color  = 0; // straight
};
struct Gradient {
    enum class Kind : uint8_t { Linear, Radial };
    enum class Spread : uint8_t { Pad, Reflect, Repeat };
    Kind                      kind   = Kind::Linear;
    Spread                    spread = Spread::Pad;
    PointF                    a, b; // Linear: t = 0 at a, 1 at b. Radial: centre a (no focal point)
    float                     radius = 0; // Radial: t = 1 at this distance from a
    // Maps the painter's current (translated, logical) coordinates into the
    // space a/b/radius are given in; identity = the same space.
    Affine                    toGradient;
    std::vector<GradientStop> stops;
};

// Stroke geometry for strokePath(Path, Stroke, …). Dash lengths are in the
// same units as the path.
struct Stroke {
    enum class Cap : uint8_t { Butt, Round, Square };
    enum class Join : uint8_t { Miter, Round, Bevel };
    float        width      = 1;
    Cap          cap        = Cap::Round;
    Join         join       = Join::Round;
    float        miterLimit = 4;
    const float *dashes     = nullptr; // on/off lengths; null or count 0 = solid
    int          dashCount  = 0;
    float        dashOffset = 0;
};

// Painters are cheap (make one per frame). dropShadow and drawIcon keep
// small process-wide caches (blurred masks, icon coverage), so those two are
// for the UI thread only; everything else may run on any thread with its own
// Painter.
class Painter {
public:
    // Paints into `target` (physical pixels); logical units are multiplied by scale.
    Painter(BitmapView target, float scale);

    float scale() const { return _scale; }
    int   physicalWidth() const { return _target.width; }
    int   physicalHeight() const { return _target.height; }

    // State stack: transform (translation only) + clip + opacity.
    void  save();
    void  restore();
    void  translate(float dx, float dy);
    void  setOpacity(float o); // multiplies everything drawn until restore()
    // Intersects the current clip. Rounded clips are anti-aliased (avatars,
    // image cards).
    void  clipRect(RectF r);
    void  clipRoundRect(RectF r, float radius);
    RectF clipBounds() const; // logical, in current coordinates

    // Rects whose edges land on whole physical pixels are filled crisp;
    // fractional edges get exact anti-aliased coverage.
    void fillRect(RectF r, Color c);
    void fillRoundRect(RectF r, float radius, Color c);
    // Strokes lie *inside* the shape's outline, like a CSS border: a 1 px
    // stroke of `r` never paints outside `r`, and strokeCircle's outer edge is
    // `radius`.
    void strokeRoundRect(RectF r, float radius, float width, Color c);
    void fillCircle(PointF center, float radius, Color c);
    void strokeCircle(PointF center, float radius, float width, Color c);
    // Butt caps; horizontal/vertical lines go through fillRect (crisp when aligned).
    void drawLine(PointF a, PointF b, float width, Color c);
    void fillPath(const Path &p, Color c);
    void fillPath(const Path &p, Color c, FillRule rule);
    void fillPath(const Path &p, const Gradient &g, FillRule rule = FillRule::NonZero);
    // Stroke centred on the path, round caps and joins (icons, custom shapes).
    void strokePath(const Path &p, float width, Color c);
    void strokePath(const Path &p, const Stroke &s, Color c);
    void strokePath(const Path &p, const Stroke &s, const Gradient &g);
    // Linear gradient from `c0` at `a` to `c1` at `b` (used sparingly: shadows, fades).
    void fillRectGradient(RectF r, PointF a, Color c0, PointF b, Color c1);
    // Soft drop shadow of the rounded box `r` (popups, menus); `blur` is the
    // CSS blur radius, `offset` moves the shadow (not the box). Like CSS
    // box-shadow it is not painted under the box itself — draw the box on top.
    void dropShadow(RectF r, float radius, float blur, Color c);
    void dropShadow(RectF r, float radius, float blur, PointF offset, Color c);

    // Scaled image into `dst` (logical), snapped to whole physical pixels.
    // Smooth shrinking area-averages the whole source on every call: for an
    // image drawn every frame (avatars), draw a cached resize() result.
    void drawBitmap(
        const BitmapView &src, RectF dst, Sampling s = Sampling::Smooth, float opacity = 1.0f
    );

    // Pre-rasterised content at *physical* integer positions relative to the
    // current (translated) origin converted to physical: text uses these.
    void   blitMask(const Mask8 &m, int physX, int physY, Color c);
    void   blitColor(const BitmapView &src, int physX, int physY, float opacity = 1.0f);
    // Current logical translation → physical pixel origin (for text placement).
    PointF toPhysical(PointF logical) const;

private:
    friend struct PainterImpl; // the raster/blend code in the .cpp files
    struct State {
        float tx = 0, ty = 0;
        int   clipX0 = 0, clipY0 = 0, clipX1 = 0, clipY1 = 0; // physical, rect part
        float opacity   = 1.0f;
        int   roundClip = -1; // index into _roundClips, -1 = none
    };
    BitmapView         _target;
    float              _scale;
    State              _s;
    std::vector<State> _stack;
    struct RoundClip {
        float x, y, w, h, r; // physical
        int   parent;
    };
    std::vector<RoundClip> _roundClips;
    // Per-painter scratch rows (coverage, clip coverage, source pixels), so
    // painters on different threads never share buffers.
    std::vector<uint8_t>   _buf8;
    std::vector<uint32_t>  _buf32;
};

// ── Images ──────────────────────────────────────────────────────────────────
// Decode PNG / JPEG / GIF (first frame) / WebP (when compiled in) into a
// premultiplied Bitmap. False on unsupported or corrupt data.
bool decodeImage(std::string_view bytes, Bitmap *out);

struct AnimFrame {
    Bitmap frame; // full canvas-sized frame, already composited
    int    delayMs = 100;
};
// Animated GIF → composited frames; a still image yields one frame.
bool decodeAnimation(std::string_view bytes, std::vector<AnimFrame> *out);

// High-quality resize (area-average down, bilinear up) — thumbnails, avatars.
Bitmap resize(const BitmapView &src, int width, int height);

// Decoders refuse images larger than this many pixels (decompression bombs).
constexpr int64_t kMaxImagePixels = 64ll << 20;

// ── SVG ─────────────────────────────────────────────────────────────────────
// Runtime SVG (custom workspace icons, SVG attachments), rendered by gfx's
// own basic renderer on every OS (identical pixels everywhere): shapes, paths, strokes
// (caps, joins, dashes), transforms, opacity, linear/radial gradients,
// clipPath, use. Text, filters, masks, patterns, markers and CSS <style>
// sheets are skipped (the rest still renders). Input is capped at 4 MB.
//
// Intrinsic size in CSS px from width/height, else the viewBox. False when
// the data is not an SVG or declares neither.
bool svgSize(std::string_view svg, float *w, float *h);
// Renders into a width×height premultiplied bitmap, transparent background,
// the viewBox fitted per preserveAspectRatio (default xMidYMid meet).
bool renderSvg(std::string_view svg, int width, int height, Bitmap *out);

// ── Icons ───────────────────────────────────────────────────────────────────
// Icons are compiled from ../gfx/**/*.svg by tools/icons.py into compact path
// data (no SVG renderer in the binary). Draw one into `r` (logical), tinted.
// The viewBox is fitted into `r` (centred, aspect kept) with its origin
// snapped to a whole physical pixel so strokes stay crisp. Parts drawn in
// currentColor take `tint`; the few fixed-colour parts (the Slack mark, the
// grey spinner arrows) keep their colour and only take tint's alpha.
enum class Icon : uint16_t; // generated: gfx/icons_generated.h (also kIconCount)
void drawIcon(Painter &p, Icon icon, RectF r, Color tint);
// The same turned clockwise by `degrees` about r's centre (a spinner).
void drawIconRotated(Painter &p, Icon icon, RectF r, Color tint, float degrees);

} // namespace gfx
