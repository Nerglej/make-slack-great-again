// Internals of the POSIX transport: a non-blocking TCP (+ TLS) stream with
// blocking helpers that honour a deadline, a cancel flag and a wake fd, and
// the HTTP/1.1 head parser shared by requests and the WebSocket upgrade.
#pragma once

#include "base/str.h"
#include "net/net.h"
#include "net/transport.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace net::detail {

int64_t nowMs(); // monotonic

using str::iequals; // ASCII case-insensitive

// What every blocking step waits for besides its fd. With a wake fd (an HTTP
// request's Cancel, a WebSocket's send/abort pipe) the wait blocks until the
// fd, the wake fd or the deadline fires, with no periodic wakeups, so whoever
// sets `cancel` must also write the wake fd. Without one (the Cancel's
// eventfd could not be made), waits are cut into ≤250 ms slices so a cancel
// flag set from another thread is still seen promptly.
struct Waiter {
    int64_t                  deadline = 0; // nowMs() value; 0 = none
    const std::atomic<bool> *cancel   = nullptr;
    int                      wakeFd   = -1; // readable → drained, then cancel is re-checked
};

enum class Wait : uint8_t { Ready, Timeout, Cancelled, Woken };
// Polls fd for POLLIN/POLLOUT `events`. Returns Woken only with
// returnOnWake; otherwise a wake just re-checks the cancel flag.
Wait waitFd(int fd, short events, const Waiter &w, bool returnOnWake = false);

struct TlsConn; // tls.cpp

// A connected TCP stream, TLS on top for https/wss. The socket is
// non-blocking; try*() never wait, read()/writeAll() wait through a Waiter.
class Stream {
public:
    Stream();
    ~Stream();
    Stream(const Stream &)            = delete;
    Stream &operator=(const Stream &) = delete;

    // DNS, TCP connect and (for secure URLs) the TLS handshake with
    // certificate + hostname verification. False + *error: "dns: …",
    // "connect: …", "tls: …", "timeout", "cancelled".
    bool open(const Url &url, const Waiter &w, std::string *error);

    static constexpr long Again = -1; // would block: wait for wantEvents()
    static constexpr long Fail  = -2; // the stream is dead, see error()
    // > 0 bytes, 0 at the end of the stream, Again or Fail.
    long                  tryRead(char *buf, size_t n);
    long                  tryWrite(const char *buf, size_t n);
    short                 wantEvents() const { return _want; }
    // Decrypted bytes are buffered inside TLS: read them before polling.
    bool                  pending() const;
    int                   fd() const { return _fd; }
    std::string           error() const { return _error; }

    // Blocking forms. read: > 0, 0 at the end, < 0 with *error set
    // ("timeout", "cancelled", "connect: reset" …).
    long read(char *buf, size_t n, const Waiter &w, std::string *error);
    bool writeAll(std::string_view data, const Waiter &w, std::string *error);
    // For the keep-alive pool: true when the peer has closed (or sent
    // something unasked) while the stream sat idle.
    bool idleDead();

private:
    int         _fd   = -1;
    short       _want = 0;
    TlsConn    *_tls  = nullptr;
    std::string _error;
};

// ── HTTP/1.1 heads ──────────────────────────────────────────────────────────

struct Head {
    int                 status = 0;
    int                 minor  = 1; // HTTP/1.x
    std::vector<Header> headers;
};
// Reads until a whole response head is in *buf (bytes after it stay there)
// and parses it. Returns the head's length, or -1 with *error set.
// *gotAny tells whether a single byte arrived (for the keep-alive retry).
long readHead(
    Stream &s, std::string *buf, const Waiter &w, Head *head, std::string *error, bool *gotAny
);
// "Host" header value: IPv6 literals in brackets, the port unless default.
std::string hostHeader(const Url &url);

// For tests: name lookups that went to the resolver (not answered by the
// DNS cache), and TLS handshakes that resumed a cached session.
int64_t dnsLookups();
int64_t tlsResumptions();

} // namespace net::detail
