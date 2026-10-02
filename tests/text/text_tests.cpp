// text tests: `text_tests <case>` (ctest runs each case separately), or no
// argument for all of them. Uses the machine's installed fonts; cases that
// need a script the machine has no font for say so and pass.
#include "text/fonts.h"
#include "text/glyph_cache.h"
#include "text/text.h"

#include <hb.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>

using namespace text;
using text::fonts::FontKey;

namespace {

int g_fail = 0;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                             \
            ++g_fail;                                                                              \
        }                                                                                          \
    } while (0)

std::unique_ptr<Layout>
lay(const std::string &s, float maxW = 1e9f, float scale = 1, const Style &st = Style{}) {
    AttributedText t;
    t.append(s, st);
    LayoutOptions o;
    o.maxWidth = maxW;
    return Layout::build(t, o, scale);
}

void wrap() {
    const std::string s = "The quick brown fox jumps over the lazy dog and keeps running far away";
    for (float scale : {1.0f, 1.5f, 2.0f}) {
        auto one = lay(s, 1e9f, scale);
        CHECK(one->lineCount() == 1);
        CHECK(std::fabs(one->width() - measure(s, Style{}, scale)) < 0.01f);
        for (float w : {60.f, 120.f, 200.f, 333.f}) {
            auto l = lay(s, w, scale);
            CHECK(l->lineCount() > 1);
            CHECK(l->width() <= w + 0.01f);
            // Every line starts at a word start (after a space), never mid-word.
            for (int i = 1; i < l->lineCount(); ++i) {
                const uint32_t off = l->hitTest({0, l->baseline(i) - 2}).offset;
                CHECK(off > 0 && s[off - 1] == ' ');
            }
            CHECK(l->height() > one->height() * (l->lineCount() - 0.5f));
        }
    }
    // Newlines are mandatory breaks; a trailing one opens an empty line.
    CHECK(lay("a\nb\n\nc")->lineCount() == 4);
    CHECK(lay("a\n")->lineCount() == 2);
    // Long words / URLs break anywhere when they overflow.
    auto url =
        lay("see https://example.com/a/very/long/path/that/does/not/fit/anywhere/at/all", 150);
    CHECK(url->lineCount() >= 3);
    CHECK(url->width() <= 150.01f);
    // CJK breaks between ideographs.
    auto cjk = lay("日本語のテキストはスペースなしで折り返されます", 80);
    CHECK(cjk->lineCount() > 1);
    CHECK(cjk->width() <= 80.01f);
}

void ellipsis() {
    AttributedText t;
    t.append("A fairly long sentence that certainly does not fit on one short line", Style{});
    LayoutOptions o;
    o.maxWidth = 150;
    o.maxLines = 1;
    o.ellipsis = true;
    auto l     = Layout::build(t, o, 1);
    CHECK(l->lineCount() == 1);
    CHECK(l->truncated());
    CHECK(l->width() <= 150.01f);
    std::printf("  ellipsis width %.1f\n", l->width());
    CHECK(l->width() > 100);
    o.maxLines = 2;
    auto l2    = Layout::build(t, o, 1.5f);
    CHECK(l2->lineCount() == 2);
    CHECK(l2->truncated());
    CHECK(l2->width() <= 150.01f);
    o.maxLines = 5;
    auto l5    = Layout::build(t, o, 1);
    CHECK(!l5->truncated());
    // Fits: nothing is cut.
    AttributedText s;
    s.append("short", Style{});
    o.maxLines = 1;
    CHECK(!Layout::build(s, o, 1)->truncated());
}

