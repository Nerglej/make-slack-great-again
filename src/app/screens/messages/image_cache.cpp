#include "app/screens/messages/image_cache.h"

#include "app/model/image_size.h"
#include "app/screens/common/remote_images.h"
#include "base/file.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace screens {

namespace {

// Scales `src` to exactly w×h, covering (centre crop) when the aspect differs.
gfx::Bitmap coverScale(const gfx::Bitmap &src, int w, int h) {
    if (w <= 0 || h <= 0 || (w == src.width() && h == src.height()))
        return src;
    const float s  = std::max(float(w) / float(src.width()), float(h) / float(src.height()));
    const int   tw = std::max(w, int(std::ceil(float(src.width()) * s)));
    const int   th = std::max(h, int(std::ceil(float(src.height()) * s)));
    gfx::Bitmap r  = gfx::resize(src.view(), tw, th);
    if (tw == w && th == h)
        return r;
    gfx::Bitmap out(w, h);
    const int   ox = (tw - w) / 2, oy = (th - h) / 2;
    for (int y = 0; y < h; ++y)
        std::copy_n(
            r.pixels() + size_t(y + oy) * size_t(tw) + size_t(ox),
            size_t(w),
            out.pixels() + size_t(y) * size_t(w)
        );
    return out;
}

// Multiplies premultiplied pixels by the coverage of a rounded rect (a circle
// when radius >= half the short side), anti-aliased over one pixel.
void maskCorners(gfx::Bitmap &b, float radius) {
    const int   w = b.width(), h = b.height();
    const float r = std::min(radius, std::min(w, h) / 2.f);
    if (r <= 0)
        return;
    uint32_t *px = b.pixels();
    for (int y = 0; y < h; ++y) {
        const float fy = float(y) + 0.5f;
        const float cy = fy < r ? r : fy > float(h) - r ? float(h) - r : fy;
        for (int x = 0; x < w; ++x) {
            const float fx = float(x) + 0.5f;
            const float cx = fx < r ? r : fx > float(w) - r ? float(w) - r : fx;
            if (cx == fx || cy == fy)
                continue; // not in a corner square: fully inside
            const float d   = std::hypot(fx - cx, fy - cy);
            const float cov = std::clamp(r - d + 0.5f, 0.f, 1.f);
            if (cov >= 1)
                continue;
            uint32_t      &p  = px[size_t(y) * size_t(w) + size_t(x)];
            const uint32_t a  = uint32_t(float(p >> 24) * cov + 0.5f);
            const uint32_t rr = uint32_t(float((p >> 16) & 0xff) * cov + 0.5f);
            const uint32_t g  = uint32_t(float((p >> 8) & 0xff) * cov + 0.5f);
            const uint32_t bb = uint32_t(float(p & 0xff) * cov + 0.5f);
            p                 = (a << 24) | (rr << 16) | (g << 8) | bb;
        }
    }
}

std::string keyOf(const ImageCache::Request &r, bool animated) {
    std::string k = r.path;
    k += '\x1f';
    k += std::to_string(r.width);
    k += 'x';
    k += std::to_string(r.height);
    k += char('0' + int(r.shape));
    if (r.shape == ImageCache::Shape::Rounded)
        k += std::to_string(int(r.radius * 4));
    k += animated ? 'A' : 'S';
    return k;
}

} // namespace

// ── Worker ──────────────────────────────────────────────────────────────────

struct ImageCache::Impl {
    struct Job {
        uint64_t id;
        Request  req;
        bool     animated;
    };
    std::mutex              m;
    std::condition_variable cv;
    std::deque<Job>         jobs;
    std::thread             thread;
    bool                    stop     = false;
    plat::App              *app      = nullptr;
    ImageCache             *owner    = nullptr; // UI thread only; null once destroyed
    size_t                  inflight = 0;       // UI thread: queued, decoding or posted

