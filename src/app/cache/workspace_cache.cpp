#include "app/cache/workspace_cache.h"

#include "app/identity.h"
#include "base/file.h"
#include "base/crypto.h"
#include "base/log.h"
#include "base/str.h"
#include "plat/plat.h"

#include <initializer_list>

namespace cache {

using model::ConvRef;
using model::kNoUser;
using model::Ts;
using model::UserRef;

namespace {

constexpr int64_t kVersion = 1;

// clearAll() calls so far: a cache opened before the last one writes nothing.
uint32_t g_clearGen = 0;

// ── Field lists ─────────────────────────────────────────────────────────────
//
// One list per record type, run in both directions by one IO: writing, it
// appends each field to a JSON array; reading, it takes them back in the
// same order (a missing trailing field reads as its default, so a record can
// only grow at the end). Change a list in the middle and bump kVersion. One
// IO with a direction flag, not two IO types: every list exists once in
// the binary.

class IO {
public:
    IO(json::Writer &w, const model::Store &s) : _w(&w), _s(const_cast<model::Store *>(&s)) {}
    IO(model::Store &s, json::Value record) : _s(&s) {
        _stack.push_back({record.begin(), record.end()});
    }

    bool   reading() const { return !_w; }
    void   str(std::string &v);
    void   num(int64_t &v);
    void   num(int32_t &v);
    void   num(uint32_t &v);
    void   byte(uint8_t &v);
    void   flags(std::initializer_list<bool *> f);
    void   user(UserRef &r);
    // A nested array: its element count when reading, `n` when writing.
    size_t open(size_t n);
    void   close();
    // A part that may be absent (null): true if present.
    bool   optional(bool present);

private:
    struct Level {
        json::Value::Iterator it, end;
    };
    json::Value        next();
    int64_t            integer(int64_t v); // writes v, or reads the next one
    json::Writer      *_w = nullptr;
    model::Store      *_s;
    std::vector<Level> _stack;
};

json::Value IO::next() {
    Level &l = _stack.back();
    if (!(l.it != l.end))
        return {};
    const json::Value v = *l.it;
    ++l.it;
    return v;
}

int64_t IO::integer(int64_t v) {
    if (_w) {
        _w->value(v);
        return v;
    }
    return next().integer();
}

void IO::str(std::string &v) {
    if (_w)
        _w->value(v);
    else
        v = std::string(next().str());
}

void IO::num(int64_t &v) {
    v = integer(v);
}
void IO::num(int32_t &v) {
    v = int32_t(integer(v));
}
void IO::num(uint32_t &v) {
    v = uint32_t(integer(v));
}
void IO::byte(uint8_t &v) {
    v = uint8_t(integer(v));
}

void IO::flags(std::initializer_list<bool *> f) {
    int64_t bits = 0, bit = 1;
    for (bool *b : f) {
        bits |= *b ? bit : 0;
        bit <<= 1;
    }
    bits = integer(bits);
    bit  = 1;
    for (bool *b : f) {
        *b = (bits & bit) != 0;
        bit <<= 1;
    }
}

void IO::user(UserRef &r) {
    if (_w) {
        _w->value(_s->user(r).id);
        return;
    }
    const std::string_view id = next().str();
    r                         = id.empty() ? kNoUser : _s->internUser(id);
}

size_t IO::open(size_t n) {
    if (_w) {
        _w->beginArray();
        return n;
    }
    const json::Value v = next();
    _stack.push_back({v.begin(), v.end()});
    return v.isArray() ? v.size() : 0;
}

void IO::close() {
    if (_w)
        _w->endArray();
    else
        _stack.pop_back();
}

bool IO::optional(bool present) {
    if (_w) {
        if (!present)
            _w->null();
        return present;
    }
    Level &l = _stack.back();
    if (l.it != l.end && (*l.it).isArray())
        return true;
    next(); // null: absent
    return false;
}

void fields(IO &io, model::User &u);
void fields(IO &io, model::Reaction &r);
void fields(IO &io, model::File &f);
void fields(IO &io, model::AttachmentField &f);
void fields(IO &io, model::Block &b);
void fields(IO &io, model::Button &b);
void fields(IO &io, model::Attachment &a);
void fields(IO &io, model::Message &m);
void fields(IO &io, model::Conversation &c);

// A list of records, each its own array.
template <class T>
void records(IO &io, std::vector<T> &v) {
    const size_t n = io.open(v.size());
    if (io.reading())
        v.resize(n);
    for (T &x : v) {
        io.open(0);
        fields(io, x);
        io.close();
    }
    io.close();
}

void users(IO &io, std::vector<UserRef> &v) {
    const size_t n = io.open(v.size());
    if (io.reading())
        v.assign(n, kNoUser);
    for (UserRef &u : v)
        io.user(u);
    io.close();
}

void fields(IO &io, model::User &u) {
    io.str(u.id);
    io.str(u.name);
    io.str(u.displayName);
    io.str(u.title);
    io.str(u.email);
    io.str(u.avatar);
    io.str(u.statusEmoji);
    io.str(u.statusText);
    io.num(u.tzOffset);
    // Presence too: the dots show until the first poll.
    io.flags({&u.hasTz, &u.active, &u.bot, &u.admin, &u.owner, &u.deleted, &u.stranger});
}

void fields(IO &io, model::Reaction &r) {
    io.str(r.name);
    io.num(r.count);
    users(io, r.users);
}

void fields(IO &io, model::File &f) {
    io.str(f.id);
    io.str(f.name);
    io.str(f.mime);
    io.str(f.prettyType);
    io.str(f.path);
    io.str(f.subtype);
    io.str(f.transcript);
    io.num(f.size);
    io.num(f.width);
    io.num(f.height);
    io.num(f.durationMs);
    io.str(f.original);
    io.str(f.permalink);
    io.str(f.thumb);
    io.str(f.title);
    io.str(f.transcriptVtt);
}

void fields(IO &io, model::AttachmentField &f) {
    io.str(f.title);
    io.str(f.value);
}

void fields(IO &io, model::Block &b) {
    io.byte(reinterpret_cast<uint8_t &>(b.kind));
    io.str(b.text);
    io.str(b.image);
    io.str(b.alt);
    io.num(b.width);
    io.num(b.height);
    const size_t n = io.open(b.rows.size());
    if (io.reading())
        b.rows.resize(n);
    for (std::vector<std::string> &row : b.rows) {
        const size_t k = io.open(row.size());
        if (io.reading())
            row.resize(k);
        for (std::string &cell : row)
            io.str(cell);
        io.close();
    }
    io.close();
}

void fields(IO &io, model::Button &b) {
    io.str(b.id);
    io.str(b.label);
    io.byte(reinterpret_cast<uint8_t &>(b.style));
    io.str(b.url);
    io.str(b.blockId);
    io.str(b.value);
}

void fields(IO &io, model::Attachment &a) {
    io.str(a.color);
    io.str(a.pretext);
    io.str(a.author);
    io.str(a.title);
    io.str(a.link);
    io.str(a.text);
    io.str(a.service);
    io.str(a.favicon);
    io.str(a.footer);
    io.str(a.image);
    io.num(a.imageWidth);
    io.num(a.imageHeight);
    records(io, a.fields);
    io.flags({&a.linkPreview, &a.msgUnfurl, &a.app});
    io.num(a.id);
    io.str(a.authorIcon);
    io.str(a.channel);
    io.num(a.ts);
    records(io, a.blocks);
    records(io, a.files);
}

void fields(IO &io, model::Message &m) {
    io.num(m.ts);
    io.num(m.threadTs);
    io.num(m.latestReply);
    io.user(m.user);
    io.user(m.pinnedBy);
    io.num(m.replyCount);
    io.str(m.text);
    io.flags({&m.edited, &m.pinned, &m.saved});
    users(io, m.replyUsers);
    records(io, m.reactions);
    if (io.optional(m.extra != nullptr)) {
        model::MessageExtras &x = m.extras();
        io.open(0);
        io.str(x.subtype);
        io.str(x.botName);
        io.str(x.botAvatar);
        records(io, x.files);
        records(io, x.attachments);
        records(io, x.blocks);
        io.str(x.botId);
        records(io, x.buttons);
        // A huddle_thread's summary ("hu"); the name line follows
        // the locale when it is drawn.
        users(io, x.huddle.attendees);
        io.num(x.huddle.startSec);
        io.num(x.huddle.endSec);
        io.flags({&x.huddle.ended});
        io.close();
    }
}

void fields(IO &io, model::Conversation &c) {
    io.str(c.id);
    io.str(c.name);
    io.str(c.topic);
    io.byte(reinterpret_cast<uint8_t &>(c.kind));
    io.user(c.dmUser);
    users(io, c.members);
    io.num(c.memberCount);
    io.num(c.unread);
    io.num(c.mentions);
    io.num(c.lastRead);
    io.num(c.latest);
    io.flags({&c.starred, &c.muted, &c.member});
    io.byte(reinterpret_cast<uint8_t &>(c.notify));
    io.str(c.canvasTitle);
    io.str(c.localName);
    io.str(c.canvasId);
}

// ── Files ───────────────────────────────────────────────────────────────────

// A file of ours, parsed, if its version is ours.
bool readDoc(const std::string &path, json::Document *doc, uint64_t *hash) {
    std::string text;
    if (!file::readAll(path, &text) || text.empty())
        return false;
    if (hash)
        *hash = crypto::fnv1a(text);
    if (!doc->parse(std::move(text)) || doc->root()["v"].integer() != kVersion) {
        LOG_INFO("cache", "ignoring %s (unreadable or another version)", path.c_str());
        return false;
    }
    return true;
}

// Opens {"v":kVersion,"<key>":[ — the caller closes with endArray/endObject.
void begin(json::Writer &w, const char *key) {
    w.beginObject().key("v").value(kVersion).key(key).beginArray();
}

std::string safeName(std::string_view s) {
    std::string out(s);
    for (char &ch : out)
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
              ch == '-' || ch == '_' || ch == '.'))
            ch = '_';
    return out == "." || out == ".." ? std::string("_") : out;
}