void emojiCaret() {
    // a | family (ZWJ) | b | flag | c | thumbs up + skin tone | d | e + combining acute | f
    const std::string fam   = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91"
                              "\xA7\xE2\x80\x8D\xF0\x9F\x91\xA6";
    const std::string flag  = "\xF0\x9F\x87\xB8\xF0\x9F\x87\xAA";
    const std::string thumb = "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD";
    const std::string eacute = "e\xCC\x81";
    const std::string heart  = "\xE2\x9D\xA4\xEF\xB8\x8F"; // ❤️ (VS16)
    const std::string s = "a" + fam + "b" + flag + "c" + thumb + "d" + eacute + "f" + heart + "g";
    auto              l = lay(s);
    std::vector<uint32_t> stops = {0};
    uint32_t              off   = 0;
    for (int i = 0; i < 40 && off < s.size(); ++i) {
        off = l->moveCaret(off, 1, 0);
        stops.push_back(off);
    }
    const std::vector<uint32_t> want = {
        0,
        1,
        uint32_t(1 + fam.size()),
        uint32_t(2 + fam.size()),
        uint32_t(2 + fam.size() + flag.size()),
        uint32_t(3 + fam.size() + flag.size()),
        uint32_t(3 + fam.size() + flag.size() + thumb.size()),
        uint32_t(4 + fam.size() + flag.size() + thumb.size()),
        uint32_t(4 + fam.size() + flag.size() + thumb.size() + eacute.size()),
        uint32_t(5 + fam.size() + flag.size() + thumb.size() + eacute.size()),
        uint32_t(5 + fam.size() + flag.size() + thumb.size() + eacute.size() + heart.size()),
        uint32_t(s.size())
    };
    CHECK(stops == want);
    // And back again.
    std::vector<uint32_t> back;
    off = uint32_t(s.size());
    back.push_back(off);
    while (off > 0)
        back.push_back(off = l->moveCaret(off, -1, 0));
    std::vector<uint32_t> rev(want.rbegin(), want.rend());
    CHECK(back == rev);
    // The family draws as one glyph-sized cluster (a ZWJ ligature), not four.
    const float w1 = lay("a" + fam + "b")->width(), w0 = lay("ab")->width();
    const float oneEmoji = lay("\xF0\x9F\x91\xA8")->width();
    if (fonts::emoji() != fonts::kNoFont)
        CHECK(w1 - w0 < oneEmoji * 1.5f);
    else
        std::printf("  (no colour emoji font installed; width check skipped)\n");
}

void rtl() {
    // "abc שלום def": the Hebrew word reads right-to-left inside an LTR line.
    const std::string heb   = "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D"; // ש ל ו ם
    const std::string s     = "abc " + heb + " def";
    auto              l     = lay(s);
    const float       xShin = l->caretRect(4).x, xLamed = l->caretRect(6).x;
    const float       xMem = l->caretRect(10).x;
    CHECK(xShin > xLamed);
    CHECK(xLamed > xMem);
    CHECK(l->caretRect(0).x < xMem);   // LTR text before stays left
    CHECK(l->caretRect(13).x > xShin); // "def" stays right
    // A pure Arabic paragraph is RTL: its first character sits at the right.
    const std::string ar = "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 \xD8\xA8\xD9\x83"; // مرحبا بك
    auto              a  = lay(ar);
    CHECK(a->caretRect(0).x > a->caretRect(uint32_t(ar.size())).x);
    CHECK(a->caretRect(0).x > a->width() * 0.8f);
    // Numbers inside RTL keep their LTR order: "שלום 123".
    auto           n  = lay(heb + " 123");
    const uint32_t d1 = uint32_t(heb.size() + 1);
    CHECK(n->caretRect(d1).x < n->caretRect(d1 + 2).x);
    // Hit testing inside the Hebrew word maps back to it.
    const HitResult h = l->hitTest({(xShin + xMem) / 2, l->baseline(0) - 3});
    CHECK(h.offset >= 4 && h.offset <= 12);
}

