// net::Client: a small pool of worker threads running blocking transport
// exchanges, plus what every OS stack is told not to do itself — following
// redirects and carrying cookies along them (see net.h).
#include "base/str.h"
#include "net/net.h"
#include "net/transport.h"
#include "net/worker.h"
#include "plat/plat.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace net {

namespace {

constexpr int kMaxWorkers = 4;

using str::iequals;

// "team.slack.com" → "slack.com": the scope a caller's own Cookie header
// keeps across redirects (the Slack boot chain stays on *.slack.com).
std::string_view siteOf(std::string_view host) {
    const size_t last = host.rfind('.');
    if (last == std::string_view::npos || last == 0)
        return host;
    const size_t prev = host.rfind('.', last - 1);
    return prev == std::string_view::npos ? host : host.substr(prev + 1);
}

bool domainMatches(std::string_view host, std::string_view domain, bool hostOnly) {
    if (hostOnly)
        return host == domain;
    return host == domain || (host.size() > domain.size() && str::endsWith(host, domain) &&
                              host[host.size() - domain.size() - 1] == '.');
}

// The cookies of one redirect chain.
struct Jar {
    struct Cookie {
        std::string name, value, domain;
        bool        hostOnly = false;
    };
    std::vector<Cookie> cookies;

    void set(std::string name, std::string value, std::string domain, bool hostOnly) {
        for (auto &c : cookies)
            if (c.name == name && c.domain == domain) {
                c.value    = std::move(value);
                c.hostOnly = hostOnly;
                return;
            }
        cookies.push_back({std::move(name), std::move(value), std::move(domain), hostOnly});
    }
    void erase(std::string_view name, std::string_view domain) {
        for (size_t i = 0; i < cookies.size(); ++i)
            if (cookies[i].name == name && cookies[i].domain == domain) {
                cookies.erase(cookies.begin() + i);
                return;
            }
    }
    // A caller's "Cookie: a=1; b=2", scoped to the request's site.
    void seed(std::string_view header, std::string_view host) {
        while (!header.empty()) {
            const size_t     semi = header.find(';');
            std::string_view pair = str::trim(header.substr(0, semi));
            header = semi == std::string_view::npos ? std::string_view() : header.substr(semi + 1);
            const size_t eq = pair.find('=');
            if (eq == std::string_view::npos || eq == 0)
                continue;
            set(std::string(pair.substr(0, eq)),
                std::string(pair.substr(eq + 1)),
                std::string(siteOf(host)),
                false);
        }
    }
    // One Set-Cookie header from `host`'s response.
    void take(std::string_view header, std::string_view host) {
        const size_t     semi = header.find(';');
        std::string_view pair = str::trim(header.substr(0, semi));
        const size_t     eq   = pair.find('=');
        if (eq == std::string_view::npos || eq == 0)
            return;
        std::string      domain(host);
        bool             hostOnly = true, expired = false;
        std::string_view attrs =
            semi == std::string_view::npos ? std::string_view() : header.substr(semi + 1);
        while (!attrs.empty()) {
            const size_t     s = attrs.find(';');
            std::string_view a = str::trim(attrs.substr(0, s));
            attrs = s == std::string_view::npos ? std::string_view() : attrs.substr(s + 1);
            const size_t     e = a.find('=');
            std::string_view k = a.substr(0, e);
            std::string_view v = e == std::string_view::npos ? std::string_view() : a.substr(e + 1);
            if (iequals(k, "domain") && !v.empty()) {
                if (v[0] == '.')
                    v.remove_prefix(1);
                const std::string d = str::asciiLower(v);
                if (domainMatches(host, d, false)) { // never for a foreign domain
                    domain   = d;
                    hostOnly = false;
                }
            } else if (iequals(k, "max-age")) {
                expired = !v.empty() && (v[0] == '-' || v == "0");
            }
        }
        const std::string name(pair.substr(0, eq));
        if (expired)
            erase(name, domain);
        else
            set(name, std::string(pair.substr(eq + 1)), domain, hostOnly);
    }
    std::string headerFor(std::string_view host) const {
        std::string out;
        for (const auto &c : cookies) {
            if (!domainMatches(host, c.domain, c.hostOnly))
                continue;
            if (!out.empty())
                out += "; ";
            out += c.name;
            out += '=';
            out += c.value;
        }
        return out;
    }
};

struct Job {
    RequestId                          id = 0;
    Request                            req;
    std::shared_ptr<std::atomic<bool>> cancel;
    bool                               progress = false; // the caller set onProgress
};

} // namespace