// Our directories hold only files we wrote, no links.
int64_t treeBytes(const std::string &dir) {
    std::vector<file::DirEntry> all;
    int64_t                     n = 0;
    if (file::listDir(dir, &all))
        for (const file::DirEntry &e : all)
            n += e.isDir ? treeBytes(file::join(dir, e.name)) : e.size;
    return n;
}

void removeTree(const std::string &dir) {
    std::vector<file::DirEntry> all;
    if (file::listDir(dir, &all))
        for (const file::DirEntry &e : all) {
            const std::string p = file::join(dir, e.name);
            if (e.isDir)
                removeTree(p);
            else
                file::remove(p);
        }
    file::remove(dir);
}

} // namespace

// ── WorkspaceCache ──────────────────────────────────────────────────────────

WorkspaceCache::WorkspaceCache(plat::App &app, model::Store &store, std::string dir)
    : _app(app), _store(store), _dir(std::move(dir)), _clearGen(g_clearGen) {}

WorkspaceCache::~WorkspaceCache() {
    close(true);
}

std::string WorkspaceCache::root(plat::App &app) {
    const std::string base = identity::cacheDir(app);
    return base.empty() ? std::string() : file::join(base, "workspaces");
}

std::string WorkspaceCache::dirFor(plat::App &app, std::string_view key) {
    const std::string r = root(app);
    // "slack:T0123" → "slack_T0123" (':' can't be in a Windows path).
    return r.empty() || key.empty() ? std::string() : file::join(r, safeName(key));
}