void cjkFallback() {
    Style mono;
    mono.mono = true;
    std::printf(
        "  ui=%s mono=%s emoji=%s faces=%zu\n",
        fonts::familyName(fonts::primary(Style{})).c_str(),
        fonts::familyName(fonts::primary(mono)).c_str(),
        fonts::familyName(fonts::emoji()).c_str(),
        fonts::faceCount()
    );
    const FontKey f = fonts::fallback(0x65E5 /* 日 */, Style{});
    if (f == fonts::kNoFont) {
        std::printf("  (no CJK font installed; skipped)\n");
        return;
    }
    const std::string fam = fonts::familyName(f);
    std::printf("  日 → %s\n", fam.c_str());
    CHECK(fonts::hasGlyph(f, 0x65E5) && fonts::hasGlyph(f, 0x8A9E));
    CHECK(
        fam.find("cjk") != std::string::npos || fam.find("droid") != std::string::npos ||
        fam.find("han") != std::string::npos || fam.find("gothic") != std::string::npos
    );
    CHECK(fonts::hasGlyph(fonts::fallback(0x0634, Style{}), 0x0634)); // Arabic
    CHECK(fonts::hasGlyph(fonts::fallback(0x05D0, Style{}), 0x05D0)); // Hebrew
    const FontKey cyr = fonts::primary(Style{});
    CHECK(fonts::hasGlyph(cyr, 0x0416)); // Cyrillic in the UI font
    // Laid out: three ideographs are about three ems wide (no tofu boxes).
    auto l = lay("日本語", 1e9f, 1);
    CHECK(l->width() > 15 * 2.5f && l->width() < 15 * 3.5f);
}

void hitRoundtrip() {
    AttributedText t;
    Style          b;
    b.weight = Weight::Bold;
    t.append("Hello ", Style{});
    t.append("bold world", b);
    t.append(" and a longer tail that wraps over several lines, with ", Style{});
    t.append("\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D", Style{});
    t.append(" and emoji \xF0\x9F\x98\x80 done.", Style{});
    for (float scale : {1.0f, 1.25f, 2.0f}) {
        LayoutOptions o;
        o.maxWidth = 160;
        auto l     = Layout::build(t, o, scale);
        int  bad   = 0;
        for (uint32_t off = 0; off <= t.text.size(); off = l->moveCaret(off, 1, 0)) {
            const gfx::RectF c = l->caretRect(off);
            const HitResult  h = l->hitTest({c.x + 0.01f, c.y + c.h / 2});
            // A caret at a line end and the next line's start share a spot
            // only through the line break; every other offset maps back.
            if (h.offset != off) {
                const gfx::RectF c2 = l->caretRect(h.offset);
                if (std::fabs(c2.x - c.x) > 0.6f || std::fabs(c2.y - c.y) > 0.5f) {
                    std::printf(
                        "  scale %.2f: off %u → caret (%.1f,%.1f) → hit %u\n",
                        scale,
                        off,
                        c.x,
                        c.y,
                        h.offset
                    );
                    ++bad;
                }
            }
            if (off == t.text.size())
                break;
        }
        CHECK(bad == 0);
    }
    // Links report their id only over their glyphs.
    AttributedText lt;
    Style          link;
    link.linkId = 42;
    lt.append("go to ", Style{});
    lt.append("example.com", link);
    auto       l = Layout::build(lt, LayoutOptions{}, 1);
    const auto c = l->caretRect(8);
    CHECK(l->hitTest({c.x, c.y + c.h / 2}).linkId == 42);
    CHECK(l->hitTest({1, c.y + c.h / 2}).linkId == 0);
    CHECK(!l->hitTest({l->width() + 20, c.y + c.h / 2}).inside);
}

