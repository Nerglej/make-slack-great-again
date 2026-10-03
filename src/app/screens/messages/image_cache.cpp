#include "app/screens/messages/image_cache.h"

#include "app/model/image_size.h"
#include "app/screens/common/remote_images.h"
#include "base/file.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace screens {

namespace {

// naturalSize's memo stays this small (it is cleared when full).
constexpr size_t kMaxSizeMemos  = 4096;
// How long "a URL not on disk yet" is believed without a new look.
constexpr double kMissingMemoMs = 5000;

double monoMs() {
    using namespace std::chrono;
    return double(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count()) /
           1000.0;
}

} // namespace

ImageCache::Key ImageCache::keyOf(const Ref &r, bool animated) {
    return {
        r.path,
        r.width,
        r.height,
        r.shape == Shape::Rounded ? int(r.radius * 4) : 0,
        uint8_t(r.shape),
        animated
    };
}

size_t ImageCache::KeyHash::operator()(const Key &k) const {
    size_t h = std::hash<std::string_view>{}(k.path);
    for (const uint64_t v :
         {uint64_t(uint32_t(k.width)) << 32 | uint32_t(k.height),
          uint64_t(uint32_t(k.radius4)) << 16 | uint64_t(k.shape) << 1 | uint64_t(k.animated)})
        h ^= std::hash<uint64_t>{}(v) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}

struct ImageCache::Entry {
    std::string             path; // the Key's view points here
    int                     width = 0, height = 0;
    float                   radius = 0;
    Shape                   shape  = Shape::Square;
    Bitmap                  still;
    Frames                  frames;
    std::vector<ui::View *> waiters;
    size_t                  bytes = 0;
    uint64_t                id    = 0;
    Entry                  *prev = nullptr, *next = nullptr; // the LRU list (Ready only)
    bool                    retry = false; // a failed download: ask again once RemoteImages would
    enum : uint8_t { Loading, Ready, Failed } state = Loading;
    bool animated                                   = false;
    bool inLru                                      = false;

    Key     key() const { return keyOf(Ref{path, width, height, shape, radius}, animated); }
    Request request() const { return {path, width, height, shape, radius}; }
};

// ── Worker ──────────────────────────────────────────────────────────────────

struct ImageCache::Impl {
    struct Job {
        uint64_t id;
        Request  req;
        bool     animated;
    };
    struct Result {
        std::vector<gfx::AnimFrame> frames;
        int                         naturalW = 0, naturalH = 0;
        bool                        ok = false, sized = false;
    };
    std::mutex              m;
    std::condition_variable cv;
    std::deque<Job>         jobs;
    std::thread             thread;
    bool                    stop     = false;
    plat::App              *app      = nullptr;
    ImageCache             *owner    = nullptr; // UI thread only; null once destroyed
    size_t                  inflight = 0;       // UI thread: queued, decoding or posted

    static void work(const Job &job, Result &out) {
        const Request &r = job.req;
        std::string    data;
        if (!file::readAll(r.path, &data))
            return;
        // naturalSize's answer for it, while the file is at hand.
        if (int32_t nw = 0, nh = 0; model::imageSize(r.path, &nw, &nh)) {
            out.sized    = true;
            out.naturalW = nw;
            out.naturalH = nh;
        }
        auto &frames = out.frames;
        bool  ok     = false;
        if (job.animated) {
            // Each frame is shrunk as it is decoded, under the frame budget.
            gfx::AnimOptions o;
            o.width  = r.width;
            o.height = r.height;
            ok       = gfx::decodeAnimation(data, &frames, o) && !frames.empty();
        } else {
            gfx::AnimFrame f;
            ok = gfx::decodeImage(data, &f.frame);
            if (ok)
                frames.push_back(std::move(f));
        }
        // An SVG (the Claude Code teammates' glyph tiles): rendered to cover
        // the requested size, as the shell's avatars are.
        if (gfx::AnimFrame f; !ok && gfx::renderSvgCover(data, r.width, r.height, &f.frame)) {
            ok = true;
            frames.clear();
            frames.push_back(std::move(f));
        }
        data = {};
        if (!ok) {
            frames.clear();
            return;
        }
        for (gfx::AnimFrame &f : frames) {
            if (r.width > 0 && r.height > 0 &&
                (r.width != f.frame.width() || r.height != f.frame.height()))
                f.frame = gfx::coverResize(f.frame.view(), r.width, r.height);
            if (r.shape == Shape::Circle)
                gfx::maskRoundedRect(f.frame, 1e9f);
            else if (r.shape == Shape::Rounded)
                gfx::maskRoundedRect(f.frame, r.radius);
        }
        out.ok = true;
    }

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
            auto res = std::make_shared<Result>();
            work(job, *res);
            self->app->post([self, id = job.id, path = std::move(job.req.path), res] {
                ImageCache *owner = self->owner;
                if (!owner)
                    return;
                if (res->sized) { // under the path asked for (a URL, not its local copy)
                    const auto it = owner->_loading.find(id);
                    owner->noteSize(
                        it != owner->_loading.end() ? it->second->path : path,
                        res->naturalW,
                        res->naturalH,
                        true
                    );
                }
                owner->deliver(id, std::move(res->frames), res->ok);
            });
        }
    }
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

