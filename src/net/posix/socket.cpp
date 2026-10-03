// Sockets for the POSIX transport: name lookup, non-blocking connect and the
// Stream every exchange runs over (see posix.h).
#include "base/str.h"
#include "net/posix/posix.h"
#include "net/transport.h"

#include <algorithm>
#include <arpa/inet.h>
#include <climits>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

namespace net::detail {

// tls.cpp
struct TlsConn;
TlsConn *tlsNew(int fd, const std::string &host, std::string *error);
void     tlsFree(TlsConn *c);
long     tlsHandshake(TlsConn *c, short *want, std::string *error);
long     tlsRead(TlsConn *c, char *buf, size_t n, short *want, std::string *error);
long     tlsWrite(TlsConn *c, const char *buf, size_t n, short *want, std::string *error);
bool     tlsPending(const TlsConn *c);

int64_t nowMs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

Wait waitFd(int fd, short events, const Waiter &w, bool returnOnWake) {
    for (;;) {
        if (w.cancel && w.cancel->load(std::memory_order_relaxed))
            return Wait::Cancelled;
        // A wake fd interrupts at once (whoever sets the cancel flag also
        // writes it), so such a wait can block until something happens; only
        // a bare cancel flag needs polling, in 250 ms slices.
        int slice = w.wakeFd >= 0 ? -1 : 250;
        if (w.deadline) {
            const int64_t left = w.deadline - nowMs();
            if (left <= 0)
                return Wait::Timeout;
            if (slice < 0 || left < slice)
                slice = int(std::min<int64_t>(left, INT_MAX));
        }
        pollfd    p[2] = {{fd, events, 0}, {w.wakeFd, POLLIN, 0}};
        const int n    = poll(p, w.wakeFd >= 0 ? 2 : 1, slice);
        if (n < 0 && errno != EINTR)
            return Wait::Ready; // let the I/O call report what is wrong
        if (n <= 0)
            continue;
        if (w.wakeFd >= 0 && p[1].revents) {
            char drain[64];
            while (::read(w.wakeFd, drain, sizeof drain) > 0) {
            }
            if (w.cancel && w.cancel->load(std::memory_order_relaxed))
                return Wait::Cancelled;
            if (returnOnWake)
                return Wait::Woken;
        }
        if (fd >= 0 && p[0].revents)
            return Wait::Ready;
    }
}

namespace {

std::string sysError(std::string_view what, int err) {
    return str::concat({what, ": ", std::strerror(err)});
}

std::string waitError(Wait r) {
    return r == Wait::Cancelled ? "cancelled" : "timeout";
}

// getaddrinfo cannot be interrupted, and a dead resolver can block it for
// tens of seconds. It runs on its own detached thread so the caller can give
// up on time. Both sides hold a reference; the last one out frees the state
// (and an answer nobody waits for any more).
struct Lookup {
    std::mutex              mutex;
    std::condition_variable cv;
    std::string             host, port;
    addrinfo               *result = nullptr;
    int                     rc     = 0;
    bool                    done   = false;
    std::atomic<int>        refs{2};

    void release() {
        if (refs.fetch_sub(1, std::memory_order_acq_rel) != 1)
            return;
        if (result)
            freeaddrinfo(result);
        delete this;
    }
};

void *lookupThread(void *p) {
    auto    *lk = static_cast<Lookup *>(p);
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *res     = nullptr;
    const int rc      = getaddrinfo(lk->host.c_str(), lk->port.c_str(), &hints, &res);
    {
        std::lock_guard<std::mutex> lock(lk->mutex);
        lk->rc     = rc;
        lk->result = rc == 0 ? res : nullptr;
        lk->done   = true;
        lk->cv.notify_one();
    }
    lk->release();
    return nullptr;
}

addrinfo *resolve(const Url &url, const Waiter &w, std::string *error) {
    const std::string port = str::number(url.port);
    addrinfo          hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_NUMERICHOST | AI_NUMERICSERV;
    addrinfo *res     = nullptr;
    if (getaddrinfo(url.host.c_str(), port.c_str(), &hints, &res) == 0)
        return res; // an IP literal: nothing to wait for

    auto *lk = new Lookup;
    lk->host = url.host;
    lk->port = port;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    // glibc's NSS modules want more than musl's default 128 KiB.
    pthread_attr_setstacksize(&attr, 256 * 1024);
    pthread_t t;
    const int started = pthread_create(&t, &attr, lookupThread, lk);
    pthread_attr_destroy(&attr);
    if (started != 0) {
        delete lk;
        *error = "dns: no thread";
        return nullptr;
    }
    int rc = 0;
    {
        std::unique_lock<std::mutex> lock(lk->mutex);
        while (!lk->done) {
            const bool cancelled = w.cancel && w.cancel->load(std::memory_order_relaxed);
            if (cancelled || (w.deadline && nowMs() >= w.deadline)) {
                *error = cancelled ? "cancelled" : "timeout";
                break;
            }
            lk->cv.wait_for(lock, std::chrono::milliseconds(100));
        }
        if (lk->done) {
            rc         = lk->rc;
            res        = lk->result;
            lk->result = nullptr; // ours now
        } else {
            res = nullptr;
        }
    }
    const bool gaveUp = !res && rc == 0;
    lk->release();
    if (gaveUp)
        return nullptr;
    if (rc != 0) {
        *error = str::concat({"dns: ", gai_strerror(rc)});
        return nullptr;
    }
    return res;
}

// One non-blocking connect; returns the fd or -1 with *error.
int connectOne(const addrinfo *ai, const Waiter &w, std::string *error) {
    const int fd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        *error = sysError("connect", errno);
        return -1;
    }
    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
        return fd;
    if (errno != EINPROGRESS) {
        *error = sysError("connect", errno);
        ::close(fd);
        return -1;
    }
    const Wait r = waitFd(fd, POLLOUT, w);
    if (r != Wait::Ready) {
        *error = waitError(r);
        ::close(fd);
        return -1;
    }
    int       err = 0;
    socklen_t len = sizeof err;
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
    if (err != 0) {
        *error = sysError("connect", err);
        ::close(fd);
        return -1;
    }
    return fd;
}

} // namespace