void inlineBox() {
    AttributedText t;
    t.append("hi ", Style{});
    Style box;
    box.inlineBoxId = 7;
    box.boxWidth    = 22;
    box.boxHeight   = 22;
    t.append(":custom:", box);
    t.append(" there", Style{});
    auto l = Layout::build(t, LayoutOptions{}, 1);
    CHECK(l->boxes().size() == 1);
    if (l->boxes().empty())
        return;
    const InlineBox &b = l->boxes()[0];
    std::printf(
        "  box %.1f,%.1f %.1fx%.1f; hi=%.1f width %.1f height %.1f baseline %.1f\n",
        b.rect.x,
        b.rect.y,
        b.rect.w,
        b.rect.h,
        measure("hi ", Style{}, 1),
        l->width(),
        l->height(),
        l->baseline(0)
    );
    CHECK(b.id == 7);
    CHECK(std::fabs(b.rect.x - measure("hi ", Style{}, 1)) < 0.6f);
    CHECK(b.rect.w == 22 && b.rect.h == 22);
    CHECK(b.rect.y >= 0 && b.rect.bottom() <= l->height() + 0.01f);
    // Roughly centred on the text: the box straddles the baseline.
    CHECK(b.rect.y < l->baseline(0) && b.rect.bottom() > l->baseline(0));
    CHECK(
        std::fabs(l->width() - (measure("hi ", Style{}, 1) + 22 + measure(" there", Style{}, 1))) <
        0.6f
    );
    // Atomic for the caret: one step over it.
    CHECK(l->moveCaret(3, 1, 0) == 3 + 8);
    CHECK(l->moveCaret(11, -1, 0) == 3);
    // A box too wide for the rest of the line wraps as a unit.
    LayoutOptions o;
    o.maxWidth = measure("hi ", Style{}, 1) + 10;
    auto w     = Layout::build(t, o, 1);
    CHECK(w->lineCount() >= 2);
    CHECK(w->boxes().size() == 1 && w->boxes()[0].rect.x < 0.01f);
}

void styleBoundary() {
    const std::string s   = "Hello World, AV Ty To";
    const float       w   = lay(s)->width();
    int               bad = 0;
    for (size_t cut = 1; cut < s.size(); ++cut) {
        AttributedText t;
        Style          a, b;
        b.color     = gfx::rgb(0xff0000);
        b.underline = true;
        b.linkId    = 3;
        t.append(s.substr(0, cut), a);
        t.append(s.substr(cut), b);
        const float w2 = Layout::build(t, LayoutOptions{}, 1)->width();
        if (std::fabs(w2 - w) > 0.001f)
            ++bad, std::printf("  cut %zu: %.3f vs %.3f\n", cut, w2, w);
    }
    CHECK(bad == 0);
}

void wordSelect() {
    const std::string s = "hello, world foo_bar  x";
    auto              l = lay(s);
    CHECK(l->wordStart(2) == 0 && l->wordEnd(2) == 5);
    CHECK(l->wordStart(5) == 5 && l->wordEnd(5) == 6); // the comma alone
    CHECK(l->wordStart(9) == 7 && l->wordEnd(9) == 12);
    CHECK(l->wordStart(15) == 13 && l->wordEnd(15) == 20);
    CHECK(l->wordStart(21) == 20 && l->wordEnd(21) == 22); // the double space
    CHECK(l->wordEnd(uint32_t(s.size())) == s.size());
}

// Layout decodes with base's strict utf8::decode: an overlong '/', a UTF-16
// surrogate and a value above U+10FFFF are U+FFFD per byte (as HarfBuzz sees
// them), never a code point the caret skips over in one step.
void malformedUtf8() {
    const std::string     s = "a\xC0\xAF"
                              "b\xED\xA0\x80"
                              "c\xF4\x90\x80\x80"
                              "d";
    auto                  l = lay(s);
    std::vector<uint32_t> stops;
    for (uint32_t off = 0; off < s.size();)
        stops.push_back(off = l->moveCaret(off, 1, 0));
    std::vector<uint32_t> want;
    for (uint32_t i = 1; i <= s.size(); ++i)
        want.push_back(i);
    CHECK(stops == want);
    // A double-click on a malformed byte selects just that byte.
    CHECK(l->wordStart(1) == 1 && l->wordEnd(1) == 2);
}

void glyphCache() {
    gfx::Bitmap  bmp(400, 200);
    gfx::Painter p(bmp.view(), 1.5f);
    p.fillRect({0, 0, 400, 200}, gfx::rgb(0xffffff));
    cache::setBudget(256 * 1024);
    for (int size = 8; size < 60; size += 3) {
        Style st;
        st.size = float(size);
        AttributedText t;
        t.append("The quick brown fox 0123456789 ÅÄÖ ЖЩ", st);
        Layout::build(t, LayoutOptions{}, 1.5f)->paint(p, {0, 0});
    }
    std::printf("  cache: %zu bytes, %zu glyphs\n", cache::bytesUsed(), cache::glyphCount());
    CHECK(cache::bytesUsed() <= 256 * 1024 + 256 * 1024);
    // Something dark was drawn.
    int dark = 0;
    for (int i = 0; i < 400 * 200; ++i)
        dark += (bmp.pixels()[i] & 0xff) < 128;
    CHECK(dark > 200);
    cache::setBudget(8u << 20);
}