void ImageCache::lruUnlink(Entry &e) {
    if (!e.inLru)
        return;
    if (e.prev)
        e.prev->next = e.next;
    else
        _lruHead = e.next;
    if (e.next)
        e.next->prev = e.prev;
    else
        _lruTail = e.prev;
    e.prev = e.next = nullptr;
    e.inLru         = false;
}

void ImageCache::lruPushFront(Entry &e) {
    e.prev = nullptr;
    e.next = _lruHead;
    if (_lruHead)
        _lruHead->prev = &e;
    _lruHead = &e;
    if (!_lruTail)
        _lruTail = &e;
    e.inLru = true;
}

ImageCache::Entry *ImageCache::lookup(const Ref &r, bool animated, ui::View *waiter) {
    if (r.path.empty())
        return nullptr;
    if (const auto it = _entries.find(keyOf(r, animated)); it != _entries.end()) {
        Entry &e = *it->second;
        if (e.inLru && _lruHead != &e) { // now the most recently used
            lruUnlink(e);
            lruPushFront(e);
        }
        // Its cool-down is RemoteImages' (which a sign-in also ends).
        if (e.state == Entry::Failed && e.retry && _remote && !_remote->failedRecently(e.path)) {
            e.state = Entry::Loading;
            e.retry = false;
            load(e);
        }
        if (e.state == Entry::Loading && waiter &&
            std::find(e.waiters.begin(), e.waiters.end(), waiter) == e.waiters.end()) {
            e.waiters.push_back(waiter);
            _waited.insert(&e);
        }
        return &e;
    }
    auto e      = std::make_shared<Entry>();
    e->path     = std::string(r.path);
    e->width    = r.width;
    e->height   = r.height;
    e->shape    = r.shape;
    e->radius   = r.radius;
    e->id       = _nextId++;
    e->animated = animated;
    if (waiter) {
        e->waiters.push_back(waiter);
        _waited.insert(e.get());
    }
    Entry *raw = e.get();
    _entries.emplace(raw->key(), std::move(e));
    load(*raw);
    return raw;
}

void ImageCache::load(Entry &e) {
    ++_impl->inflight; // until deliver(), whichever way the bytes come
    _loading[e.id]    = &e;
    const uint64_t id = e.id;
    const bool     an = e.animated;
    if (!RemoteImages::isRemote(e.path)) {
        decode(id, e.request(), an);
        return;
    }
    if (!_remote) {
        _app.post([impl = _impl, id] {
            if (impl->owner)
                impl->owner->deliver(id, {}, false);
        });
        return;
    }
    if (std::string local = _remote->cachedPath(e.path); !local.empty()) {
        Request lr = e.request();
        lr.path    = std::move(local);
        decode(id, std::move(lr), an);
        return;
    }
    _remote->fetch(e.path, [impl = _impl, id, r = e.request(), an](const std::string &path) {
        if (!impl->owner)
            return;
        // What naturalSize remembered about it not being here is over.
        impl->owner->_sizes.erase(r.path);
        if (path.empty()) {
            impl->owner->deliver(id, {}, false, true);
            return;
        }
        Request lr = r;
        lr.path    = path;
        impl->owner->decode(id, std::move(lr), an);
    });
}

void ImageCache::decode(uint64_t id, Request r, bool animated, bool urgent) {
    {
        std::lock_guard lock(_impl->m);
        if (urgent)
            _impl->jobs.push_front({id, std::move(r), animated});
        else
            _impl->jobs.push_back({id, std::move(r), animated});
    }
    _impl->cv.notify_one();
}

void ImageCache::decodeOnce(Request r, std::function<void(gfx::Bitmap)> done, bool urgent) {
    ++_impl->inflight; // until deliver()
    const uint64_t id = _nextId++;
    _once.emplace(id, std::move(done));
    decode(id, std::move(r), false, urgent);
}

ImageCache::Bitmap ImageCache::get(const Ref &r, ui::View *waiter, Handle *held) {
    Entry *e = lookup(r, false, waiter);
    if (!e || e->state != Entry::Ready)
        return nullptr;
    if (held)
        held->_e = _entries.find(e->key())->second;
    return e->still;
}

ImageCache::Frames ImageCache::frames(const Ref &r, ui::View *waiter, Handle *held) {
    Entry *e = lookup(r, true, waiter);
    if (!e || e->state != Entry::Ready)
        return nullptr;
    if (held)
        held->_e = _entries.find(e->key())->second;
    return e->frames;
}

bool ImageCache::touch(const Handle &h) {
    const std::shared_ptr<Entry> e = h._e.lock();
    if (!e || !e->inLru)
        return false;
    if (_lruHead != e.get()) {
        lruUnlink(*e);
        lruPushFront(*e);
    }
    return true;
}

bool ImageCache::failed(const Ref &r) const {
    for (const bool animated : {false, true})
        if (const auto it = _entries.find(keyOf(r, animated));
            it != _entries.end() && it->second->state == Entry::Failed)
            return true;
    return false;
}

