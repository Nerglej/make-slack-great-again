#include "screens/shell/avatars.h"

#include "app/model/types.h"
#include "app/screens/common/avatar_initial.h"
#include "app/screens/common/remote_images.h"
#include "base/file.h"
#include "base/str.h"

#include <algorithm>
#include <cmath>

namespace shell {

namespace {

// The file at `path` centre-cropped to a square and resized to px×px into
// *out; false when it can't be read as an image.
bool decodeSquare(const std::string &path, int px, gfx::Bitmap *out) {
    std::string bytes;
    gfx::Bitmap b;
    // An SVG (the Claude Code teammates' glyph tiles, custom workspace icons)
    // is rendered so its shorter side is px.
    if (!file::readAll(path, &bytes) ||
        !(gfx::decodeImage(bytes, &b) || gfx::renderSvgCover(bytes, px, px, &b)) || b.empty())
        return false;
    // Centre-crop to a square, then one high-quality resize: painting then
    // only copies pixels (drawBitmap would area-average on every frame).
    *out = b.width() == px && b.height() == px ? std::move(b) : gfx::coverResize(b.view(), px, px);
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
    const int side = std::min(b.width(), b.height());
    if (side <= 0)
        return {};
    gfx::Bitmap sq = gfx::coverResize(b.view(), side, side); // a crop, no resize
    gfx::maskRoundedRect(sq, float(side) * 0.22f);
    return toPlatImage(sq);
}

Avatar::Presence Avatar::presenceOf(const model::User *u, bool hasPresence, bool phantom) {
    return !u || u->bot || !hasPresence ? Presence::None
           : u->dnd                     ? Presence::Dnd
           : u->active                  ? Presence::Active
           : phantom || u->unavailable  ? Presence::Phantom
                                        : Presence::Away;
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