int64_t WorkspaceCache::diskBytes(plat::App &app) {
    const std::string r = root(app);
    return r.empty() ? 0 : treeBytes(r);
}

void WorkspaceCache::clearAll(plat::App &app) {
    ++g_clearGen;
    if (const std::string r = root(app); !r.empty())
        removeTree(r);
}

void WorkspaceCache::remove(plat::App &app, std::string_view key) {
    if (const std::string d = dirFor(app, key); !d.empty())
        removeTree(d);
}

bool WorkspaceCache::writable() const {
    return !_dir.empty() && _observer && _clearGen == g_clearGen;
}

bool WorkspaceCache::load(json::Document *meta) {
    if (_observer || _dir.empty())
        return false;
    // Observe from here on, whatever was found: the backend's first answers
    // write the cache of a cold start.
    const auto observe = [this] {
        _observer = _store.observe(model::Store::kAnyConv, [this](const model::Change &ch) {
            onChange(ch);
        });
    };
    json::Document roster, users, emoji;
    uint64_t       hRoster = 0, hUsers = 0, hEmoji = 0, hMeta = 0;
    if (!readDoc(file::join(_dir, "roster.json"), &roster, &hRoster) ||
        roster.root()["c"].size() == 0) {
        observe();
        return false;
    }
    _written["roster.json"] = hRoster;
    // Users before conversations: their refs resolve to the real records.
    if (readDoc(file::join(_dir, "users.json"), &users, &hUsers)) {
        _written["users.json"] = hUsers;
        for (const json::Value rec : users.root()["u"]) {
            model::User u;
            IO          io(_store, rec);
            fields(io, u);
            if (!u.id.empty())
                _store.addUser(std::move(u));
        }
    }
    for (const json::Value rec : roster.root()["c"]) {
        model::Conversation c;
        IO                  io(_store, rec);
        fields(io, c);
        if (c.id.empty())
            continue;
        if (uint8_t(c.kind) > uint8_t(model::ConvKind::Group))
            c.kind = model::ConvKind::Channel;
        if (uint8_t(c.notify) > uint8_t(model::NotifyLevel::All))
            c.notify = model::NotifyLevel::Default;
        // Nothing newer than the read cursor: a badge kept from a merge
        // would never clear (opening the chat has nothing left to mark).
        if (c.latest && c.lastRead >= c.latest)
            c.unread = c.mentions = 0;
        _store.addConversation(std::move(c));
    }
    if (readDoc(file::join(_dir, "emoji.json"), &emoji, &hEmoji)) {
        _written["emoji.json"] = hEmoji;
        for (const json::Value e : emoji.root()["e"])
            if (!e.key().empty())
                _store.setCustomEmoji(std::string(e.key()), std::string(e.str()));
    }
    json::Document m;
    if (readDoc(file::join(_dir, "meta.json"), &m, &hMeta)) {
        _written["meta.json"] = hMeta;
        const json::Value r   = m.root();
        if (const std::string_view me = r["me"].str(); !me.empty())
            _store.me = _store.internUser(me);
        _last = std::string(r["last"].str());
        _store.myGroups.clear();
        for (const json::Value g : r["groups"])
            _store.myGroups.emplace_back(g.str());
        for (const json::Value t : r["muted"])
            if (const ConvRef c = _store.findConversation(t[0].str()); c != model::kNoConv)
                _store.setThreadMuted(c, t[1].integer(), true);
        for (const json::Value t : r["rem"])
            if (const ConvRef c = _store.findConversation(t[0].str()); c != model::kNoConv)
                _store.setReminderAt(c, t[1].integer(), t[2].integer());
        for (const json::Value t : r["ai"])
            _store.setAiTranscript(
                std::string(t.key()), std::string(t[0].str()), std::string(t[1].str())
            );
        if (meta)
            *meta = std::move(m);
    }
    _store.usersChanged();                        // one repaint for the whole roster
    _usersProfileRev  = _store.profileRevision(); // what users.json holds
    _usersPresenceRev = _store.presenceRevision();
    observe();
    LOG_INFO(
        "cache",
        "%s: %zu conversations, %zu users from the cache",
        _dir.c_str(),
        _store.conversationCount(),
        _store.userCount()
    );
    return true;
}