Stream::Stream() = default;

Stream::~Stream() {
    if (_tls)
        tlsFree(_tls);
    if (_fd >= 0)
        ::close(_fd);
}

bool Stream::open(const Url &url, const Waiter &w, std::string *error) {
    addrinfo *list = resolve(url, w, error);
    if (!list)
        return false;
    for (const addrinfo *ai = list; ai; ai = ai->ai_next) {
        // A black-holed address (broken IPv6) must not eat the whole
        // timeout while another one would answer.
        Waiter one = w;
        if (ai->ai_next) {
            const int64_t cap = nowMs() + 4000;
            if (!one.deadline || one.deadline > cap)
                one.deadline = cap;
        }
        _fd = connectOne(ai, one, error);
        if (_fd >= 0 || *error == "cancelled" ||
            (*error == "timeout" && one.deadline == w.deadline))
            break;
    }
    freeaddrinfo(list);
    if (_fd < 0)
        return false;
    if (!url.secure())
        return true;
    _tls = tlsNew(_fd, url.host, error);
    if (!_tls)
        return false;
    for (;;) {
        const long r = tlsHandshake(_tls, &_want, error);
        if (r == 0)
            return true;
        if (r == Fail)
            return false;
        const Wait wr = waitFd(_fd, _want, w);
        if (wr != Wait::Ready) {
            *error = waitError(wr);
            return false;
        }
    }
}

long Stream::tryRead(char *buf, size_t n) {
    if (_tls)
        return tlsRead(_tls, buf, n, &_want, &_error);
    for (;;) {
        const ssize_t r = ::recv(_fd, buf, n, 0);
        if (r >= 0)
            return long(r);
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            _want = POLLIN;
            return Again;
        }
        _error = sysError("connect", errno);
        return Fail;
    }
}

long Stream::tryWrite(const char *buf, size_t n) {
    if (_tls)
        return tlsWrite(_tls, buf, n, &_want, &_error);
    for (;;) {
#ifdef MSG_NOSIGNAL
        const ssize_t r = ::send(_fd, buf, n, MSG_NOSIGNAL); // a closed peer must not SIGPIPE us
#else
        const ssize_t r = ::send(_fd, buf, n, 0);
#endif
        if (r >= 0)
            return long(r);
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            _want = POLLOUT;
            return Again;
        }
        _error = sysError("connect", errno);
        return Fail;
    }
}

bool Stream::pending() const {
    return _tls && tlsPending(_tls);
}

long Stream::read(char *buf, size_t n, const Waiter &w, std::string *error) {
    for (;;) {
        if (!pending()) {
            // Poll first even when data may be there: the cancel flag and
            // the deadline are checked on every call.
            const Wait r = waitFd(_fd, POLLIN, w);
            if (r != Wait::Ready) {
                *error = waitError(r);
                return -1;
            }
        }
        const long got = tryRead(buf, n);
        if (got >= 0)
            return got;
        if (got == Fail) {
            *error = _error;
            return -1;
        }
        if (_want == POLLOUT) { // TLS wants to write first (rare)
            const Wait r = waitFd(_fd, POLLOUT, w);
            if (r != Wait::Ready) {
                *error = waitError(r);
                return -1;
            }
        }
    }
}

bool Stream::writeAll(std::string_view data, const Waiter &w, std::string *error) {
    size_t off = 0;
    while (off < data.size()) {
        const long r = tryWrite(data.data() + off, data.size() - off);
        if (r > 0) {
            off += size_t(r);
            continue;
        }
        if (r == Fail || r == 0) {
            *error = r == 0 ? "connect: closed" : _error;
            return false;
        }
        const Wait wr = waitFd(_fd, _want, w);
        if (wr != Wait::Ready) {
            *error = waitError(wr);
            return false;
        }
    }
    return true;
}

bool Stream::idleDead() {
    pollfd p{_fd, POLLIN, 0};
    if (poll(&p, 1, 0) == 0)
        return false;
    // Readable while idle: the peer closed, or (TLS 1.3) sent a session
    // ticket, which TLS swallows and reports as "nothing for you".
    char       c;
    const long r = tryRead(&c, 1);
    return r != Again;
}

} // namespace net::detail
