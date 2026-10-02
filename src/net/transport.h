// The per-OS seam under net.h (internal to the module). The common code
// (client.cpp, websocket.cpp) owns threads, queues, redirects, cookies and
// delivery to the UI thread; a transport only does blocking I/O on the
// thread it is called from:
//
//   posix/  our own HTTP/1.1 + WebSocket over sockets + mbedTLS (Linux)
//   win/    WinHTTP (HTTP and its WebSocket API), the OS's TLS
//   mac/    NSURLSession (data tasks, NSURLSessionWebSocketTask), the OS's TLS
#pragma once

#include "net/net.h"

#include <atomic>
#include <memory>
#include <string>
#include <string_view>

namespace net::detail {

// What every request says it is when the caller sets no User-Agent: the
// same on every OS, and what the old app's Qt stack sent everywhere.
inline constexpr const char *kUserAgent = "Mozilla/5.0";

// One HTTP exchange — no redirect following, no cookie handling, no
// decompression (the request carries no Accept-Encoding; a stack that adds
// one must also decode). Blocks the calling worker thread. Fills
// resp.status/headers/body, or resp.error (see Response::error) with
// status 0. Honours req.timeoutMs; returns promptly (within ~250 ms) with
// error "cancelled" once *cancel turns true. resp.url is set by the caller.
// May keep connections alive between calls (thread-safe pool).
//
// `progress`, when set, hears the body as it arrives (bytes so far, the
// Content-Length or 0) on the calling worker thread, as often as the
// transport likes: client.cpp throttles it. req.onProgress is never set.
using Progress = std::function<void(int64_t received, int64_t total)>;
void perform(
    const Url               &url,
    const Request           &req,
    Response                &resp,
    const std::atomic<bool> &cancel,
    const Progress          &progress
);
// The Content-Length header's value; 0 when absent or not a number.
int64_t contentLength(const std::vector<Header> &headers);

// Drops idle kept-alive connections (Client destructor of the last client).
void closeIdleConnections();

struct WsMessage {
    enum Kind : uint8_t { Text, Binary, Closed } kind = Text;
    std::string data;     // Text / Binary payload; Closed: the reason
    int         code = 0; // Closed: the peer's close code, 1006 when dropped
};

// One WebSocket connection. connect() and recv() run on the socket's own
// reader thread; send() and close() may be called from any thread while
// recv() blocks there (implementations serialise writes themselves).
class WsConn {
public:
    virtual ~WsConn() = default;
    // The opening handshake. False + *error ("dns", "connect", "tls",
    // "handshake", "timeout", "unsupported", optionally ": detail").
    virtual bool connect(
        const Url &url, const std::vector<Header> &headers, int timeoutMs, std::string *error
    )                                                   = 0;
    // Blocks until a whole message (fragments joined, pings answered,
    // pongs dropped) or the end of the connection: then kind = Closed and
    // false is returned. Never returns Closed twice.
    virtual bool recv(WsMessage &msg)                   = 0;
    virtual bool send(std::string_view data, bool text) = 0;
    // A ping control frame (≤ 125 bytes of payload); its pong is dropped.
    // False when the stack can't send one (WinHTTP: it sends its own
    // keepalives instead).
    virtual bool ping(std::string_view payload)         = 0;
    // Starts the closing handshake; recv() then returns Closed.
    virtual void close(int code)                        = 0;
    // Tears the connection down now (destructor path): a blocked recv()
    // returns Closed promptly.
    virtual void abort()                                = 0;
};
std::unique_ptr<WsConn> makeWsConn();

// net::freeLoopbackPort, per OS (Winsock needs its own start-up).
int loopbackPort();

} // namespace net::detail
