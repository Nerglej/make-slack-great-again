// net::WebSocket: one reader thread per socket runs the transport's
// blocking connect/recv and posts every event to the UI thread.
#include "net/net.h"
#include "net/transport.h"
#include "net/worker.h"
#include "plat/plat.h"

#include <mutex>

namespace net {

struct WebSocket::Impl : std::enable_shared_from_this<WebSocket::Impl> {
    explicit Impl(plat::App &a) : app(a) {}

    plat::App &app;
    WebSocket *owner = nullptr; // UI thread; null once the WebSocket is gone
    bool       open  = false;   // UI thread
    uint64_t   epoch = 0;       // UI thread: bumps per open(), stale events are dropped

    std::mutex                      mutex; // guards conn against the destructor
    std::unique_ptr<detail::WsConn> conn;
    detail::Thread                  reader;

    void post(uint64_t ep, std::function<void(WebSocket &)> fn) {
        std::weak_ptr<Impl> weak = weak_from_this();
        app.post([weak, ep, fn = std::move(fn)] {
            auto self = weak.lock();
            if (self && self->owner && self->epoch == ep)
                fn(*self->owner);
        });
    }
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (conn)
                conn->abort();
        }
        reader.join();
        std::lock_guard<std::mutex> lock(mutex);
        conn.reset();
    }
};

WebSocket::WebSocket(plat::App &app) : _impl(std::make_shared<Impl>(app)) {
    _impl->owner = this;
}

WebSocket::~WebSocket() {
    _impl->owner = nullptr;
    _impl->stop();
}

void WebSocket::open(std::string url, std::vector<Header> headers, int timeoutMs) {
    Impl &d = *_impl;
    d.stop();
    d.open            = false;
    const uint64_t ep = ++d.epoch;
    Url            u;
    if (!u.parse(url) || (u.scheme != "ws" && u.scheme != "wss")) {
        d.post(ep, [](WebSocket &ws) {
            if (auto f = ws.onClosed)
                f(0, "url");
        });
        return;
    }
    {
        std::lock_guard<std::mutex> lock(d.mutex);
        d.conn = detail::makeWsConn();
    }
    detail::WsConn *conn = d.conn.get();
    Impl           *raw  = &d; // stop() joins the reader before Impl or conn go
    d.reader.start([raw, conn, ep, u = std::move(u), headers = std::move(headers), timeoutMs] {
        std::string err;
        if (!conn->connect(u, headers, timeoutMs, &err)) {
            raw->post(ep, [err](WebSocket &ws) {
                if (auto f = ws.onClosed)
                    f(0, err);
            });
            return;
        }
        raw->post(ep, [raw](WebSocket &ws) {
            raw->open = true;
            if (auto f = ws.onOpen)
                f();
        });
        detail::WsMessage msg;
        while (conn->recv(msg)) {
            if (msg.kind == detail::WsMessage::Text)
                raw->post(ep, [data = std::move(msg.data)](WebSocket &ws) mutable {
                    if (auto f = ws.onText)
                        f(std::move(data));
                });
            else if (msg.kind == detail::WsMessage::Binary)
                raw->post(ep, [data = std::move(msg.data)](WebSocket &ws) mutable {
                    if (auto f = ws.onBinary)
                        f(std::move(data));
                });
            msg = detail::WsMessage();
        }
        raw->post(
            ep,
            [raw, code = msg.code ? msg.code : 1006, reason = std::move(msg.data)](WebSocket &ws) {
                raw->open = false;
                if (auto f = ws.onClosed)
                    f(code, reason);
            }
        );
    });
}

bool WebSocket::isOpen() const {
    return _impl->open;
}

void WebSocket::sendText(std::string_view text) {
    Impl &d = *_impl;
    if (!d.open)
        return;
    std::lock_guard<std::mutex> lock(d.mutex);
    if (d.conn)
        d.conn->send(text, true);
}

bool WebSocket::ping(std::string_view payload) {
    Impl &d = *_impl;
    if (!d.open)
        return false;
    std::lock_guard<std::mutex> lock(d.mutex);
    return d.conn && d.conn->ping(payload);
}

void WebSocket::close(int code) {
    Impl                       &d = *_impl;
    std::lock_guard<std::mutex> lock(d.mutex);
    if (d.conn)
        d.conn->close(code);
}

} // namespace net