struct Client::Impl : std::enable_shared_from_this<Client::Impl> {
    explicit Impl(plat::App &a) : app(a) {}

    plat::App                                                           &app;
    // UI thread only.
    RequestId                                                            nextId = 1;
    std::unordered_map<RequestId, std::function<void(Response)>>         done;
    std::unordered_map<RequestId, std::shared_ptr<std::atomic<bool>>>    flags;
    std::unordered_map<RequestId, std::function<void(int64_t, int64_t)>> progress;

    // Shared with the workers.
    std::mutex                 mutex;
    std::condition_variable    wake;
    std::deque<Job>            queue;
    bool                       stopping = false;
    int                        idle     = 0;
    std::deque<detail::Thread> workers; // deque: Thread is not movable

    void workerLoop();
    void run(Job &job);
    void deliver(RequestId id, Response resp);
    void report(RequestId id, int64_t received, int64_t total);
};

namespace detail {

bool parseContentLength(std::string_view v, int64_t *out) {
    v = str::trim(v);
    if (v.empty())
        return false;
    int64_t n = 0;
    for (char c : v) {
        if (c < '0' || c > '9' || n > (int64_t(1) << 50))
            return false;
        n = n * 10 + (c - '0');
    }
    *out = n;
    return true;
}

int64_t contentLength(const std::vector<Header> &headers) {
    int64_t n = 0;
    return parseContentLength(headerValue(headers, "content-length"), &n) ? n : 0;
}

std::string_view headerValue(const std::vector<Header> &headers, std::string_view name) {
    for (const auto &h : headers)
        if (iequals(h.name, name))
            return h.value;
    return {};
}

bool parseHeaderLines(std::string_view text, std::vector<Header> *out) {
    while (!text.empty()) {
        const size_t     eol  = text.find("\r\n");
        std::string_view line = text.substr(0, eol);
        text = eol == std::string_view::npos ? std::string_view() : text.substr(eol + 2);
        if (line.empty())
            break;
        if (line[0] == ' ' || line[0] == '\t') { // obsolete line folding
            if (out->empty())
                return false;
            out->back().value += ' ';
            out->back().value += str::trim(line);
            continue;
        }
        const size_t colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0)
            return false;
        out->push_back(
            {std::string(line.substr(0, colon)), std::string(str::trim(line.substr(colon + 1)))}
        );
    }
    return true;
}

} // namespace detail

std::string_view Response::header(std::string_view name) const {
    return detail::headerValue(headers, name);
}

void Client::Impl::workerLoop() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex);
            ++idle;
            wake.wait(lock, [this] { return stopping || !queue.empty(); });
            --idle;
            if (stopping)
                return;
            job = std::move(queue.front());
            queue.pop_front();
        }
        run(job);
    }
}

void Client::Impl::run(Job &job) {
    Response resp;
    Url      url;
    if (!url.parse(job.req.url) || url.scheme == "ws" || url.scheme == "wss") {
        resp.error = "url";
        resp.url   = job.req.url;
        deliver(job.id, std::move(resp));
        return;
    }
    Request         &req = job.req;
    Jar              jar;
    // Posted about once a percent (every 256 KiB when the size is unknown):
    // a hundred UI-thread hops for a whole download, whatever the transport.
    detail::Progress progress;
    if (job.progress)
        progress = [this, id = job.id, posted = int64_t(-1)](int64_t got, int64_t total) mutable {
            const int64_t step = total > 0 ? std::max<int64_t>(total / 100, 16 * 1024) : 256 * 1024;
            if (posted >= 0 && got - posted < step && got != total)
                return;
            if (got == posted)
                return;
            posted = got;
            report(id, got, total);
        };
    for (size_t i = 0; i < req.headers.size();)
        if (iequals(req.headers[i].name, "cookie")) {
            jar.seed(req.headers[i].value, url.host);
            req.headers.erase(req.headers.begin() + i);
        } else {
            ++i;
        }
    bool agent = false;
    for (const auto &h : req.headers)
        agent = agent || iequals(h.name, "user-agent");
    if (!agent)
        req.headers.push_back({"User-Agent", detail::kUserAgent});
    const std::string firstHost = url.host;
    for (int hop = 0;; ++hop) {
        Request one = req;
        if (url.host != firstHost) { // credentials never leak to another host
            for (size_t i = 0; i < one.headers.size();) {
                if (iequals(one.headers[i].name, "authorization"))
                    one.headers.erase(one.headers.begin() + i);
                else
                    ++i;
            }
        }
        const std::string cookie = jar.headerFor(url.host);
        if (!cookie.empty())
            one.headers.push_back({"Cookie", cookie});
        resp = Response();
        detail::perform(url, one, resp, *job.cancel, progress);
        resp.url = url.str();
        if (!resp.error.empty() || resp.status < 300 || resp.status >= 400 || resp.status == 304)
            break;
        const std::string_view location = resp.header("location");
        if (location.empty() || req.maxRedirects <= 0)
            break;
        if (hop >= req.maxRedirects) {
            resp.status = 0;
            resp.error  = "too_many_redirects";
            break;
        }
        for (const auto &h : resp.headers)
            if (iequals(h.name, "set-cookie"))
                jar.take(h.value, url.host);
        Url next;
        if (!next.parse(url.resolve(location)) || next.scheme == "ws" || next.scheme == "wss")
            break; // hand the 3xx back as is
        // What browsers do: 303 always, and 301/302 after a POST, become a GET.
        if (resp.status == 303 ||
            ((resp.status == 301 || resp.status == 302) && req.method == "POST")) {
            req.method = "GET";
            req.body.clear();
            for (size_t i = 0; i < req.headers.size();)
                if (iequals(req.headers[i].name, "content-type"))
                    req.headers.erase(req.headers.begin() + i);
                else
                    ++i;
        }
        url = std::move(next);
    }
    deliver(job.id, std::move(resp));
}