    static void run(std::shared_ptr<Impl> self) {
        for (;;) {
            Job job;
            {
                std::unique_lock lock(self->m);
                self->cv.wait(lock, [&] { return self->stop || !self->jobs.empty(); });
                if (self->stop)
                    return;
                job = std::move(self->jobs.front());
                self->jobs.pop_front();
            }
            auto        frames = std::make_shared<std::vector<gfx::AnimFrame>>();
            bool        ok     = false;
            std::string data;
            if (file::readAll(job.req.path, &data)) {
                if (job.animated) {
                    ok = gfx::decodeAnimation(data, frames.get()) && !frames->empty();
                } else {
                    gfx::AnimFrame f;
                    ok = gfx::decodeImage(data, &f.frame);
                    if (ok)
                        frames->push_back(std::move(f));
                }
                // An SVG (the Claude Code teammates' glyph tiles): rendered
                // to cover the requested size, as the shell's avatars do.
                float sw = 0, sh = 0;
                if (!ok && gfx::svgSize(data, &sw, &sh) && sw > 0 && sh > 0) {
                    const Request &q = job.req;
                    const float    k = q.width <= 0 && q.height <= 0
                                           ? 1.f
                                           : std::max(float(q.width) / sw, float(q.height) / sh);
                    gfx::AnimFrame f;
                    ok = gfx::renderSvg(
                        data,
                        std::max(1, int(std::lround(sw * k))),
                        std::max(1, int(std::lround(sh * k))),
                        &f.frame
                    );
                    if (ok) {
                        frames->clear();
                        frames->push_back(std::move(f));
                    }
                }
            }
            data = {};
            if (ok) {
                const Request &r = job.req;
                for (gfx::AnimFrame &f : *frames) {
                    f.frame = coverScale(f.frame, r.width, r.height);
                    if (r.shape == Shape::Circle)
                        maskCorners(f.frame, 1e9f);
                    else if (r.shape == Shape::Rounded)
                        maskCorners(f.frame, r.radius);
                }
            }
            const uint64_t id = job.id;
            self->app->post([self, id, frames, ok] {
                if (self->owner)
                    self->owner->deliver(id, std::move(*frames), ok);
            });
        }
    }
};

struct ImageCache::Entry {
    std::string             key;
    Bitmap                  still;
    Frames                  frames;
    std::vector<ui::View *> waiters;
    size_t                  bytes   = 0;
    uint64_t                lastUse = 0, id = 0;
    bool                    retry = false; // a failed download: ask again once RemoteImages would
    enum : uint8_t { Loading, Ready, Failed } state = Loading;
    bool animated                                   = false;
};

ImageCache::ImageCache(plat::App &app, size_t budget)
    : _app(app), _impl(std::make_shared<Impl>()), _budget(budget) {
    _impl->app    = &app;
    _impl->owner  = this;
    _impl->thread = std::thread(Impl::run, _impl);
}

ImageCache::~ImageCache() {
    _impl->owner = nullptr; // results still in the post queue are dropped
    {
        std::lock_guard lock(_impl->m);
        _impl->stop = true;
        _impl->jobs.clear();
    }
    _impl->cv.notify_all();
    if (_impl->thread.joinable())
        _impl->thread.join();
}

ImageCache::Entry *ImageCache::lookup(const Request &r, bool animated, ui::View *waiter) {
    if (r.path.empty())
        return nullptr;
    const std::string key = keyOf(r, animated);
    for (auto &e : _entries) {
        if (e->key != key)
            continue;
        e->lastUse = ++_clock;
        // Its cool-down is RemoteImages' (which a sign-in also ends).
        if (e->state == Entry::Failed && e->retry && _remote && !_remote->failedRecently(r.path)) {
            e->state = Entry::Loading;
            e->retry = false;
            load(*e, r);
        }
        if (e->state == Entry::Loading && waiter &&
            std::find(e->waiters.begin(), e->waiters.end(), waiter) == e->waiters.end())
            e->waiters.push_back(waiter);
        return e.get();
    }
    auto e      = std::make_unique<Entry>();
    e->key      = key;
    e->id       = _nextId++;
    e->animated = animated;
    e->lastUse  = ++_clock;
    if (waiter)
        e->waiters.push_back(waiter);
    Entry *raw = e.get();
    _entries.push_back(std::move(e));
    load(*raw, r);
    return raw;
}

void ImageCache::load(Entry &e, const Request &r) {
    ++_impl->inflight; // until deliver(), whichever way the bytes come
    const uint64_t id = e.id;
    const bool     an = e.animated;
    if (!RemoteImages::isRemote(r.path)) {
        decode(id, r, an);
        return;
    }
    if (!_remote) {
        _app.post([impl = _impl, id] {
            if (impl->owner)
                impl->owner->deliver(id, {}, false);
        });
        return;
    }
    if (std::string local = _remote->cachedPath(r.path); !local.empty()) {
        Request lr = r;
        lr.path    = std::move(local);
        decode(id, std::move(lr), an);
        return;
    }
    _remote->fetch(r.path, [impl = _impl, id, r, an](const std::string &path) {
        if (!impl->owner)
            return;
        if (path.empty()) {
            impl->owner->deliver(id, {}, false, true);
            return;
        }
        Request lr = r;
        lr.path    = path;
        impl->owner->decode(id, std::move(lr), an);
    });
}