std::string WorkspaceCache::messagesFile(ConvRef c) const {
    return str::concat({"messages/", safeName(_store.conversation(c).id), ".json"});
}

Ts WorkspaceCache::loadMessages(ConvRef c) {
    if (_dir.empty() || c >= _store.conversationCount())
        return 0;
    if (_checked.size() <= c)
        _checked.resize(c + 1, 0);
    if (_checked[c])
        return 0;
    _checked[c] = 1;
    track(c);
    const std::string name = messagesFile(c);
    json::Document    doc;
    uint64_t          hash = 0;
    if (!readDoc(file::join(_dir, name), &doc, &hash))
        return 0;
    _written[name] = hash;
    std::vector<model::Message> page;
    for (const json::Value rec : doc.root()["m"]) {
        model::Message m;
        IO             io(_store, rec);
        fields(io, m);
        if (m.ts <= 0)
            continue;
        if (m.isReply())
            m.threadTs = 0; // a top-level list: a page must not mix in replies
        page.push_back(std::move(m));
    }
    if (page.empty())
        return 0;
    Ts newest = 0;
    for (const model::Message &m : page)
        newest = std::max(newest, m.ts);
    const bool wasEmpty = _store.conversation(c).messages.empty();
    _store.addPage(c, std::move(page));
    // The cache held the whole history: no "load older" from the top of it.
    if (wasEmpty && !doc.root()["more"].boolean(true))
        _store.updateConversation(c, [](model::Conversation &x) { x.hasMoreBefore = false; });
    return newest;
}