void Client::Impl::report(RequestId id, int64_t received, int64_t total) {
    std::weak_ptr<Impl> weak = weak_from_this();
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping)
            return;
    }
    // Posted before deliver()'s, so it runs first: never after done.
    app.post([weak, id, received, total] {
        auto self = weak.lock();
        if (!self)
            return;
        auto it = self->progress.find(id);
        if (it == self->progress.end())
            return;           // cancelled or answered
        auto fn = it->second; // it may cancel itself
        fn(received, total);
    });
}

void Client::Impl::deliver(RequestId id, Response resp) {
    std::weak_ptr<Impl> weak = weak_from_this();
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping)
            return; // the destructor is waiting for us; nothing to deliver to
    }
    app.post([weak, id, resp = std::move(resp)]() mutable {
        auto self = weak.lock();
        if (!self)
            return;
        auto it = self->done.find(id);
        if (it == self->done.end())
            return; // cancelled
        auto fn = std::move(it->second);
        self->done.erase(it);
        self->flags.erase(id);
        self->progress.erase(id);
        if (fn)
            fn(std::move(resp));
    });
}

// How many Clients are alive, so the last one out can drop the shared
// keep-alive pool (its sockets would otherwise sit open until the process
// ends — and a leak check would flag them).
static std::atomic<int> g_liveClients{0};

Client::Client(plat::App &app) : _impl(std::make_shared<Impl>(app)) {
    g_liveClients.fetch_add(1, std::memory_order_relaxed);
}

Client::~Client() {
    {
        std::lock_guard<std::mutex> lock(_impl->mutex);
        _impl->stopping = true;
        for (auto &[id, flag] : _impl->flags)
            flag->store(true);
        _impl->queue.clear();
    }
    _impl->wake.notify_all();
    for (auto &w : _impl->workers)
        w.join();
    _impl->done.clear();
    _impl->progress.clear();
    if (g_liveClients.fetch_sub(1, std::memory_order_acq_rel) == 1)
        detail::closeIdleConnections();
}

RequestId Client::send(Request req, std::function<void(Response)> done) {
    Impl           &d    = *_impl;
    const RequestId id   = d.nextId++;
    auto            flag = std::make_shared<std::atomic<bool>>(false);
    d.done.emplace(id, std::move(done));
    d.flags.emplace(id, flag);
    // The callback stays on the UI thread; the worker only knows it exists.
    const bool progress = bool(req.onProgress);
    if (progress)
        d.progress.emplace(id, std::move(req.onProgress));
    req.onProgress = nullptr;
    {
        std::lock_guard<std::mutex> lock(d.mutex);
        d.queue.push_back({id, std::move(req), flag, progress});
        if (d.idle < int(d.queue.size()) && int(d.workers.size()) < kMaxWorkers) {
            d.workers.emplace_back();
            Impl *raw = &d; // the destructor joins every worker before Impl goes
            if (!d.workers.back().start([raw] { raw->workerLoop(); }))
                d.workers.pop_back();
        }
    }
    d.wake.notify_one();
    return id;
}

void Client::cancel(RequestId id) {
    Impl &d = *_impl;
    if (auto it = d.flags.find(id); it != d.flags.end()) {
        it->second->store(true);
        d.flags.erase(it);
    }
    d.done.erase(id);
    d.progress.erase(id);
}

} // namespace net