// inkBounds is the box of what paint() draws from a whole-pixel origin: the
// side bearings and the blank edges of the glyph masks are not in it.
// inkLean: a "1"'s ink mass sits right of its ink box's centre (the flag is
// light), a symmetric "0" has none, and it is per glyph: "11" leans like "1",
// and a "+" after "99" does not pull the string's lean towards it.
void inkLean() {
    CHECK(lay("")->inkLean() == 0);
    for (float scale : {1.0f, 1.5f, 2.0f}) {
        Style st;
        st.size         = 11.7f;
        st.weight       = Weight::Bold;
        const float px  = 1 / scale;
        const float one = lay("1", 1e9f, scale, st)->inkLean();
        CHECK(one > px / 2);
        CHECK(std::fabs(lay("0", 1e9f, scale, st)->inkLean()) < px / 2);
        CHECK(std::fabs(lay("11", 1e9f, scale, st)->inkLean() - one) < px / 2);
        CHECK(std::fabs(lay("99+", 1e9f, scale, st)->inkLean()) < px);
    }
}

void inkBounds() {
    CHECK(lay("")->inkBounds().w == 0);
    CHECK(lay(" ")->inkBounds().w == 0);
    for (float scale : {1.0f, 1.5f, 2.0f}) {
        for (const char *s : {"1", "5", "99+", "Wg"}) {
            Style st;
            st.size        = 10.1f;
            st.weight      = Weight::Bold;
            st.color       = gfx::rgb(0x000000);
            auto       l   = lay(s, 1e9f, scale, st);
            const auto ink = l->inkBounds();
            CHECK(ink.w > 0 && ink.h > 0);
            CHECK(ink.w <= l->width() + 1 / scale); // the ink, not the advance
            CHECK(ink.y > 0 && ink.bottom() <= l->height());
            gfx::Bitmap  bmp(64, 64);
            gfx::Painter p(bmp.view(), scale);
            p.fillRect({0, 0, 64, 64}, gfx::rgb(0xffffff));
            l->paint(p, {4, 4}); // whole device pixels at every scale here
            // Every clearly dark pixel is inside, and each edge touches ink.
            const int x0     = int(std::lround((4 + ink.x) * scale)),
                      x1     = x0 + int(std::lround(ink.w * scale));
            const int y0     = int(std::lround((4 + ink.y) * scale)),
                      y1     = y0 + int(std::lround(ink.h * scale));
            bool      inside = true, l0 = false, r0 = false, t0 = false, b0 = false;
            for (int y = 0; y < 64; ++y)
                for (int x = 0; x < 64; ++x) {
                    const int v = int(bmp.pixels()[y * 64 + x] & 0xff);
                    if (v < 192 && (x < x0 || x >= x1 || y < y0 || y >= y1))
                        inside = false;
                    if (v < 250) {
                        l0 = l0 || x == x0, r0 = r0 || x == x1 - 1;
                        t0 = t0 || y == y0, b0 = b0 || y == y1 - 1;
                    }
                }
            CHECK(inside && l0 && r0 && t0 && b0);
        }
    }
}