void WorkspaceCache::track(ConvRef c) {
    if (c >= _store.conversationCount() || tracked(c))
        return;
    if (_tracked.size() <= c)
        _tracked.resize(c + 1, 0);
    _tracked[c] = 1;
}

void WorkspaceCache::setLastConversation(ConvRef c) {
    if (c >= _store.conversationCount() || _store.conversation(c).id == _last)
        return;
    _last = _store.conversation(c).id;
    mark(kMeta);
}

void WorkspaceCache::extrasChanged() {
    mark(kMeta);
}

void WorkspaceCache::emojiChanged() {
    mark(kEmoji);
}

void WorkspaceCache::onChange(const model::Change &ch) {
    using K = model::ChangeKind;
    switch (ch.kind) {
    case K::Roster:
        mark(kConvs);
        return;
    case K::Meta:
        mark(kConvs | kMeta);
        return; // kMeta: muted threads
    case K::Users:
        // Only a profile change: presence/DND flips, emoji, user groups and
        // channel names don't change what the next start needs at once.
        if (_store.profileRevision() != _usersProfileRev)
            mark(kUsers);
        return;
    case K::Typing:
        return;
    default:
        break;
    }
    // A message list changed. Update also carries reminders (setReminderAt).
    if (ch.kind == K::Update)
        mark(kMeta);
    if (ch.thread == 0 && tracked(ch.conv)) {
        if (_msgDirty.size() <= ch.conv)
            _msgDirty.resize(ch.conv + 1, 0);
        _msgDirty[ch.conv] = 1;
        schedule();
    }
}

void WorkspaceCache::mark(uint8_t what) {
    _dirty |= what;
    schedule();
}

// At most one write per kWriteDelayMs: a throttle, not a debounce, so a
// workspace that changes every second still gets written.
void WorkspaceCache::schedule() {
    if (_timer || !writable())
        return;
    _timer = _app.addTimer(kWriteDelayMs, false, [this] {
        _timer = 0;
        flush();
    });
}

void WorkspaceCache::write(const char *name, std::string data) {
    const uint64_t h  = crypto::fnv1a(data);
    const auto     it = _written.find(name);
    if (it != _written.end() && it->second == h)
        return; // unchanged since we read or wrote it
    if (file::writeAtomic(file::join(_dir, name), data, 0600))
        _written[name] = h;
    else
        LOG_WARN("cache", "can't write %s/%s", _dir.c_str(), name);
}

