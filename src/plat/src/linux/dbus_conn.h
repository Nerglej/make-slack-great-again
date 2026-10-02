// A libdbus connection driven entirely by the App loop (watchFd / addTimer /
// post), plus small message builder/reader helpers. Nothing here blocks: the
// bus Hello is sent as an ordinary async call, so even connecting never waits
// on the daemon. Objects and signal subscriptions live in our own tables and
// are routed from one connection filter, which makes a reconnect a matter of
// re-sending the match rules (see Bus::onConnected).
#pragma once

#include "core/backends.h"

#include <dbus/dbus.h>

#include <functional>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace plat::linux_services {

// libdbus aborts the process on invalid UTF-8 or embedded NULs in a string
// argument, and strings here come from the app (labels, message bodies).
std::string sanitizeUtf8(std::string_view s);

// The session bus address: $DBUS_SESSION_BUS_ADDRESS, else the systemd user
// bus at $XDG_RUNTIME_DIR/bus. Empty when there is none.
std::string sessionBusAddress();
// The system bus address (logind, NetworkManager): $DBUS_SYSTEM_BUS_ADDRESS,
// else the well-known socket.
std::string systemBusAddress();

// file:// URI <-> local path. fileUri percent-encodes everything but the
// unreserved characters and '/'; pathFromFileUri accepts file:///p and
// file://localhost/p only (other hosts and schemes give "").
std::string fileUri(std::string_view absPath);
std::string pathFromFileUri(std::string_view uri);

// Appends arguments to a message. Containers take a callback that fills them.
class MsgWriter {
public:
    explicit MsgWriter(DBusMessage *m) { dbus_message_iter_init_append(m, &_it); }

    void str(std::string_view s);
    void objectPath(const char *p) {
        dbus_message_iter_append_basic(&_it, DBUS_TYPE_OBJECT_PATH, &p);
    }
    void boolean(bool b) {
        dbus_bool_t v = b;
        dbus_message_iter_append_basic(&_it, DBUS_TYPE_BOOLEAN, &v);
    }
    void byte(uint8_t v) { dbus_message_iter_append_basic(&_it, DBUS_TYPE_BYTE, &v); }
    void i32(int32_t v) { dbus_message_iter_append_basic(&_it, DBUS_TYPE_INT32, &v); }
    void u32(uint32_t v) { dbus_message_iter_append_basic(&_it, DBUS_TYPE_UINT32, &v); }
    void i64(int64_t v) { dbus_message_iter_append_basic(&_it, DBUS_TYPE_INT64, &v); }
    void f64(double v) { dbus_message_iter_append_basic(&_it, DBUS_TYPE_DOUBLE, &v); }
    void bytes(const uint8_t *data, size_t n); // "ay"

    void array(const char *elemSig, const std::function<void(MsgWriter &)> &fill) {
        container(DBUS_TYPE_ARRAY, elemSig, fill);
    }
    void structure(const std::function<void(MsgWriter &)> &fill) {
        container(DBUS_TYPE_STRUCT, nullptr, fill);
    }
    void dictEntry(const std::function<void(MsgWriter &)> &fill) {
        container(DBUS_TYPE_DICT_ENTRY, nullptr, fill);
    }
    void variant(const char *sig, const std::function<void(MsgWriter &)> &fill) {
        container(DBUS_TYPE_VARIANT, sig, fill);
    }
    // One "{sv}" entry of an a{sv} dictionary.
    void entry(const char *key, const char *sig, const std::function<void(MsgWriter &)> &fill) {
        dictEntry([&](MsgWriter &e) {
            e.str(key);
            e.variant(sig, fill);
        });
    }
    void strings(const std::vector<std::string> &v) {
        array("s", [&](MsgWriter &a) {
            for (const auto &s : v)
                a.str(s);
        });
    }

private:
    MsgWriter() = default;
    void container(int type, const char *sig, const std::function<void(MsgWriter &)> &fill);

    DBusMessageIter _it;
};

// Reads arguments in order. Every getter checks the type, advances on
// success and returns false (without advancing) on a mismatch.
class MsgReader {
public:
    explicit MsgReader(DBusMessage *m) { _ok = m && dbus_message_iter_init(m, &_it); }

    int  type() const { return _ok ? dbus_message_iter_get_arg_type(&_it) : DBUS_TYPE_INVALID; }
    bool atEnd() const { return type() == DBUS_TYPE_INVALID; }
    bool skip() { return _ok && dbus_message_iter_next(&_it); }

    bool      str(std::string *out); // s, o or g
    bool      boolean(bool *out);
    bool      i32(int32_t *out);
    bool      u32(uint32_t *out);
    bool      i64(int64_t *out);
    bool      f64(double *out);
    bool      unixFd(int *out); // "h": a new fd the caller owns
    // Any integer type, widened (portal values have been u and i over time).
    bool      integer(int64_t *out);
    bool      bytes(std::vector<uint8_t> *out);       // "ay"
    bool      strings(std::vector<std::string> *out); // "as"
    // Enter the container at the cursor (array, struct, dict entry, variant)
    // and step past it in this reader. The child is empty on a mismatch.
    MsgReader enter();
    // Unwrap nested variants until a non-variant value is at the cursor.
    MsgReader value();

private:
    MsgReader() = default;
    template <class T>
    bool basic(int type, T *out);