void perf() {
    AttributedText t;
    Style          bold, code, link, it;
    bold.weight     = Weight::Bold;
    code.mono       = true;
    code.background = gfx::rgb(0xf0f0f0);
    link.color      = gfx::rgb(0x1264a3);
    link.linkId     = 1;
    it.italic       = true;
    t.append("Hey ", Style{});
    t.append("@robin", link);
    t.append(", I pushed the fix for ", Style{});
    t.append("layout.cpp", code);
    t.append(" — the wrapping bug was in the ", Style{});
    t.append("greedy line filler", bold);
    t.append(" \xF0\x9F\x8E\x89 and it should now handle long URLs like ", Style{});
    t.append("https://example.com/some/path?q=1", link);
    t.append(" properly. Let me know if ", Style{});
    t.append("anything", it);
    t.append(" looks off \xF0\x9F\x91\x8D", Style{});
    LayoutOptions o;
    o.maxWidth = 520;
    std::printf(
        "  message: %zu bytes, %d lines\n", t.text.size(), Layout::build(t, o, 1)->lineCount()
    );
    gfx::Bitmap  bmp(1100, 300);
    gfx::Painter p(bmp.view(), 2);
    for (int i = 0; i < 50; ++i) // warm caches (fonts, fallback, glyphs)
        Layout::build(t, o, 2)->paint(p, {0, 0});
    double best = 1e9;
    for (int round = 0; round < 5; ++round) {
        const int  n  = 2000;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; ++i)
            Layout::build(t, o, 2);
        const double us =
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0)
                .count() /
            n;
        best = std::min(best, us);
    }
    auto       l  = Layout::build(t, o, 2);
    const int  np = 2000;
    const auto t1 = std::chrono::steady_clock::now();
    for (int i = 0; i < np; ++i)
        l->paint(p, {0, 0});
    const double paintUs =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t1).count() /
        np;
    std::printf("  layout %.1f us, paint %.1f us (scale 2)\n", best, paintUs);
    CHECK(best < 200);
}

// The colour emoji font renders in colour: CBDT (Noto Color Emoji, Linux),
// sbix (Apple Color Emoji, which also carries placeholder outlines) and COLR
// (Segoe UI Emoji, Windows) all come out as premultiplied ARGB.
void emojiColor() {
    const FontKey k = fonts::emoji();
    if (k == fonts::kNoFont) {
        std::printf("  no colour emoji font installed; skipped\n");
        return;
    }
    std::printf("  emoji font: %s\n", fonts::familyName(k).c_str());
    hb_codepoint_t gid = 0;
    CHECK(hb_font_get_nominal_glyph(fonts::hbFont(k), 0x1F600, &gid)); // 😀
    // 30 and 36 fall between Apple Color Emoji's strikes (20 26 32 40 …).
    for (uint32_t px : {20u, 30u, 36u, 64u}) {
        fonts::Raster r;
        CHECK(fonts::rasterize(k, gid, px * 64, 0, &r));
        CHECK(r.color && r.argb);
        std::printf("  %upx: %dx%d\n", px, r.w, r.h);
        // Sized for the requested pixel size, whatever strike it came from.
        CHECK(r.w >= int(px) * 3 / 4 && r.w <= int(px) * 3 / 2);
        CHECK(r.h >= int(px) * 3 / 4 && r.h <= int(px) * 3 / 2);
        bool tinted = false; // a pixel that is neither grey nor transparent
        for (int y = 0; r.argb && y < r.h; ++y)
            for (int x = 0; x < r.w; ++x) {
                const uint32_t c  = r.argb[size_t(y) * r.pitch + x];
                const int      cr = (c >> 16) & 255, cg = (c >> 8) & 255, cb = c & 255;
                tinted = tinted ||
                         ((c >> 24) > 128 && (std::abs(cr - cg) > 40 || std::abs(cg - cb) > 40));
            }
        CHECK(tinted);
    }
}