void WorkspaceCache::flush() {
    if (_timer) {
        _app.cancelTimer(_timer);
        _timer = 0;
    }
    const uint8_t dirty = _dirty;
    _dirty              = 0;
    std::vector<uint8_t> msgs;
    msgs.swap(_msgDirty);
    if (!writable())
        return;
    const model::Store &s = _store;
    if (dirty & kConvs) {
        json::Writer w;
        IO           io(w, s);
        begin(w, "c");
        for (ConvRef c = 0; c < s.conversationCount(); ++c) {
            io.open(0);
            fields(io, const_cast<model::Conversation &>(s.conversation(c)));
            io.close();
        }
        w.endArray().endObject();
        write("roster.json", w.take());
    }
    if (dirty & kUsers) {
        _usersProfileRev  = s.profileRevision();
        _usersPresenceRev = s.presenceRevision();
        json::Writer w;
        IO           io(w, s);
        begin(w, "u");
        for (size_t i = 0; i < s.userCount(); ++i) {
            model::User &u = const_cast<model::User &>(s.user(UserRef(i)));
            if (u.placeholder || u.id.empty())
                continue; // known by id only: the next start interns it again
            io.open(0);
            fields(io, u);
            io.close();
        }
        w.endArray().endObject();
        write("users.json", w.take());
    }
    if (dirty & kEmoji) {
        json::Writer w;
        w.beginObject().key("v").value(kVersion).key("e").beginObject();
        for (const auto &[name, value] : s.customEmoji())
            w.key(name).value(value);
        w.endObject().endObject();
        write("emoji.json", w.take());
    }
    if (dirty & kMeta) {
        json::Writer w;
        w.beginObject().key("v").value(kVersion);
        w.key("me").value(s.user(s.me).id).key("last").value(_last);
        w.key("groups").beginArray();
        for (const std::string &g : s.myGroups)
            w.value(g);
        w.endArray();
        for (int k = 0; k < 2; ++k) { // muted threads, reminders
            w.key(k ? "rem" : "muted").beginArray();
            for (const model::Store::Mark &m : k ? s.reminders() : s.mutedThreads()) {
                if (m.conv >= s.conversationCount())
                    continue;
                w.beginArray().value(s.conversation(m.conv).id).value(int64_t(m.ts));
                if (k)
                    w.value(m.value);
                w.endArray();
            }
            w.endArray();
        }
        w.key("ai").beginObject(); // "Transcribe with AI" results
        for (const auto &[fileId, t] : s.aiTranscripts())
            w.key(fileId).beginArray().value(t.text).value(t.by).endArray();
        w.endObject();
        w.key("x").beginObject();
        if (saveExtras)
            saveExtras(w);
        w.endObject().endObject();
        write("meta.json", w.take());
    }
    for (ConvRef c = 0; c < msgs.size(); ++c) {
        if (!msgs[c] || c >= s.conversationCount())
            continue;
        // The newest kMaxMessages confirmed ones (a pending copy must not
        // come back as a ghost after a restart — old Session::cacheMessages).
        const model::Conversation          &cv = s.conversation(c);
        std::vector<const model::Message *> keep;
        bool                                older = cv.hasMoreBefore;
        for (size_t i = cv.messages.size(); i-- > 0;) {
            if (cv.messages[i].pending)
                continue;
            if (keep.size() == size_t(kMaxMessages)) {
                older = true;
                break;
            }
            keep.push_back(&cv.messages[i]);
        }
        json::Writer w;
        IO           io(w, s);
        w.beginObject().key("v").value(kVersion).key("more").value(older).key("m").beginArray();
        for (size_t i = keep.size(); i-- > 0;) {
            io.open(0);
            fields(io, const_cast<model::Message &>(*keep[i]));
            io.close();
        }
        w.endArray().endObject();
        write(messagesFile(c).c_str(), w.take());
    }
}

void WorkspaceCache::close(bool keep) {
    if (!_observer)
        return;
    // The presence dots the next start shows until its first poll.
    if (_store.profileRevision() != _usersProfileRev ||
        _store.presenceRevision() != _usersPresenceRev)
        _dirty |= kUsers;
    if (keep)
        flush();
    if (_timer) {
        _app.cancelTimer(_timer);
        _timer = 0;
    }
    _store.unobserve(_observer);
    _observer = 0;
    _dirty    = 0;
    _msgDirty.clear();
}

} // namespace cache
