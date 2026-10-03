// WorkspaceCache — one workspace's Store on disk, so a start opens at once.
//
// What is kept, per workspace, in <cacheDir>/workspaces/<key>/ (app/identity.h; earlier
// versions kept their own format in <dataDir>/cache/<key>, left alone):
//   roster.json          conversations: kind, names, members, read cursors,
//                        unread/mention badges, star, mute, notify level,
//                        local name
//   users.json           every known user and bot
//   emoji.json           custom emoji, aliases included
//   meta.json            me, the last open conversation, my user groups,
//                        muted threads, reminders, and the backend's own
//                        state ("x": Slack's saved items, followed threads,
//                        off-roster probe times, the DM activity sweep stamp)
//   messages/<conv>.json the newest kMaxMessages top-level messages of each
//                        conversation opened (no pending copies, no threads)
//
// Reading: load() fills an empty Store synchronously at activation; the
// backend then merges the network's answers into it with its usual rules.
// Messages load per conversation when it is first opened (loadMessages).
//
// Writing: the cache observes the Store and writes what changed at most
// once per kWriteDelayMs (1 s), on close(),
// and only when a file's bytes differ from what it last wrote. Files are
// written atomically and owner-only (0600), on the UI thread: the files are
// small (users.json is the big one).
//
// Wiping: remove() on sign-out of that workspace, clearAll() for Settings →
// "Clear cache" — after which caches still open write nothing more this
// run, so the next start is a cold one.
//
// Format: JSON, every record a positional array in the order of its field
// list (workspace_cache.cpp) — no key per field. A file whose "v" is not
// kVersion, or that does not parse, is ignored: the start is a cold one.
#pragma once

#include "app/model/store.h"
#include "base/json.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace plat {
class App;
}

namespace cache {

class WorkspaceCache {
public:
    static constexpr int kMaxMessages  = 50;   // per conversation
    static constexpr int kWriteDelayMs = 1000; // batched writes, at most one a second

    // `dir` is this workspace's directory (dirFor); "" keeps nothing.
    WorkspaceCache(plat::App &app, model::Store &store, std::string dir);
    ~WorkspaceCache(); // close(true)
    WorkspaceCache(const WorkspaceCache &)            = delete;
    WorkspaceCache &operator=(const WorkspaceCache &) = delete;

    // <cacheDir>/workspaces ("" when the OS has no cache directory).
    static std::string root(plat::App &app);
    // The directory of a workspace record key ("slack:T0123").
    static std::string dirFor(plat::App &app, std::string_view workspaceKey);
    // Bytes of every workspace's files (Settings → Storage).
    static int64_t     diskBytes(plat::App &app);
    // Settings → "Clear cache": every workspace's files.
    static void        clearAll(plat::App &app);
    // Sign-out: that workspace's files.
    static void        remove(plat::App &app, std::string_view workspaceKey);

    // Fills the Store from disk (users, conversations, emoji, my groups,
    // muted threads, reminders, me). False when there is no usable roster —
    // then nothing is loaded. *meta keeps meta.json for the caller's own
    // state (meta->root()["x"]).
    bool        load(json::Document *meta);
    // The conversation that was open last ("" if none).
    std::string lastConversation() const { return _last; }
    // Conversation c's cached messages into the Store (a page). Returns the
    // newest ts loaded, 0 if there was nothing. Once per conversation.
    model::Ts   loadMessages(model::ConvRef c);
    // From now on c's messages are kept (an opened conversation).
    void        track(model::ConvRef c);
    void        setLastConversation(model::ConvRef c);

    // The backend's own state, written as the members of meta's "x" object;
    // call extrasChanged() when it changed.
    std::function<void(json::Writer &)> saveExtras;
    void                                extrasChanged();
    void                                emojiChanged(); // the Store has no event for emoji

    // Writes what is pending now.
    void flush();
    // Stops observing; keep = flush first, else drop what is pending. No
    // write happens afterwards (call it before the Store is cleared).
    void close(bool keep);

private:
    enum Dirty : uint8_t { kConvs = 1, kUsers = 2, kEmoji = 4, kMeta = 8 };
    void        onChange(const model::Change &ch);
    void        mark(uint8_t what);
    void        schedule();
    bool        tracked(model::ConvRef c) const { return c < _tracked.size() && _tracked[c]; }
    bool        writable() const;
    void        write(const char *name, std::string data);
    std::string messagesFile(model::ConvRef c) const;

    plat::App                                &_app;
    model::Store                             &_store;
    std::string                               _dir, _last;
    uint32_t                                  _clearGen; // clearAll() since: write nothing
    model::Store::ObserverId                  _observer        = 0;
    uint64_t                                  _timer           = 0;
    uint8_t                                   _dirty           = 0;
    // The Store's profile/presence revisions users.json holds: a presence
    // flip doesn't re-serialise the roster (it is saved at close, or with
    // the next profile change).
    uint64_t                                  _usersProfileRev = 0, _usersPresenceRev = 0;
    std::vector<uint8_t>                      _tracked, _checked, _msgDirty; // by ConvRef
    std::unordered_map<std::string, uint64_t> _written; // file → hash of its bytes
};

} // namespace cache
