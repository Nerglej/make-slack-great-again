// Small bitmaps the shell draws everywhere (avatars, the workspace icon,
// notification and tray pictures): decoded once per (path, size) and shared.
// Message images are the messages screens' ImageCache's business; these are a
// few dozen 40-80 px squares, so a plain map with no eviction is enough.
// Files are PNG / JPEG / GIF / WebP, or SVG (Claude Code's teammate tiles),
// which is rendered at the size asked for.
//
// A path may be an http(s) URL (Slack's avatars): on disk already
// (RemoteImages) it decodes at once like a file; otherwise get() hands out an
// EMPTY bitmap — ui::Image paints its placeholder for it — and starts the
// download. When the picture lands that same bitmap is filled in place, so
// every view already holding it shows it without being told, and onLoaded
// runs (once per loop turn) for the shell to repaint the window. Callers that
// need pixels now (tray, notifications) check empty() as well as null.
#pragma once

#include "ui/ui.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace screens {
class RemoteImages;
}

namespace model {
struct User;
}

namespace shell {

class Avatars {
public:
    Avatars();
    ~Avatars();
    Avatars(const Avatars &)            = delete;
    Avatars &operator=(const Avatars &) = delete;

    // The image at `path` scaled to fit px×px (aspect kept, centre-cropped to
    // a square); null when the file is missing or undecodable (cached too);
    // empty while a URL downloads (see above).
    std::shared_ptr<const gfx::Bitmap> get(const std::string &path, int px);
    size_t                             size() const { return _cache.size(); }

    void                  setRemote(screens::RemoteImages *r) { _remote = r; }
    // A downloaded picture filled its bitmap (UI thread, coalesced).
    std::function<void()> onLoaded;

private:
    void landed(const std::string &key, const std::string &file, int px);

    std::unordered_map<std::string, std::shared_ptr<gfx::Bitmap>> _cache;
    screens::RemoteImages                                        *_remote = nullptr;
    std::shared_ptr<char>                                         _alive; // guards downloads
    bool                                                          _notifyQueued = false;
};

// premultiplied gfx::Bitmap → plat::Image (tray icons, notification pictures).
plat::Image toPlatImage(const gfx::Bitmap &b);
// msga's roundedNotifIcon: a square picture masked into the rounded square
// avatars take everywhere (corner radius 22% of the side, anti-aliased), for
// an OS notification. Non-square input is centre-cropped first.
plat::Image roundedNotificationImage(const gfx::Bitmap &b);

// A square avatar (rounded or round) with msga's presence dot (UserAvatar::
// paint): a 6 px dot centred 3 px in from the bottom-right corner on a 10 px
// disc of `ring` (the row colour) — green when active, amber when away only
// for want of an official client (phantom), a DND bar, else a hollow ring
// (bright on a selected row).
class Avatar : public ui::Image {
public:
    enum class Presence : uint8_t { None, Active, Away, Phantom, Dnd };
    Avatar();
    ~Avatar() override;
    void            setPresence(Presence p, ui::C ring, bool selected = false);
    // msga's drawUserAvatar dot for a user: none for a bot or a backend
    // without presence, then DND, active, phantom (yellow: `phantom`, e.g. me
    // with no official client connected, or a peer that can't be reached,
    // User::unavailable), else away. Null u: none.
    static Presence presenceOf(const model::User *u, bool hasPresence, bool phantom = false);
    Presence        presence() const { return _presence; }
    // msga's initial-letter placeholder: the first letter of `name` over the
    // placeholder until the photo is there.
    void            setInitial(std::string_view name);
    void            paintOver(gfx::Painter &p) override;
    void            styleChanged() override;

private:
    std::string                   _initial;
    std::unique_ptr<text::Layout> _initialLayout;
    Presence                      _presence = Presence::None;
    ui::C                         _ring     = ui::C::Sidebar;
    bool                          _selected = false;
};

} // namespace shell