void ImageCache::decode(uint64_t id, Request r, bool animated) {
    {
        std::lock_guard lock(_impl->m);
        _impl->jobs.push_back({id, std::move(r), animated});
    }
    _impl->cv.notify_one();
}

ImageCache::Bitmap ImageCache::get(const Request &r, ui::View *waiter) {
    Entry *e = lookup(r, false, waiter);
    return e && e->state == Entry::Ready ? e->still : nullptr;
}

ImageCache::Frames ImageCache::frames(const Request &r, ui::View *waiter) {
    Entry *e = lookup(r, true, waiter);
    return e && e->state == Entry::Ready ? e->frames : nullptr;
}

bool ImageCache::failed(const Request &r) const {
    const std::string k1 = keyOf(r, false), k2 = keyOf(r, true);
    for (const auto &e : _entries)
        if ((e->key == k1 || e->key == k2) && e->state == Entry::Failed)
            return true;
    return false;
}

void ImageCache::forget(ui::View *waiter) {
    for (auto &e : _entries)
        e->waiters.erase(
            std::remove(e->waiters.begin(), e->waiters.end(), waiter), e->waiters.end()
        );
}

void ImageCache::deliver(uint64_t id, std::vector<gfx::AnimFrame> frames, bool ok, bool transient) {
    --_impl->inflight;
    Entry *e = nullptr;
    for (auto &x : _entries)
        if (x->id == id)
            e = x.get();
    if (!e)
        return;
    if (ok) {
        e->state = Entry::Ready;
        for (const gfx::AnimFrame &f : frames)
            e->bytes += size_t(f.frame.width()) * size_t(f.frame.height()) * 4;
        if (e->animated) {
            e->frames = std::make_shared<const std::vector<gfx::AnimFrame>>(std::move(frames));
        } else {
            e->still = std::make_shared<const gfx::Bitmap>(std::move(frames.front().frame));
        }
        _bytes += e->bytes;
    } else {
        e->state = Entry::Failed;
        e->retry = transient; // the download failed, not the decode: ask again later
    }
    std::vector<ui::View *> waiters = std::move(e->waiters);
    e->waiters.clear();
    for (ui::View *v : waiters)
        v->update();
    for (size_t i = 0; i < _listeners.size(); ++i) {
        auto fn = _listeners[i].fn; // a listener may unlisten itself
        fn();
    }
    evict();
}

void ImageCache::evict() {
    while (_bytes > _budget) {
        size_t victim = SIZE_MAX;
        for (size_t i = 0; i < _entries.size(); ++i)
            if (_entries[i]->state == Entry::Ready &&
                (victim == SIZE_MAX || _entries[i]->lastUse < _entries[victim]->lastUse))
                victim = i;
        if (victim == SIZE_MAX)
            return;
        _bytes -= _entries[victim]->bytes;
        _entries.erase(_entries.begin() + ptrdiff_t(victim));
    }
}

void ImageCache::setBudget(size_t b) {
    _budget = b;
    evict();
}

size_t ImageCache::entryCount() const {
    return _entries.size();
}
size_t ImageCache::pending() const {
    return _impl->inflight;
}

ImageCache::ListenerId ImageCache::listen(std::function<void()> fn) {
    _listeners.push_back({_nextListener, std::move(fn)});
    return _nextListener++;
}

void ImageCache::unlisten(ListenerId id) {
    _listeners.erase(
        std::remove_if(
            _listeners.begin(), _listeners.end(), [id](const Listener &l) { return l.id == id; }
        ),
        _listeners.end()
    );
}

bool ImageCache::naturalSize(const std::string &path, int *w, int *h) {
    for (const SizeMemo &m : _sizes)
        if (m.path == path) {
            *w = m.w;
            *h = m.h;
            return m.ok;
        }
    std::string file = path;
    if (RemoteImages::isRemote(path)) {
        // Only once it is on disk; until then the caller has its own guess.
        file = _remote ? _remote->cachedPath(path) : std::string();
        if (file.empty()) {
            *w = *h = 0;
            return false;
        }
    }
    int32_t    iw = 0, ih = 0;
    const bool ok = model::imageSize(file, &iw, &ih);
    _sizes.push_back({path, iw, ih, ok});
    *w = iw;
    *h = ih;
    return ok;
}