void ImageCache::forget(ui::View *waiter) {
    // Only loading entries have waiters.
    for (auto it = _waited.begin(); it != _waited.end();) {
        auto &ws = (*it)->waiters;
        ws.erase(std::remove(ws.begin(), ws.end(), waiter), ws.end());
        it = ws.empty() ? _waited.erase(it) : std::next(it);
    }
}

void ImageCache::deliver(uint64_t id, std::vector<gfx::AnimFrame> frames, bool ok, bool transient) {
    --_impl->inflight;
    if (const auto once = _once.find(id); once != _once.end()) {
        auto done = std::move(once->second);
        _once.erase(once);
        done(ok && !frames.empty() ? std::move(frames.front().frame) : gfx::Bitmap());
        return;
    }
    const auto it = _loading.find(id);
    if (it == _loading.end())
        return;
    Entry *e = it->second;
    _loading.erase(it);
    if (ok) {
        e->state = Entry::Ready;
        e->bytes = 0;
        for (const gfx::AnimFrame &f : frames)
            e->bytes += size_t(f.frame.width()) * size_t(f.frame.height()) * 4;
        if (e->animated) {
            e->frames = std::make_shared<const std::vector<gfx::AnimFrame>>(std::move(frames));
        } else {
            e->still = std::make_shared<const gfx::Bitmap>(std::move(frames.front().frame));
        }
        _bytes += e->bytes;
        lruPushFront(*e);
    } else {
        e->state = Entry::Failed;
        e->retry = transient; // the download failed, not the decode: ask again later
    }
    std::vector<ui::View *> waiters = std::move(e->waiters);
    e->waiters.clear();
    _waited.erase(e);
    for (ui::View *v : waiters)
        v->update();
    for (size_t i = 0; i < _listeners.size(); ++i) {
        auto fn = _listeners[i].fn; // a listener may unlisten itself
        fn();
    }
    evict();
}

void ImageCache::evict() {
    // The least recently used ready entry goes first, O(1) each.
    while (_bytes > _budget && _lruTail) {
        Entry &victim = *_lruTail;
        lruUnlink(victim);
        _bytes -= victim.bytes;
        _entries.erase(victim.key()); // destroys it; held handles expire
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

void ImageCache::noteSize(const std::string &path, int w, int h, bool ok) {
    if (_sizes.size() >= kMaxSizeMemos && !_sizes.count(path))
        _sizes.clear(); // a bound, not a policy: an answer is cheap to find again
    _sizes[path] = {w, h, ok, false, 0};
}

bool ImageCache::naturalSize(const std::string &path, int *w, int *h) {
    *w = *h = 0;
    if (const auto it = _sizes.find(path); it != _sizes.end()) {
        const SizeMemo &m = it->second;
        if (!m.missing) {
            *w = m.w;
            *h = m.h;
            return m.ok;
        }
        if (monoMs() - m.at < kMissingMemoMs)
            return false;
        _sizes.erase(it); // look again
    }
    ++_sizeProbes;
    std::string file = path;
    if (RemoteImages::isRemote(path)) {
        // Only once it is on disk; until then the caller has its own guess.
        file = _remote ? _remote->cachedPath(path) : std::string();
        if (file.empty()) {
            if (_sizes.size() >= kMaxSizeMemos)
                _sizes.clear();
            _sizes[path] = {0, 0, false, true, monoMs()};
            return false;
        }
    }
    // The header only (a few KB), no decode.
    int32_t    iw = 0, ih = 0;
    const bool ok = model::imageSize(file, &iw, &ih);
    noteSize(path, iw, ih, ok);
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
    drop();
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
    if (s != _shape || radius != _radius)
        drop();
    _shape  = s;
    _radius = radius;
    update();
}

void CachedImage::setAnimated(bool on, bool emoji) {
    _animated = on;
    _emoji    = emoji;
    drop();
    update();
}

void CachedImage::drop() {
    _frames.reset();
    _still.reset();
    _held.reset();
    _heldW = _heldH = 0;
}

ImageCache::Ref CachedImage::ref() const {
    const float s = windowScale();
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
    const gfx::Bitmap    *bmp = nullptr;
    const ImageCache::Ref q   = ref();
    // Once ready, the picture is kept and painted without a lookup; the
    // cache only hears that it is still in use (its LRU). A new size asks again.
    const bool            held =
        (_animated ? bool(_frames) : bool(_still)) && q.width == _heldW && q.height == _heldH;
    if (held) {
        _cache.touch(_held);
    } else if (_animated) {
        if (auto f = _cache.frames(q, this, &_held)) {
            _frames = std::move(f);
            _heldW  = q.width;
            _heldH  = q.height;
        }
    } else {
        _still = _cache.get(q, this, &_held); // the placeholder until this size is there
        _heldW = _still ? q.width : 0;
        _heldH = _still ? q.height : 0;
    }
    if (_animated && _frames) {
        bmp = &(*_frames)[size_t(_frame) % _frames->size()].frame;
        scheduleFrame();
    } else if (!_animated) {
        bmp = _still.get();
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
            _loadingLayout = text::Layout::build(t, o, windowScale());
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
