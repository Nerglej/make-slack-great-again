#include "screens/shell/avatars.h"

#include "app/screens/common/avatar_initial.h"
#include "app/screens/common/remote_images.h"
#include "base/file.h"
#include "base/str.h"

#include <algorithm>
#include <cmath>

namespace shell {

namespace {

// An SVG (the Claude Code teammates' glyph tiles, custom workspace icons)
// rendered so its shorter side is px: the crop below squares it like a photo.
bool renderSvgCover(std::string_view bytes, int px, gfx::Bitmap *out) {
    float w = 0, h = 0;
    if (!gfx::svgSize(bytes, &w, &h) || w <= 0 || h <= 0)
        return false;
    const float k = float(px) / std::min(w, h);
    return gfx::renderSvg(
        bytes, std::max(px, int(std::lround(w * k))), std::max(px, int(std::lround(h * k))), out
    );
}

// The file at `path` centre-cropped to a square and resized to px×px into
// *out; false when it can't be read as an image.
bool decodeSquare(const std::string &path, int px, gfx::Bitmap *out) {
    std::string bytes;
    gfx::Bitmap b;
    if (!file::readAll(path, &bytes) ||
        !(gfx::decodeImage(bytes, &b) || renderSvgCover(bytes, px, &b)) || b.empty())
        return false;
    // Centre-crop to a square, then one high-quality resize: painting then
    // only copies pixels (drawBitmap would area-average on every frame).
    const int side = std::min(b.width(), b.height());
    if (b.width() != b.height()) {
        gfx::Bitmap sq(side, side);
        const int   ox = (b.width() - side) / 2, oy = (b.height() - side) / 2;
        for (int y = 0; y < side; ++y)
            std::copy_n(
                b.pixels() + size_t(y + oy) * b.width() + ox, side, sq.pixels() + size_t(y) * side
            );
        b = std::move(sq);
    }
    *out = side == px ? std::move(b) : gfx::resize(b.view(), px, px);
    return true;
}

} // namespace

Avatars::Avatars() : _alive(std::make_shared<char>(0)) {}
Avatars::~Avatars() = default;

std::shared_ptr<const gfx::Bitmap> Avatars::get(const std::string &path, int px) {
    if (path.empty() || px <= 0)
        return nullptr;
    const std::string key = str::concat({path, "@", str::number(px)});
    if (const auto it = _cache.find(key); it != _cache.end())
        return it->second;
    std::string file = path;
    if (screens::RemoteImages::isRemote(path)) {
        file = _remote ? _remote->cachedPath(path) : std::string();
        if (file.empty() && _remote) {
            // The placeholder the views hold until landed() fills it.
            auto blank = std::make_shared<gfx::Bitmap>();
            _cache.emplace(key, blank);
            std::weak_ptr<char> alive = _alive;
            _remote->fetch(path, [this, alive, key, px](const std::string &f) {
                if (!alive.expired())
                    landed(key, f, px);
            });
            return blank;
        }
    }
    auto out = std::make_shared<gfx::Bitmap>();
    if (file.empty() || !decodeSquare(file, px, out.get()))
        out.reset();
    _cache.emplace(key, out);
    return out;
}

void Avatars::landed(const std::string &key, const std::string &file, int px) {
    const auto it = _cache.find(key);
    if (it == _cache.end() || !it->second)
        return;
    if (file.empty() || !decodeSquare(file, px, it->second.get())) {
        // Not remembered as a failure: a later get() asks again (RemoteImages
        // keeps that from becoming a storm). Views keep the blank.
        _cache.erase(it);
        return;
    }
    if (_notifyQueued || !onLoaded)
        return;
    // One repaint for a burst of avatars landing together.
    _notifyQueued             = true;
    std::weak_ptr<char> alive = _alive;
    ui::app()->platform().post([this, alive] {
        if (alive.expired())
            return;
        _notifyQueued = false;
        if (onLoaded)
            onLoaded();
    });
}

plat::Image toPlatImage(const gfx::Bitmap &b) {
    plat::Image img;
    img.width  = b.width();
    img.height = b.height();
    img.pixels.assign(b.pixels(), b.pixels() + size_t(b.width()) * b.height());
    return img;
}

plat::Image roundedNotificationImage(const gfx::Bitmap &b) {
    plat::Image img;
    const int   side = std::min(b.width(), b.height());
    if (side <= 0)
        return img;
    img.width  = side;
    img.height = side;
    img.pixels.resize(size_t(side) * side);
    const int       x0 = (b.width() - side) / 2, y0 = (b.height() - side) / 2;
    const float     r  = side * 0.22f;
    const uint32_t *px = b.pixels();
    for (int y = 0; y < side; ++y)
        for (int x = 0; x < side; ++x) {
            // Coverage of the pixel by the rounded square: its centre's
            // distance past the corner arc, ±half a pixel.
            const float    cx = x + 0.5f, cy = y + 0.5f;
            const float    dx = std::max({r - cx, cx - (side - r), 0.f});
            const float    dy = std::max({r - cy, cy - (side - r), 0.f});
            const float    a  = std::clamp(r + 0.5f - std::sqrt(dx * dx + dy * dy), 0.f, 1.f);
            const uint32_t p  = px[size_t(y + y0) * b.width() + (x + x0)];
            uint32_t       q  = 0;
            if (a >= 1.f) {
                q = p;
            } else if (a > 0.f) { // premultiplied: every channel scales
                for (int sh = 0; sh < 32; sh += 8)
                    q |= uint32_t(float((p >> sh) & 0xff) * a + 0.5f) << sh;
            }
            img.pixels[size_t(y) * side + x] = q;
        }
    return img;
}

Avatar::Avatar() {
    setPaintOutset(3); // the dot's ring pokes past the corner
    setPlaceholder(ui::C::SidebarTextMuted);
}

void Avatar::setPresence(Presence p, ui::C ring, bool selected) {
    if (p == _presence && ring == _ring && selected == _selected)
        return;
    _presence = p;
    _ring     = ring;
    _selected = selected;
    update();
}

Avatar::~Avatar() = default;

void Avatar::setInitial(std::string_view name) {
    std::string i = screens::avatarInitial(name);
    if (i == _initial)
        return;
    _initial = std::move(i);
    _initialLayout.reset();
    update();
}

void Avatar::styleChanged() {
    _initialLayout.reset();
    Image::styleChanged();
}

void Avatar::paintOver(gfx::Painter &p) {
    if (!_initial.empty() && (!bitmap() || bitmap()->empty()))
        screens::paintInitial(p, *this, bounds(), 0, 0, _initial, _initialLayout);
    if (_presence == Presence::None)
        return;
    const ui::PointF c{width() - 3, height() - 3};
    if (_ring != ui::C::None)
        p.fillCircle(c, 5, ui::color(_ring));
    switch (_presence) {
    case Presence::Active:
        p.fillCircle(c, 3, ui::color(ui::C::Online));
        break;
    case Presence::Phantom:
        p.fillCircle(c, 3, ui::color(ui::C::PresencePhantom));
        break;
    case Presence::Dnd:
        p.fillCircle(c, 3, ui::color(ui::C::FormDivider));
        p.drawLine({c.x - 2, c.y}, {c.x + 2, c.y}, 1.5f, ui::color(ui::C::PresenceAway));
        break;
    default:
        p.strokeCircle(
            c, 3, 1, ui::color(_selected ? ui::C::SidebarSelectedText : ui::C::SidebarTextMuted)
        );
    }
}

} // namespace shell