    mutable DBusMessageIter _it{};
    bool                    _ok = false;
};

// Owning handle for a DBusMessage.
struct MessageRef {
    DBusMessage *m = nullptr;
    MessageRef()   = default;
    explicit MessageRef(DBusMessage *msg) : m(msg) {}
    MessageRef(const MessageRef &o) : m(o.m) {
        if (m)
            dbus_message_ref(m);
    }
    MessageRef &operator=(MessageRef o) {
        std::swap(m, o.m);
        return *this;
    }
    ~MessageRef() {
        if (m)
            dbus_message_unref(m);
    }
    DBusMessage *get() const { return m; }
};

// Builds a method call; checks nothing (our own constant names).
MessageRef methodCall(const char *dest, const char *path, const char *iface, const char *method);
MessageRef signalMessage(const char *path, const char *iface, const char *member);

// A connection to one bus address, with automatic reconnect. Everything is
// dispatched from the App loop; callbacks never run re-entrantly from inside
// a call that sends.
class Bus {
public:
    Bus(BackendApp &app, std::string address);
    ~Bus();
    Bus(const Bus &)            = delete;
    Bus &operator=(const Bus &) = delete;

    // Opens the connection (non-blocking). False when there is no address or
    // the socket could not be opened; a reconnect is then attempted later.
    bool start();

    // True once the bus answered Hello (we have a unique name).
    bool               ready() const { return _ready; }
    // A socket is open (possibly still saying Hello): calls made now are
    // queued and sent in order.
    bool               connected() const { return _conn != nullptr; }
    const std::string &uniqueName() const { return _unique; }

    // After Hello on every (re)connect: publish state, resend match rules.
    std::function<void()> onReady;
    // The connection dropped; ready() is false until onReady runs again.
    std::function<void()> onLost;

    // Reply: the reply message, or null with the error name/message (also
    // when the connection is not up — then called from a posted closure).
    using ReplyFn =
        std::function<void(DBusMessage *reply, const char *errName, const char *errMsg)>;
    void call(MessageRef msg, ReplyFn fn = nullptr, int timeoutMs = -1);
    void send(MessageRef msg); // signals and replies; dropped when not connected

    // Method calls to `path` (exact match). Return false for "not mine";
    // libdbus then answers UnknownMethod.
    using MethodFn = std::function<bool(DBusMessage *)>;
    void addObject(const std::string &path, MethodFn fn) { _objects[path] = std::move(fn); }
    void removeObject(const std::string &path) { _objects.erase(path); }

    // Signals matching `rule` (a D-Bus match rule; sent to the daemon on
    // every connect) are passed to fn if they carry iface/member. fn does any
    // finer filtering itself (sender, path, ids).
    uint64_t subscribe(
        std::string                        rule,
        std::string                        iface,
        std::string                        member,
        std::function<void(DBusMessage *)> fn
    );
    void unsubscribe(uint64_t id);

    // Tracks the owner of a well-known name: fn(owner) now-ish (via
    // GetNameOwner) and on every NameOwnerChanged; "" = no owner.
    void watchName(const std::string &name, std::function<void(const std::string &owner)> fn);

    // Reply helpers for method handlers.
    void replyEmpty(DBusMessage *call);
    void replyError(DBusMessage *call, const char *name, const char *msg);

    BackendApp &app() const { return _app; }

private:
    struct Sub {
        std::string                        rule, iface, member;
        std::function<void(DBusMessage *)> fn;
    };

    static DBusHandlerResult filterThunk(DBusConnection *, DBusMessage *, void *);
    DBusHandlerResult        filter(DBusMessage *m);
    void                     scheduleDispatch();
    void                     dispatch();
    void                     lost();
    void                     scheduleReconnect();
    void                     addMatch(const std::string &rule);
    void                     queryOwner(const std::string &name);

    static dbus_bool_t addWatch(DBusWatch *, void *);
    static void        removeWatch(DBusWatch *, void *);
    static void        toggleWatch(DBusWatch *, void *);
    static dbus_bool_t addTimeout(DBusTimeout *, void *);
    static void        removeTimeout(DBusTimeout *, void *);
    static void        toggleTimeout(DBusTimeout *, void *);
    void               syncWatch(DBusWatch *w);
    void               syncTimeout(DBusTimeout *t);

    BackendApp                      &_app;
    std::string                      _address;
    DBusConnection                  *_conn  = nullptr;
    bool                             _ready = false;
    std::string                      _unique;
    std::shared_ptr<Bus *>           _alive; // posted closures check it
    bool                             _dispatchQueued = false;
    bool                             _dispatching    = false;
    bool                             _lostPending    = false;
    std::set<DBusPendingCall *>      _pending; // in flight
    std::map<DBusWatch *, uint64_t>  _watches;
    std::map<DBusTimeout *, TimerId> _timeouts;
    std::map<std::string, MethodFn>  _objects;
    std::map<uint64_t, Sub>          _subs;
    uint64_t                         _nextSub = 1;
    std::map<std::string, std::vector<std::function<void(const std::string &)>>> _names;
    TimerId _reconnectTimer = 0;
    int     _backoffMs      = 1000;
};

} // namespace plat::linux_services