// ── CachedImage ─────────────────────────────────────────────────────────────

CachedImage::CachedImage(ImageCache &cache, std::string path, ImageCache::Shape shape, float radius)
    : _cache(cache), _path(std::move(path)), _radius(radius), _shape(shape) {
    setRole(ui::Role::Image);
}

CachedImage::~CachedImage() {
    ui::app()->cancelTimer(_timer);
    _cache.forget(this);
}

void CachedImage::setPath(std::string path) {
    if (path == _path)
        return;
    _path = std::move(path);
    _frames.reset();
    _frame = 0;
    update();
}

void CachedImage::setLoadingText(std::string text, ui::C color) {
    _loadingText  = std::move(text);
    _loadingColor = color;
    _loadingLayout.reset();
    update();
}

void CachedImage::styleChanged() {
    _loadingLayout.reset(); // theme or text size changed
    View::styleChanged();
}

void CachedImage::paintPlaceholder(gfx::Painter &p, ui::RectF r, float radius) {
    if (_placeholder == ui::C::None)
        return;
    if (radius > 0)
        p.fillRoundRect(r, radius, ui::color(_placeholder));
    else
        p.fillRect(r, ui::color(_placeholder));
}

void CachedImage::setShape(ImageCache::Shape s, float radius) {
    _shape  = s;
    _radius = radius;
    update();
}

void CachedImage::setAnimated(bool on, bool emoji) {
    _animated = on;
    _emoji    = emoji;
    _frames.reset();
    update();
}

ImageCache::Request CachedImage::request() const {
    const float s = window() ? window()->scale() : 1.f;
    return {
        _path, int(std::lround(width() * s)), int(std::lround(height() * s)), _shape, _radius * s
    };
}

void CachedImage::windowChanged() {
    if (!window()) {
        ui::app()->cancelTimer(_timer);
        _timer = 0;
    }
}

void CachedImage::scheduleFrame() {
    if (_timer || !_frames || _frames->size() < 2 || !window() || ui::app()->reducedMotion() ||
        !(_emoji ? _cache.animateEmoji() : _cache.animate()))
        return;
    const int delay = std::max(20, (*_frames)[size_t(_frame) % _frames->size()].delayMs);
    _timer          = ui::app()->addTimer(delay, false, [this] {
        _timer = 0;
        _frame = int(size_t(_frame + 1) % _frames->size());
        update();
    });
}

void CachedImage::paint(gfx::Painter &p) {
    View::paint(p);
    if (width() <= 0 || height() <= 0)
        return;
    const gfx::Bitmap *bmp = nullptr;
    ImageCache::Bitmap still;
    if (_animated) {
        const ImageCache::Request r = request();
        if (!_frames || (*_frames)[0].frame.width() != r.width)
            if (auto f = _cache.frames(r, this))
                _frames = std::move(f);
        if (_frames) {
            bmp = &(*_frames)[size_t(_frame) % _frames->size()].frame;
            scheduleFrame();
        }
    } else {
        still = _cache.get(request(), this);
        bmp   = still.get();
    }
    const ui::RectF r      = bounds();
    const float     radius = _shape == ImageCache::Shape::Circle    ? std::min(r.w, r.h) / 2
                             : _shape == ImageCache::Shape::Rounded ? _radius
                                                                    : 0;
    if (!bmp) {
        paintPlaceholder(p, r, radius);
        if (_loadingText.empty())
            return;
        if (!_loadingLayout) {
            text::AttributedText t;
            t.append(_loadingText, ui::font(ui::Font::Body, _loadingColor));
            text::LayoutOptions o;
            o.maxLines     = 1;
            _loadingLayout = text::Layout::build(t, o, window() ? window()->scale() : 1.f);
        }
        const text::Layout &l = *_loadingLayout;
        p.save();
        p.clipRect(r);
        l.paint(p, snapPx({std::floor((r.w - l.width()) / 2), std::floor((r.h - l.height()) / 2)}));
        p.restore();
        return;
    }
    // Scaled and masked for exactly this size: a 1:1 blit.
    p.drawBitmap(bmp->view(), r, gfx::Sampling::Nearest);
}

} // namespace screens
