// ImageCache — decoded, scaled-to-need, byte-bounded images for every screen
// (avatars, message images, GIFs, custom emoji, favicons).
//
//   auto bmp = ctx.images.get({path, 72, 72, ImageCache::Shape::Circle}, this);
//   if (bmp) p.drawBitmap(bmp->view(), rect); else paint a placeholder;
//
// get() never blocks: it returns what is cached for that exact request (path,
// physical size, shape) or null, and queues the decode on a worker thread.
// When the result lands (on the UI thread, via plat::App::post) every view
// that asked for it as `waiter` gets update(), and listeners are called. A
// waiter view MUST call forget(this) from its destructor (CachedImage does).
//
// What is cached is exactly what is painted: the file is decoded, scaled to
// the requested physical size (cover-cropped when the aspect differs) and,
// for Circle/Rounded, pre-masked with anti-aliased corners — so painting is a
// straight blit with no per-frame resampling or clipping. Entries are evicted
// least-recently-used once the total passes the budget (default 48 MB);
// handed-out bitmaps are shared_ptrs, so eviction never pulls pixels from
// under a paint. A file that fails to decode is remembered (no retry storm).
//
// A path is an absolute local file (fixture assets, uploads) or an http(s)
// URL: URLs are downloaded to disk by RemoteImages (setRemote; without one
// they fail) and decoded from there, waiters notified the same way. A failed
// download is asked for again once RemoteImages' cool-down for it is over.
#pragma once

#include "gfx/gfx.h"
#include "plat/plat.h"
#include "ui/ui.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace screens {

class RemoteImages;

class ImageCache {
public:
    enum class Shape : uint8_t { Square, Rounded, Circle };
    struct Request {
        std::string path;
        int         width = 0, height = 0; // physical px; 0,0 = the natural size
        Shape       shape  = Shape::Square;
        float       radius = 0; // physical px, Rounded only
    };
    using Bitmap = std::shared_ptr<const gfx::Bitmap>;
    using Frames = std::shared_ptr<const std::vector<gfx::AnimFrame>>;

    explicit ImageCache(plat::App &app, size_t budgetBytes = size_t(48) << 20);
    ~ImageCache(); // stops and joins the worker
    ImageCache(const ImageCache &)            = delete;
    ImageCache &operator=(const ImageCache &) = delete;

    // The still image (first frame of a GIF), or null while loading/failed.
    Bitmap get(const Request &r, ui::View *waiter = nullptr);
    // Every frame of an animation (one frame for a still), or null.
    Frames frames(const Request &r, ui::View *waiter = nullptr);
    // True once the request finished decoding and failed (show a fallback).
    bool   failed(const Request &r) const;
    // Drop `waiter` from every pending request (call from its destructor).
    void   forget(ui::View *waiter);

    // Natural pixel size from the file header (no decode; memoised). False
    // for a URL not downloaded yet.
    bool naturalSize(const std::string &path, int *w, int *h);

    // Where URLs come from (main's; may stay null).
    void          setRemote(RemoteImages *r) { _remote = r; }
    RemoteImages *remote() const { return _remote; }

    // Called (UI thread) whenever any image finished loading.
    using ListenerId = uint32_t;
    ListenerId listen(std::function<void()> fn);
    void       unlisten(ListenerId id);

    // Settings → Animate GIFs and images: off, animations show their first frame.
    void setAnimate(bool on) { _animate = on; }
    bool animate() const { return _animate; }
    // Settings → Animate emoji: animated custom emoji (message text,
    // reactions) play; off, they show their first frame.
    void setAnimateEmoji(bool on) { _animateEmoji = on; }
    bool animateEmoji() const { return _animateEmoji; }

    size_t bytes() const { return _bytes; }
    size_t budget() const { return _budget; }
    void   setBudget(size_t b);
    size_t entryCount() const;
    // Requests queued or decoding (tests pump until this is 0).
    size_t pending() const;

    struct Impl; // worker state, shared with posted results

private:
    struct Entry;
    Entry *lookup(const Request &r, bool animated, ui::View *waiter);
    void   load(Entry &e, const Request &r);
    void   decode(uint64_t id, Request r, bool animated);
    void deliver(uint64_t id, std::vector<gfx::AnimFrame> frames, bool ok, bool transient = false);
    void evict();

    plat::App                          &_app;
    std::shared_ptr<Impl>               _impl;
    RemoteImages                       *_remote = nullptr;
    std::vector<std::unique_ptr<Entry>> _entries;
    struct Listener {
        ListenerId            id;
        std::function<void()> fn;
    };
    std::vector<Listener> _listeners;
    struct SizeMemo {
        std::string path;
        int         w, h;
        bool        ok;
    };
    std::vector<SizeMemo> _sizes;
    size_t                _bytes = 0, _budget;
    uint64_t              _clock = 0, _nextId = 1;
    ListenerId            _nextListener = 1;
    bool                  _animate      = true;
    bool                  _animateEmoji = true;
};

// A view showing one cached image at its own size (avatars, favicons,
// thumbnails). Placeholder colour until the image is ready; animated GIFs
// play while the view is in a window (and not under reduced motion).
class CachedImage : public ui::View {
public:
    CachedImage(
        ImageCache       &cache,
        std::string       path   = {},
        ImageCache::Shape shape  = ImageCache::Shape::Square,
        float             radius = 0
    );
    ~CachedImage() override;

    void               setPath(std::string path);
    void               setShape(ImageCache::Shape s, float radius = 0); // radius logical px
    // Play GIF frames; `emoji`: gated by animateEmoji(), not animate().
    void               setAnimated(bool on, bool emoji = false);
    void               setPlaceholder(ui::C c) { _placeholder = c; }
    // msga's "Loading image…": text centred over the placeholder until the
    // picture is there (Font::Body in `color`).
    void               setLoadingText(std::string text, ui::C color);
    const std::string &path() const { return _path; }

    void paint(gfx::Painter &p) override;
    void windowChanged() override;
    void styleChanged() override;

protected:
    // What shows while the image isn't there (default: the placeholder colour).
    virtual void paintPlaceholder(gfx::Painter &p, ui::RectF r, float radius);

private:
    ImageCache::Request request() const;
    void                scheduleFrame();

    std::string                   _loadingText;
    std::unique_ptr<text::Layout> _loadingLayout;
    ui::C                         _loadingColor = ui::C::TextFaint;

    ImageCache        &_cache;
    std::string        _path;
    ImageCache::Frames _frames; // while animating
    plat::TimerId      _timer  = 0;
    float              _radius = 0;
    int                _frame  = 0;
    ImageCache::Shape  _shape;
    ui::C              _placeholder = ui::C::Border;
    bool               _animated    = false;
    bool               _emoji       = false;
};

} // namespace screens