// Emoji reach the canvas in colour through the whole pipeline (itemize,
// shape, cache, paint), in the sizes and sequences the app draws: the status
// presets, a VS16 heart, a ZWJ sequence, a flag, a Unicode 14 face.
// MSGA_TEXT_DUMP=<file.ppm> writes the canvas out for a look.
void emojiPaint() {
    if (fonts::emoji() == fonts::kNoFont) {
        std::printf("  no colour emoji font installed; skipped\n");
        return;
    }
    const char *const items[] = {
        "\xF0\x9F\x93\x85",                             // 📅 calendar
        "\xF0\x9F\x9A\x8C",                             // 🚌 bus
        "\xF0\x9F\xA4\x92",                             // 🤒 thermometer
        "\xF0\x9F\x8C\xB4",                             // 🌴 palm tree
        "\xF0\x9F\x8F\xA1",                             // 🏡 house with garden
        "\xE2\x9D\xA4\xEF\xB8\x8F",                     // ❤️ (VS16)
        "\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB", // 👩‍💻 (ZWJ)
        "\xF0\x9F\x87\xB8\xF0\x9F\x87\xAA",             // 🇸🇪
        "\xF0\x9F\xAB\xA0", // 🫠 melting face: not in Apple's 40/48/52 strikes
    };
    constexpr int   kItems = int(std::size(items));
    constexpr float kScale = 2, kCell = 40;
    gfx::Bitmap     canvas(int(kCell * kScale * kItems), int(kCell * kScale * 3));
    gfx::Painter    p(canvas.view(), kScale);
    p.fillRect({0, 0, kCell * kItems, kCell * 3}, gfx::rgb(0xffffff));
    int row = 0;
    for (float size : {13.f, 18.f, 22.f}) { // 26, 36, 44 physical px
        for (int i = 0; i < kItems; ++i) {
            Style st;
            st.size = size;
            auto l  = lay(items[i], 1e9f, kScale, st);
            l->paint(p, {kCell * float(i) + 4, kCell * float(row) + 4});
            int tinted = 0;
            for (int y = int(kCell * kScale * row); y < int(kCell * kScale * (row + 1)); ++y)
                for (int x = int(kCell * kScale * i); x < int(kCell * kScale * (i + 1)); ++x) {
                    const uint32_t c  = canvas.view().pixels[size_t(y) * canvas.view().stride + x];
                    const int      cr = (c >> 16) & 255, cg = (c >> 8) & 255, cb = c & 255;
                    tinted += std::abs(cr - cg) > 40 || std::abs(cg - cb) > 40;
                }
            if (tinted < int(size * size) / 4)
                std::printf("  %gpx item %d: %d coloured pixels\n", size, i, tinted);
            CHECK(tinted >= int(size * size) / 4);
        }
        ++row;
    }
    if (const char *dump = std::getenv("MSGA_TEXT_DUMP")) {
        if (FILE *f = std::fopen(dump, "wb")) {
            const gfx::BitmapView v = canvas.view();
            std::fprintf(f, "P6\n%d %d\n255\n", v.width, v.height);
            for (int y = 0; y < v.height; ++y)
                for (int x = 0; x < v.width; ++x) {
                    const uint32_t c      = v.pixels[size_t(y) * v.stride + x];
                    const uint8_t  rgb[3] = {uint8_t(c >> 16), uint8_t(c >> 8), uint8_t(c)};
                    std::fwrite(rgb, 1, 3, f);
                }
            std::fclose(f);
        }
    }
}

struct Case {
    const char *name;
    void (*fn)();
};
constexpr Case kCases[] = {
    {"wrap", wrap},
    {"ellipsis", ellipsis},
    {"emoji_caret", emojiCaret},
    {"emoji_color", emojiColor},
    {"emoji_paint", emojiPaint},
    {"rtl", rtl},
    {"cjk_fallback", cjkFallback},
    {"hit_roundtrip", hitRoundtrip},
    {"inline_box", inlineBox},
    {"style_boundary", styleBoundary},
    {"word_select", wordSelect},
    {"malformed_utf8", malformedUtf8},
    {"glyph_cache", glyphCache},
    {"ink_bounds", inkBounds},
    {"ink_lean", inkLean},
    {"perf", perf},
};

} // namespace

int main(int argc, char **argv) {
    std::string err;
    if (!text::init(&err)) {
        std::printf("text::init failed: %s\n", err.c_str());
        return 1;
    }
    int ran = 0;
    for (auto &c : kCases) {
        if (argc > 1 && std::strcmp(argv[1], c.name) != 0)
            continue;
        std::printf("%s\n", c.name);
        c.fn();
        ++ran;
    }
    if (!ran) {
        std::printf("unknown case\n");
        return 2;
    }
    std::printf(g_fail ? "%d failure(s)\n" : "ok\n", g_fail);
    return g_fail ? 1 : 0;
}
