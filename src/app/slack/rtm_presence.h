// The presence link: an RTM WebSocket on a session
// (xoxc) workspace's own token, held only so Slack counts this app as a
// connected client. Slack reports a user active only while one of its
// clients holds a connection (users.getPresence's `online`); the public
// users.setPresence can force "away" but never "active", so a Web-API-only
// client is away to everyone unless the official app happens to run.
//
// rtm.connect works for a session token (not a granular OAuth one), and the
// wss URL it returns carries no auth: the handshake MUST send the `d` cookie,
// or Slack answers {"type":"error",…invalid_auth} and drops the socket ~5 s
// later. Frames on the socket (every conversation's events) are ignored —
// delivery stays with the polling; only hello, pong and error are read.
// Slack pongs RTM pings (unlike Socket Mode), so two missed pongs are a real
// liveness signal (it also catches a laptop-sleep gap). Input is forwarded
// as `tickle` frames, the official client's way of resetting auto-away.
#pragma once

#include "app/model/backend.h"
#include "app/slack/web_api.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace plat {
class App;
}

namespace slack {

class RtmPresence {
public:
    using Mode    = model::Backend::PresenceMode;
    using Link    = model::Backend::PresenceLink;
    // `connect` performs rtm.connect with the workspace's token, cookie and
    // host (the backend's api()); `cookie` rides the WebSocket handshake.
    using Connect = std::function<void(std::string form, ApiDone done)>;
    RtmPresence(plat::App &app, Connect connect, std::string cookie);
    ~RtmPresence(); // closes; onStateChanged never runs afterwards
    RtmPresence(const RtmPresence &)            = delete;
    RtmPresence &operator=(const RtmPresence &) = delete;

    // Native drops the link; either holding mode establishes it (WhileUsing
    // counts the change itself as activity). Idempotent.
    void setMode(Mode mode);
    Mode mode() const { return _mode; }
    Link state() const { return _state; }
    // Real input: WhileUsing re-arms the idle clock and brings an idle link
    // back; both holding modes send a (throttled) tickle.
    void noteActivity();

    std::function<void(Link)> onStateChanged;

    // ── Test seams ──────────────────────────────────────────────────────────
    struct Timing {
        int pingMs         = 30'000;      // json ping cadence (Slack pongs each)
        int tickleGapMs    = 60'000;      // min spacing of activity tickles
        int alwaysTickleMs = 5 * 60'000;  // WhileRunning: unconditional tickles
        int idleMs         = 30 * 60'000; // WhileUsing: no input this long drops it
        int reconnectMinMs = 2'000, reconnectMaxMs = 60'000;
        int stableMs = 30'000; // a connection this long resets the backoff
    };
    void setTimingForTest(const Timing &t) { _t = t; }
    bool connected() const;
    int  ticklesSentForTest() const { return _tickles; }

private:
    bool holding() const { return _mode != Mode::Native; }
    void ensureHolding();
    void openAndConnect();
    void connectWs(const std::string &url);
    void teardown(); // the socket, the handshake, the link timers; state untouched
    void scheduleReconnect();
    void onOpen();
    void onClosed(int code, const std::string &reason);
    void onText(const std::string &text);
    void sendPing();
    void sendTickle(bool force);
    void onIdle();
    void setState(Link s);
    void stopTimer(uint64_t &id);

    plat::App                      &_app;
    Connect                         _connect;
    std::string                     _cookie;
    Timing                          _t;
    Mode                            _mode  = Mode::Native;
    Link                            _state = Link::Off;
    std::unique_ptr<net::WebSocket> _ws;
    // One rtm.connect or socket open at a time (rtm.connect is Tier 1).
    bool                            _connecting     = false;
    // rtm.connect refused the token outright: given up until the mode changes.
    bool                            _unavailable    = false;
    int                             _generation     = 0; // bumps per teardown
    int                             _reconnectMs    = 0;
    int64_t                         _connectedSince = 0; // monotonic ms; 0 = down
    int                             _pingId = 0, _awaitingPongs = 0, _tickles = 0;
    int64_t                         _lastTickle = 0;
    uint64_t                        _pingTimer = 0, _tickleTimer = 0, _idleTimer = 0;
    uint64_t                        _reconnectTimer = 0;
    std::shared_ptr<bool>           _alive;
};

} // namespace slack
