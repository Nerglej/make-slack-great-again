// The reading half of the Claude Code backend: sessions and their state, the
// roster scans, transcripts into the Store, typing, history and threads, the
// timers (see backend.h).
#include "app/claude/backend.h"

#include "app/claude/backend_internal.h"
#include "app/claude/common.h"
#include "app/claude/outputs.h"
#include "app/claude/render.h"
#include "app/mrkdwn/mrkdwn.h"
#include "base/file.h"
#include "base/i18n.h"
#include "base/json.h"
#include "base/log.h"
#include "base/process.h"
#include "base/str.h"
#include "base/time.h"
#include "plat/plat.h"

#include <algorithm>
#include <cstring>
#include <utility>
#include <sys/stat.h>
#if defined(__linux__)
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace claude {

using i18n::tr;
using model::ConvRef;
using model::kNoConv;
using model::kNoUser;
using model::Message;
using model::Ts;
using model::UserRef;

// ── Shared helpers (backend_internal.h) ─────────────────────────────────────

int64_t nowMs() {
    return base::nowMicros() / 1000;
}

namespace {

struct Stat {
    bool    ok   = false;
    int64_t size = 0, mtimeMs = 0, birthMs = 0;
};

Stat statPath(const std::string &path) {
    Stat out;
    if (path.empty())
        return out;
#ifdef _WIN32
    struct _stat64 st;
    std::wstring   w(path.begin(), path.end());
    if (_wstat64(w.c_str(), &st) != 0)
        return out;
    out.ok      = true;
    out.size    = int64_t(st.st_size);
    out.mtimeMs = int64_t(st.st_mtime) * 1000;
    out.birthMs = int64_t(st.st_ctime) * 1000; // Windows: the creation time
#else
    struct stat st;
    if (::stat(path.c_str(), &st) != 0)
        return out;
    out.ok   = true;
    out.size = int64_t(st.st_size);
#if defined(__APPLE__)
    out.mtimeMs = int64_t(st.st_mtimespec.tv_sec) * 1000 + st.st_mtimespec.tv_nsec / 1'000'000;
    out.birthMs =
        int64_t(st.st_birthtimespec.tv_sec) * 1000 + st.st_birthtimespec.tv_nsec / 1'000'000;
#else
    out.mtimeMs = int64_t(st.st_mtim.tv_sec) * 1000 + st.st_mtim.tv_nsec / 1'000'000;
    out.birthMs = out.mtimeMs; // see bornMs
#endif
#endif
    return out;
}

} // namespace

int64_t sizeOf(const std::string &path) {
    const Stat s = statPath(path);
    return s.ok ? s.size : -1;
}

int64_t mtimeMs(const std::string &path) {
    return statPath(path).mtimeMs;
}

int64_t bornMs(const std::string &path) {
#if defined(__linux__) && defined(SYS_statx) && defined(STATX_BTIME)
    // The birth time tells a fork (a newer copy) from its parent; statx has
    // it on file systems that keep one.
    struct statx sx;
    if (::syscall(SYS_statx, AT_FDCWD, path.c_str(), 0, STATX_BTIME | STATX_MTIME, &sx) == 0) {
        if (sx.stx_mask & STATX_BTIME)
            return int64_t(sx.stx_btime.tv_sec) * 1000 + sx.stx_btime.tv_nsec / 1'000'000;
        return int64_t(sx.stx_mtime.tv_sec) * 1000 + sx.stx_mtime.tv_nsec / 1'000'000;
    }
#endif
    return statPath(path).birthMs;
}

std::string loginName() {
    std::string n = base::env("USER");
    if (n.empty())
        n = base::env("USERNAME");
    return n.empty() ? std::string(tr("You")) : n;
}

bool awaitsApproval(const SessionInfo &s) {
    return s.kind == SessionInfo::Kind::Background && s.awaitsApproval && s.running;
}

bool needsLogin(const SessionInfo &s) {
    return str::startsWith(s.needs, "login required");
}

namespace {
uint64_t mix(uint64_t h, std::string_view s) {
    for (unsigned char c : s)
        h = (h ^ c) * 1099511628211ull;
    return (h ^ 0xff) * 1099511628211ull;
}
uint64_t mix(uint64_t h, int64_t v) {
    return mix(h, std::string_view(reinterpret_cast<const char *>(&v), sizeof v));
}
} // namespace

uint64_t fingerprint(const Message &m) {
    uint64_t h = 1469598103934665603ull;
    h          = mix(h, m.ts);
    h          = mix(h, int64_t(m.user));
    h          = mix(h, m.threadTs);
    h          = mix(h, m.text);
    h          = mix(h, int64_t(m.replyCount));
    h          = mix(h, m.latestReply);
    h          = mix(h, int64_t(m.pending));
    for (const auto &r : m.reactions) {
        h = mix(h, r.name);
        h = mix(h, int64_t(r.count));
    }
    if (m.extra) {
        h = mix(h, m.extra->subtype);
        for (const auto &f : m.extra->files)
            h = mix(h, f.path);
        for (const auto &a : m.extra->attachments) {
            h = mix(h, a.title);
            h = mix(h, a.text);
        }
        for (const auto &b : m.extra->buttons) {
            h = mix(h, b.id);
            h = mix(h, b.label);
        }
    }
    return h;
}

size_t sharedStart(const std::vector<TranscriptItem> &a, const std::vector<TranscriptItem> &b) {
    size_t n = 0;
    while (n < a.size() && n < b.size() && a[n].ts == b[n].ts && a[n].kind == b[n].kind)
        ++n;
    return n;
}

std::string knownSessionsPath() {
    return dirs().data + "/known-sessions.json";
}

std::string profilePath() {
    return dirs().data + "/profile.json";
}

// ── Construction ────────────────────────────────────────────────────────────

Backend::Backend(model::Store &store, plat::App &app, Credentials creds)
    : model::Backend(store), _app(app), _creds(std::move(creds)), _paths(Paths::detect()),
      _team(dirs().data + "/team"), _alive(std::make_shared<bool>(true)) {
    _launcher = std::make_unique<Launcher>(_app, _creds.claudePath, _paths);
    loadKnown();
    loadProfile();
}

Backend::~Backend() {
    close();
}

void Backend::close() {
    if (!*_alive)
        return;
    saveKnown();
    *_alive = false;
    for (uint64_t *t : {&_debounce, &_watchTimer, &_safetyPoll, &_saveTimer, &_typingTimer}) {
        if (*t)
            _app.cancelTimer(*t);
        *t = 0;
    }
    for (uint64_t id : _oneShots)
        _app.cancelTimer(id);
    _oneShots.clear();
    for (auto &[id, t] : _sessions) {
        if (const auto typing = std::exchange(t->typing, {}).lock())
            typing->cancel(); // its done finds the backend gone
        t->answering.reset();
    }
}

void Backend::post(std::function<void()> fn) {
    _app.post([alive = _alive, fn = std::move(fn)] {
        if (*alive)
            fn();
    });
}

void Backend::after(int ms, std::function<void()> fn) {
    auto slot = std::make_shared<uint64_t>(0);
    *slot     = _app.addTimer(ms, false, [this, alive = _alive, slot, fn = std::move(fn)] {
        if (!*alive)
            return;
        std::erase(_oneShots, *slot);
        fn();
    });
    _oneShots.push_back(*slot);
}

Backend::Capabilities Backend::capabilities() const {
    Capabilities c;
    c.presence       = true;  // a session's dot: working vs not
    c.selfStatus     = false; // nothing to be "away" from
    c.profileContact = false; // your profile is a name and a picture, nothing more
    c.agentSessions  = true;
    c.zenMode        = true; // hides the tool-call cards
    c.slashCommands  = true; // Claude Code's own, typed to it (commands())
    // fileUpload stays off (msga's): Forward sends a message's files as links.
    return c;
}

// ── Known sessions ──────────────────────────────────────────────────────────
// Ended interactive sessions are in no Claude Code registry — only their
// transcript remains — so msga remembers the sessions it has seen, and forgets
// each one once Claude Code has dropped it.

void Backend::loadKnown() {
    std::string data;
    if (!file::readAll(knownSessionsPath(), &data))
        return;
    json::Document doc;
    if (!doc.parse(std::move(data)))
        return;
    const json::Value root = doc.root();
    for (const json::Value o : root["sessions"]) {
        const std::string convId(o["id"].str());
        std::string       sid(o["sessionId"].str());
        if (sid.empty() && !str::startsWith(convId, kNewPrefix))
            sid = convId; // written before conversation ids could differ
        if (convId.empty() || sid.empty())
            continue;
        Tracked &t        = ensureTracked(convId);
        t.info.sessionId  = sid;
        _convOf[sid]      = convId;
        t.info.name       = o["name"].str();
        t.info.cwd        = o["cwd"].str();
        t.info.kind       = o["kind"].str() == "background" ? SessionInfo::Kind::Background
                                                            : SessionInfo::Kind::Interactive;
        t.info.entrypoint = o["entrypoint"].str();
        t.transcriptPath  = o["transcript"].str();
        t.lastRead        = model::parseTs(o["lastRead"].str());
        t.localName       = o["localName"].str();
        t.standalone      = o["standalone"].boolean();
        t.role            = o["role"].str();
        t.starred         = o["starred"].boolean();
        t.muted           = o["muted"].boolean();
        t.notify          = model::NotifyLevel(std::clamp<int64_t>(o["notify"].integer(), 0, 3));
        if (str::startsWith(convId, kNewPrefix))
            _launchedHere.insert(sid); // a "+" session, remembered before `started` was
    }
    for (const json::Value v : root["started"])
        if (!v.str().empty())
            _launchedHere.insert(std::string(v.str()));
    for (const json::Value o : root["hidden"]) {
        const std::string sid(o["sessionId"].str());
        if (sid.empty() || _convOf.count(sid))
            continue;
        Hidden h;
        h.atMs       = int64_t(o["at"].number());
        h.transcript = o["transcript"].str();
        h.seenSize   = int64_t(o["size"].number(-1));
        _hidden[sid] = std::move(h);
    }
    _lastConv = root["open"].str();
}

void Backend::saveKnown() {
    json::Writer w;
    w.beginObject();
    w.key("sessions").beginArray();
    for (const auto &[convId, tp] : _sessions) {
        const Tracked &t = *tp;
        if (t.info.sessionId.empty())
            continue; // a "+" session nobody wrote to yet: nothing to come back to
        w.beginObject();
        w.key("id").value(convId);
        w.key("sessionId").value(t.info.sessionId);
        w.key("name").value(titleOf(t));
        w.key("cwd").value(t.info.cwd);
        w.key("kind").value(
            t.info.kind == SessionInfo::Kind::Background ? "background" : "interactive"
        );
        w.key("entrypoint").value(t.info.entrypoint);
        w.key("transcript").value(t.transcriptPath);
        w.key("lastRead").value(t.lastRead ? model::formatTs(t.lastRead) : std::string());
        if (!t.localName.empty())
            w.key("localName").value(t.localName);
        if (t.standalone)
            w.key("standalone").value(true); // a branch opened as a session
        if (const std::string role = roleOf(t); role != kGeneralist)
            w.key("role").value(role);
        if (t.starred)
            w.key("starred").value(true);
        if (t.muted)
            w.key("muted").value(true);
        if (t.notify != model::NotifyLevel::Default)
            w.key("notify").value(int64_t(t.notify));
        w.endObject();
    }
    w.endArray();
    w.key("hidden").beginArray();
    for (const auto &[sid, h] : _hidden) {
        // Forgotten once Claude Code has dropped the session: its transcript
        // and, for a background session, its job are both gone.
        if ((h.transcript.empty() || !file::exists(h.transcript)) &&
            !file::exists(_paths.jobsDir() + "/" + sid.substr(0, 8)))
            continue;
        w.beginObject();
        w.key("sessionId").value(sid);
        w.key("at").value(double(h.atMs));
        w.key("transcript").value(h.transcript);
        if (h.seenSize >= 0)
            w.key("size").value(double(h.seenSize));
        w.endObject();
    }
    w.endArray();
    w.key("started").beginArray();
    for (const std::string &sid : _launchedHere)
        if (_convOf.count(sid) || _hidden.count(sid) ||
            file::exists(_paths.jobsDir() + "/" + sid.substr(0, 8)))
            w.value(sid);
    w.endArray();
    if (!_lastConv.empty())
        w.key("open").value(_lastConv);
    w.endObject();
    if (!dirs().data.empty())
        file::writeAtomic(knownSessionsPath(), w.str(), 0600);
}

void Backend::scheduleSaveKnown() {
    if (!_started) {
        saveKnown();
        return;
    }
    if (_saveTimer)
        return;
    _saveTimer = _app.addTimer(2000, false, [this, alive = _alive] {
        if (!*alive)
            return;
        _saveTimer = 0;
        saveKnown();
    });
}

Backend::Tracked &Backend::ensureTracked(const std::string &convId) {
    auto it = _sessions.find(convId);
    if (it == _sessions.end()) {
        it                 = _sessions.emplace(convId, std::make_unique<Tracked>()).first;
        it->second->convId = convId;
    }
    return *it->second;
}

Backend::Tracked *Backend::find(std::string_view convId) {
    const auto it = _sessions.find(std::string(convId));
    return it == _sessions.end() ? nullptr : it->second.get();
}

const Backend::Tracked *Backend::find(std::string_view convId) const {
    const auto it = _sessions.find(std::string(convId));
    return it == _sessions.end() ? nullptr : it->second.get();
}

Backend::Tracked *Backend::findRef(ConvRef conv) {
    if (conv >= _store.conversationCount())
        return nullptr;
    return find(_store.conversation(conv).id);
}

const Backend::Tracked *Backend::findRef(ConvRef conv) const {
    if (conv >= _store.conversationCount())
        return nullptr;
    return find(_store.conversation(conv).id);
}

std::string Backend::convIdFor(const std::string &sessionId) const {
    const auto it = _convOf.find(sessionId);
    return it == _convOf.end() ? sessionId : it->second;
}

model::ConvRef Backend::lastConversation() const {
    return _lastConv.empty() ? kNoConv : _store.findConversation(_lastConv);
}

// ── Session state ───────────────────────────────────────────────────────────

bool Backend::busy(const Tracked &t) const {
    // The worker is on its way out — or will be as soon as the launch it was
    // stopped during reports back (stopRequested): no dot from the Stop on.
    if (t.stopping || t.stopRequested)
        return false;
    if (t.sending)
        return true; // msga's turn: from launching it until its end is written
    return working(t);
}

bool Backend::working(const Tracked &t) const {
    if (!t.info.running)
        return false;
    if (statusIsBusy(t.info.status))
        return true;
    // A turn that ends on a question leaves the job "blocked", and the reply's
    // turn doesn't flip it back to "working" (seen 2026-09-26: blocked for 20
    // minutes over two turns, nothing in between). A turn still open whose
    // records are newer than that status, its worker busy, is under way.
    return statusIsBusy(t.info.workerStatus) && t.parser.turnOpen() &&
           t.parser.lastActivity() / 1000 > t.info.statusSinceMs;
}

bool Backend::needsUser(const Tracked &t) const {
    return !t.sending && t.info.running && statusNeedsUser(t.info.status) && !needsLogin(t.info) &&
           !working(t);
}

std::string Backend::roleOf(const Tracked &t) const {
    if (!t.parser.role().empty())
        return t.parser.role();
    return t.role.empty() ? std::string(kGeneralist) : t.role;
}

Role Backend::roleFor(const Tracked &t) const {
    return _team.resolve(roleOf(t), t.parser.roleName());
}

model::UserRef Backend::userRef(const std::string &id) {
    return _store.internUser(id);
}

model::UserRef Backend::roleUser(const std::string &role) {
    if (role.empty() || role == kGeneralist)
        return userRef(kAgentUser);
    return userRef(kRoleUserPrefix + role);
}

model::UserRef Backend::sessionUser(const Tracked &t) {
    return userRef(kAssistantPrefix + t.convId);
}

model::UserRef Backend::subagentAuthor(const TranscriptItem &item, UserRef parent) {
    if (item.kind != TranscriptItem::Kind::Subagent)
        return parent;
    // A type that isn't a teammate ("claude", "Explore") is no teammate either
    // — not the parent's role — unless its prompt names one (a session with no
    // teammate types spawns them as "claude", see teammateNote).
    if (_team.find(item.agentType))
        return roleUser(item.agentType);
    if (_team.find(item.agentRole))
        return roleUser(item.agentRole);
    return userRef(kAgentUser);
}

std::vector<std::string> Backend::roleIds() const {
    std::vector<std::string> ids;
    for (const Role &r : _team.roles())
        ids.push_back(r.id);
    for (const auto &[id, name] : _team.formers())
        ids.push_back(id);
    return ids;
}

std::string Backend::titleOf(const Tracked &t) const {
    if (!t.info.name.empty())
        return teammateNames(withoutTeammateNote(t.info.name));
    if (!t.parser.aiTitle().empty())
        return teammateNames(t.parser.aiTitle());
    if (!t.info.cwd.empty())
        return std::string(file::baseName(t.info.cwd));
    return t.info.sessionId.substr(0, 8);
}

std::string Backend::shownTitle(const Tracked &t) const {
    return t.localName.empty() ? titleOf(t) : t.localName;
}

// A title is plain text, so a teammate mention it echoes from the prompt
// ("@claude:role:engineer") can't be a pill: name the teammate instead.
std::string Backend::teammateNames(std::string_view text) const {
    std::string out;
    size_t      i = 0, last = 0;
    const auto  idChar = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    };
    const auto wordChar = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_';
    };
    while ((i = text.find("claude:", i)) != std::string_view::npos) {
        // <@claude:…> or a bare @claude:… not inside a word, path or address.
        const bool token = i >= 2 && text[i - 1] == '@' && text[i - 2] == '<';
        const bool bare  = i >= 1 && text[i - 1] == '@' && !token &&
                           (i < 2 || !(wordChar(text[i - 2]) || std::strchr("@/:.-", text[i - 2])));
        if (!token && !bare) {
            i += 7;
            continue;
        }
        size_t      j = i + 7;
        std::string role;
        if (text.substr(j, 5) == "agent") {
            role = kGeneralist;
            j += 5;
        } else if (text.substr(j, 5) == "role:") {
            j += 5;
            const size_t from = j;
            while (j < text.size() && idChar(text[j]))
                ++j;
            if (j == from) {
                i = j;
                continue;
            }
            role = std::string(text.substr(from, j - from));
        } else {
            i += 7;
            continue;
        }
        if (token ? (j >= text.size() || text[j] != '>')
                  : (j < text.size() && (wordChar(text[j]) || text[j] == '-'))) {
            i = j;
            continue;
        }
        const size_t start = token ? i - 2 : i - 1;
        out.append(text.substr(last, start - last));
        out += '@';
        out += _team.resolve(role).name;
        last = token ? j + 1 : j;
        i    = last;
    }
    if (last == 0)
        return std::string(text);
    out.append(text.substr(last));
    return out;
}

std::string Backend::readOnlyReason(const Tracked &t) const {
    if (t.info.running && t.info.kind == SessionInfo::Kind::Interactive) {
        if (t.info.entrypoint == "cli")
            return tr(
                "This session is running in a terminal \xE2\x80\x94 reply there. Once it "
                "ends, you can continue it here."
            );
        return tr(
            "Another program is driving this session. Once it ends, you can continue it "
            "here."
        );
    }
    if (_creds.claudePath.empty())
        return notInstalledMessage();
    return {};
}

// Not working, yet waiting on something besides a message: open in a
// terminal, or on a permission question (messages wait for its answer).
bool Backend::unavailable(const Tracked &t) const {
    return awaitsApproval(t.info) || !readOnlyReason(t).empty();
}

bool Backend::roleBusy(const std::string &role) const {
    for (const auto &[id, t] : _sessions)
        if (!asThread(*t) && busy(*t) && roleOf(*t) == role)
            return true;
    return roleSubagentRunning(role);
}

// A subagent started as the teammate running in a live session — the same
// one pumpTyping has thinking in its thread. A plain one ("claude",
// "Explore") speaks as the Generalist, yet isn't one: it lights no dot.
bool Backend::roleSubagentRunning(const std::string &role) const {
    if (role.empty() || role == kGeneralist)
        return false;
    for (const auto &[id, tp] : _sessions) {
        const Tracked &t = *tp;
        if (!t.info.running || t.stopping || asThread(t))
            continue;
        for (const auto &item : t.parser.items()) {
            if (item.kind != TranscriptItem::Kind::Subagent || item.agentId.empty())
                continue;
            const bool asRole = _team.find(item.agentType)
                                    ? item.agentType == role
                                    : _team.find(item.agentRole) && item.agentRole == role;
            if (asRole && subagentRunSinceMs(t, item.agentId))
                return true;
        }
    }
    return false;
}

// A teammate is yellow while none of its sessions works and one of them is
// yellow itself (see assistantUser) — green, as any working one makes it, wins.
bool Backend::roleUnavailable(const std::string &role) const {
    if (roleBusy(role))
        return false;
    for (const auto &[id, t] : _sessions)
        if (!asThread(*t) && roleOf(*t) == role && unavailable(*t))
            return true;
    return false;
}

model::User Backend::meUser() const {
    model::User u;
    u.id          = kMeId;
    u.name        = _myName.empty() ? loginName() : _myName;
    u.displayName = u.name;
    if (!_myAvatarPath.empty() && file::exists(_myAvatarPath))
        u.avatar = _myAvatarPath;
    u.active = true;
    return u;
}

model::User Backend::assistantUser(const Tracked &t) const {
    model::User u;
    u.id          = kAssistantPrefix + t.convId;
    u.name        = shownTitle(t);
    u.displayName = u.name;
    u.avatar      = roleFor(t).avatar;
    u.title       = homeRelative(t.info.cwd); // shown on the profile card
    u.active      = busy(t);
    u.unavailable = !u.active && unavailable(t);
    if (needsUser(t))
        u.statusText = tr("Waiting for you");
    else if (!t.sending && t.info.running && needsLogin(t.info))
        u.statusText = tr("Not logged in");
    else if (busy(t))
        u.statusText = tr("Working");
    else if (t.info.running && statusHasShell(t.info.status))
        u.statusText = tr("Running a background command");
    return u;
}

model::User Backend::teammateUser(const Role &r) const {
    model::User u;
    u.id          = r.id.empty() || r.id == kGeneralist ? std::string(kAgentUser)
                                                        : std::string(kRoleUserPrefix) + r.id;
    u.name        = r.name;
    u.displayName = r.name;
    u.avatar      = r.avatar;
    u.title       = r.description; // shown on the profile card
    u.active      = roleBusy(r.id);
    u.unavailable = roleUnavailable(r.id);
    return u;
}

// ── Transcripts ─────────────────────────────────────────────────────────────

void Backend::tail(Tracked &t) {
    if (t.info.sessionId.empty())
        return;
    if (t.transcriptPath.empty() || !file::exists(t.transcriptPath)) {
        const std::string found = _paths.findTranscript(t.info.sessionId);
        if (found.empty())
            return;
        t.transcriptPath = found;
    }
    const int64_t size = sizeOf(t.transcriptPath);
    if (size < 0)
        return;
    if (size < t.offset) {
        // Rewritten or truncated: start over.
        t.parser = {};
        t.offset = 0;
        t.rendered.clear();
    }
    if (size == t.offset)
        return;
    std::string all;
    if (!file::readAll(t.transcriptPath, &all) || int64_t(all.size()) <= t.offset)
        return;
    t.parser.feed(std::string_view(all).substr(size_t(t.offset)));
    t.offset = int64_t(all.size());
    // A role this machine doesn't know (another machine's teammate, a deleted
    // file) keeps the name its sessions give it.
    if (_team.noteFormer(t.parser.role(), t.parser.roleName()))
        putUser(teammateUser(_team.resolve(t.parser.role())));
}

const Backend::SubagentCount &
Backend::subagentStats(const Tracked &t, const std::string &agentId) const {
    const std::string path = Paths::subagentTranscript(t.transcriptPath, agentId);
    const int64_t     size = sizeOf(path);
    auto             &c    = _subagentCounts[path];
    if (c.size != size) {
        c.size = size;
        std::string bytes;
        if (size >= 0 && file::readAll(path, &bytes)) {
            TranscriptParser p;
            p.feed(bytes);
            c.count    = int(p.items().size());
            c.zenCount = int(std::count_if(p.items().begin(), p.items().end(), [](const auto &i) {
                return i.kind != TranscriptItem::Kind::ToolGroup;
            }));
            c.latest   = p.items().empty() ? 0 : p.items().back().ts;
            c.activity = p.activity();
        } else {
            c.count    = 0;
            c.zenCount = 0;
            c.latest   = 0;
            c.activity.clear();
        }
    }
    return c;
}

int Backend::subagentReplyCount(const Tracked &t, const std::string &agentId, Ts *latest) const {
    const SubagentCount &c = subagentStats(t, agentId);
    *latest                = c.latest;
    return _zen ? c.zenCount : c.count;
}

// A background subagent runs from its first record after it last stopped:
// the session is notified each time it stops, a few ms after its last record,
// and it may start again (a reply relayed to it, or on its own when work of
// its own ends) — then its file grows past that notification.
int64_t Backend::subagentRunSinceMs(const Tracked &t, const std::string &agentId) const {
    const auto   &activity = subagentStats(t, agentId).activity;
    const int64_t stopped  = t.parser.taskStoppedAt(agentId);
    if (activity.empty() || activity.back() <= stopped)
        return 0;
    auto it = activity.end();
    while (it != activity.begin() && *(it - 1) > stopped)
        --it;
    return *it / 1000;
}

void Backend::attachOutputs(
    Message                           &m,
    const std::vector<TranscriptItem> &items,
    size_t                             i,
    const std::string                 &convId,
    const std::string                 &cwd,
    const std::string                 &keyPrefix
) const {
    const TranscriptItem &item = items[i];
    if (item.kind != TranscriptItem::Kind::AssistantText)
        return;
    OutputContext ctx;
    ctx.convId     = convId;
    ctx.messageKey = keyPrefix + (item.uuid.empty() ? model::formatTs(item.ts) : item.uuid);
    ctx.cwd        = cwd;
    ctx.date       = item.date;
    ctx.turnStart  = items.front().date;
    for (size_t j = i; j-- > 0;)
        if (items[j].kind == TranscriptItem::Kind::UserPrompt) {
            ctx.turnStart = items[j].date;
            break;
        }
    for (model::File &f : outputFiles(item.text, ctx))
        m.extras().files.push_back(std::move(f));
}

const Message &Backend::renderedAt(Tracked &t, size_t i) {
    const auto   &items  = t.parser.items();
    const UserRef author = roleUser(roleOf(t));
    if (author != t.renderedAuthor) {
        t.rendered.clear(); // the role became known: Claude's messages change hands
        t.renderedAuthor = author;
    }
    if (t.rendered.size() > items.size())
        t.rendered.resize(items.size());
    // A subagent started as a teammate is that teammate's thread; an answer
    // carries the files it made.
    const UserRef me     = _store.me;
    const auto    render = [&](size_t j) {
        Message m = toMessage(items[j], me, subagentAuthor(items[j], author));
        attachOutputs(m, items, j, t.convId, t.info.cwd);
        return m;
    };
    while (t.rendered.size() <= i) {
        const size_t j = t.rendered.size();
        t.rendered.emplace_back(items[j], render(j));
    }
    if (!(t.rendered[i].first == items[i]))
        t.rendered[i] = {items[i], render(i)};
    return t.rendered[i].second;
}

// A subagent's report, handed back to the session: its thread already ends
// with it (the subagent's own transcript), so here it's a line from the
// subagent pointing there instead of the report again.
Message
Backend::handbackPointer(const Tracked &t, const TranscriptItem &subagent, Ts ts, UserRef parent) {
    Message m;
    m.ts                   = ts;
    m.user                 = subagentAuthor(subagent, parent);
    m.extras().subtype     = kProgressSubtype; // what the session says next notifies
    const std::string what = escapeMrkdwn(
        subagent.text.empty() && !subagent.tools.empty() ? subagent.tools.front().name
                                                         : subagent.text
    );
    const std::string link = str::concat(
        {"<",
         mrkdwn::threadLink(t.convId, model::formatTs(subagent.ts)),
         "|",
         tr("its thread"),
         ">"}
    );
    m.text =
        str::concat({"_", i18n::arg(tr("Subagent “%1” reported back in %2."), what, link), "_"});
    return m;
}

std::vector<Message> Backend::visibleMessages(Tracked &t) {
    tail(t);
    const auto                             &items     = t.parser.items();
    const UserRef                           assistant = roleUser(roleOf(t));
    const bool                              isBusy    = busy(t);
    // The user's replies in subagent threads, relayed through this session:
    // replies in the thread, each counted on its root with msga's copies.
    std::unordered_map<std::string, Ts>     subagentRoot; // by agentId
    std::unordered_map<std::string, size_t> subagentItem; // by agentId
    std::map<Ts, int>                       relayed;      // by root
    std::map<Ts, Ts>                        relayedLatest;
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i].kind == TranscriptItem::Kind::Subagent && !items[i].agentId.empty()) {
            subagentRoot[items[i].agentId] = items[i].ts;
            subagentItem[items[i].agentId] = i;
        }
    const auto noteRelay = [&](Ts root, Ts ts) {
        ++relayed[root];
        relayedLatest[root] = std::max(relayedLatest[root], ts);
    };
    for (const auto &item : items)
        if (!item.relayTo.empty())
            if (const auto r = subagentRoot.find(item.relayTo); r != subagentRoot.end())
                noteRelay(r->second, item.ts);
    if (t.flying && t.flying->threadRoot)
        noteRelay(t.flying->threadRoot, t.flying->ts);
    for (const auto &o : t.outbox)
        if (o.threadRoot)
            noteRelay(o.threadRoot, o.ts);
    std::vector<Message> out;
    out.reserve(items.size() + 1);
    for (size_t i = 0; i < items.size(); ++i) {
        const auto &item = items[i];
        if (!isVisible(item, isBusy) || (_zen && item.kind == TranscriptItem::Kind::ToolGroup))
            continue;
        Message m = renderedAt(t, i).clone();
        if (item.kind == TranscriptItem::Kind::Subagent && !item.agentId.empty()) {
            Ts latest = 0;
            m.replyCount =
                uint32_t(subagentReplyCount(t, item.agentId, &latest) + relayed[item.ts]);
            latest = std::max(latest, relayedLatest[item.ts]);
            if (latest)
                m.latestReply = latest;
        }
        if (!item.relayTo.empty())
            if (const auto r = subagentRoot.find(item.relayTo); r != subagentRoot.end())
                m.threadTs = r->second;
        if (item.kind == TranscriptItem::Kind::PeerMessage)
            if (const auto r = subagentItem.find(item.agentId); r != subagentItem.end())
                m = handbackPointer(t, items[r->second], m.ts, assistant);
        out.push_back(std::move(m));
    }

    // A session stopped for the user where the transcript doesn't show it — a
    // terminal session waiting at a prompt, a background one on a permission
    // prompt — gets a message of its own, so it notifies and badges like an
    // answer. (A background session that asked a question already shows the
    // question as its last answer.) Its ts is pinned to when the status
    // changed, so it stays the same message while the session waits.
    if (const Ts waitTs = waitingTs(t, out.empty() ? 0 : out.back().ts))
        out.push_back(waitingMessage(t, waitTs));

    appendOutgoing(t, out, 0);

    // Side conversations branched off this session (/btw): each one's first
    // prompt is a message here, rooting a thread with the rest of it.
    bool branched = false;
    for (auto &[id, fp] : _sessions) {
        Tracked &f = *fp;
        if (f.forkOf != t.convId || !asThread(f))
            continue;
        const auto replies = threadMessages(f);
        if (f.forkAt < 0)
            continue;
        Message root    = renderedAt(f, size_t(f.forkAt)).clone();
        root.replyCount = uint32_t(replies.size());
        if (!replies.empty())
            root.latestReply = replies.back().ts;
        out.push_back(std::move(root));
        branched = true;
    }
    if (branched)
        std::stable_sort(out.begin(), out.end(), [](const Message &a, const Message &b) {
            return a.ts < b.ts;
        });
    applyReactions(t.convId, out);
    return out;
}

// "Waiting for you in the terminal." / "Waiting for your approval: …" at ts
// (waitingTs), with the approval's options as buttons once read.
Message Backend::waitingMessage(const Tracked &t, Ts ts) {
    const bool    inTerminal = terminalWaits(t);
    const UserRef assistant  = roleUser(roleOf(t));
    Message       m;
    m.ts                         = ts;
    m.user                       = assistant;
    const std::string_view needs = str::startsWith(t.info.needs, "approve ")
                                       ? std::string_view(t.info.needs).substr(8)
                                       : std::string_view(t.info.needs);
    m.text = inTerminal ? std::string(tr("Waiting for you in the terminal."))
                        : i18n::arg(tr("Waiting for your approval: %1"), escapeMrkdwn(needs));
    if (!inTerminal) {
        // Its options, once read off the screen (readApproval), as
        // buttons; until then a note that msga is still at it — never a
        // terminal.
        const bool read = t.approvalNeeds == t.info.needs && !t.approvalOptions.empty();
        if (read) {
            for (const auto &o : t.approvalOptions) {
                model::Button b;
                b.id    = "option:" + str::number(int64_t(o.number));
                b.label = o.label;
                if (str::startsWith(o.label, "Yes"))
                    b.style = model::Button::Style::Primary;
                else if (str::startsWith(o.label, "No"))
                    b.style = model::Button::Style::Danger;
                m.extras().buttons.push_back(std::move(b));
            }
        } else if (
            t.answering.expired() && t.approvalReads >= kApprovalReads && !t.approvalAnswered
        ) {
            m.text += "\n";
            m.text += tr("Its options couldn't be read yet; trying again.");
        }
    }
    return m;
}

bool Backend::newsAt(Tracked &t, bool thread, Ts ts, Message *out) {
    const auto   &items = t.parser.items();
    const UserRef owner = roleUser(roleOf(t));
    const size_t  from  = thread ? size_t(std::max(t.forkAt, 0)) + 1 : 0;
    for (size_t i = from; i < items.size(); ++i) {
        if (items[i].ts != ts)
            continue;
        *out = toMessage(items[i], _store.me, subagentAuthor(items[i], owner));
        if (thread)
            out->threadTs = t.forkRoot;
        return true;
    }
    // Not an item: the session's "Waiting for…" (seenOf's only other news).
    if (thread || (!terminalWaits(t) && !awaitsApproval(t.info)))
        return false;
    *out = waitingMessage(t, ts);
    return true;
}

bool Backend::terminalWaits(const Tracked &t) const {
    return t.info.running && t.info.kind == SessionInfo::Kind::Interactive && needsUser(t);
}

// The ts of "Waiting for you…" (visibleMessages), 0 when it doesn't wait:
// pinned to when the status changed, after everything before it (lastTs).
Ts Backend::waitingTs(const Tracked &t, Ts lastTs) const {
    if (!terminalWaits(t) && !awaitsApproval(t.info))
        return 0;
    const Ts micros = t.info.statusSinceMs * 1000;
    return micros <= lastTs ? lastTs + 1 : micros;
}

// What visibleMessages (thread: threadMessages) shows, as sync() takes it —
// each message's ts, whose and what kind — read off the parser's items alone.
// It must keep in step with those two, item for item.
std::map<Ts, Backend::Seen> Backend::seenOf(Tracked &t, bool thread) {
    std::map<Ts, Tracked::Seen> out;
    tail(t);
    const auto &items  = t.parser.items();
    const bool  isBusy = busy(t);
    const auto  shown  = [&](const TranscriptItem &item) {
        return isVisible(item, isBusy) && !(_zen && item.kind == TranscriptItem::Kind::ToolGroup);
    };
    // toMessage: prompts are mine; tool cards, subagents, remarks and what
    // other sessions or subagents said to it (hand-backs) progress.
    const auto seen = [](const TranscriptItem &item, Ts threadTs) {
        Tracked::Seen s;
        s.mine     = item.kind == TranscriptItem::Kind::UserPrompt;
        s.progress = item.kind == TranscriptItem::Kind::ToolGroup ||
                     item.kind == TranscriptItem::Kind::Subagent ||
                     item.kind == TranscriptItem::Kind::PeerMessage ||
                     (item.kind == TranscriptItem::Kind::AssistantText &&
                      item.state == TranscriptItem::State::Progress);
        s.thread   = threadTs;
        return s;
    };
    const auto outgoing = [&](Ts threadRoot) {
        const auto add = [&](const Tracked::Outgoing &o) {
            out[o.ts] = {true, false, o.threadRoot ? o.threadRoot : threadRoot};
        };
        if (t.flying)
            add(*t.flying);
        for (const auto &o : t.outbox)
            add(o);
    };
    if (thread) {
        if (t.forkAt < 0)
            return out;
        for (size_t i = size_t(t.forkAt) + 1; i < items.size(); ++i)
            if (shown(items[i]))
                out[items[i].ts] = seen(items[i], t.forkRoot);
        outgoing(t.forkRoot);
        return out;
    }
    std::unordered_map<std::string, Ts> subagentRoot; // by agentId
    for (const auto &item : items)
        if (item.kind == TranscriptItem::Kind::Subagent && !item.agentId.empty())
            subagentRoot[item.agentId] = item.ts;
    Ts lastTs = 0;
    for (const auto &item : items) {
        if (!shown(item))
            continue;
        Ts threadTs = 0;
        if (!item.relayTo.empty())
            if (const auto r = subagentRoot.find(item.relayTo); r != subagentRoot.end())
                threadTs = r->second;
        out[item.ts] = seen(item, threadTs);
        lastTs       = item.ts;
    }
    if (const Ts waitTs = waitingTs(t, lastTs))
        out[waitTs] = {};
    outgoing(0);
    for (auto &[id, fp] : _sessions) {
        Tracked &f = *fp;
        if (f.forkOf != t.convId || !asThread(f))
            continue;
        tail(f);
        if (f.forkAt >= 0)
            out[f.parser.items()[size_t(f.forkAt)].ts] = {true, false, 0}; // its first prompt
    }
    return out;
}

std::vector<Message> Backend::threadMessages(Tracked &f) {
    tail(f);
    std::vector<Message> out;
    if (f.forkAt < 0)
        return out;
    const auto &items  = f.parser.items();
    const bool  isBusy = busy(f);
    for (size_t i = size_t(f.forkAt) + 1; i < items.size(); ++i) {
        const auto &item = items[i];
        if (!isVisible(item, isBusy) || (_zen && item.kind == TranscriptItem::Kind::ToolGroup))
            continue;
        Message m  = renderedAt(f, i).clone();
        m.threadTs = f.forkRoot;
        out.push_back(std::move(m));
    }
    appendOutgoing(f, out, f.forkRoot);
    applyReactions(f.forkOf, out);
    return out;
}

// The root is the Subagent message; the replies are its own transcript: the
// prompt written by the session's assistant, the rest said by the subagent
// (the teammate it was started as, else the assistant too), and the user's
// replies relayed to it through the session.
std::vector<Message> Backend::subagentThread(Tracked &t, Ts root) {
    std::vector<Message> out;
    const auto           msgs      = visibleMessages(t);
    const UserRef        assistant = roleUser(roleOf(t));
    std::string          agentId;
    UserRef              subagent = assistant;
    for (const auto &item : t.parser.items())
        if (item.ts == root) {
            agentId  = item.agentId;
            subagent = subagentAuthor(item, assistant);
        }
    for (const auto &m : msgs)
        if (m.threadTs == root)
            out.push_back(m.clone());
    std::string bytes;
    if (!agentId.empty() &&
        file::readAll(Paths::subagentTranscript(t.transcriptPath, agentId), &bytes)) {
        TranscriptParser p;
        p.feed(bytes);
        const auto &items = p.items();
        for (size_t i = 0; i < items.size(); ++i) {
            const auto &item = items[i];
            if (_zen && item.kind == TranscriptItem::Kind::ToolGroup)
                continue;
            Message m = toMessage(item, assistant, subagent);
            attachOutputs(m, items, i, t.convId, t.info.cwd, agentId + "-");
            m.threadTs = root;
            out.push_back(std::move(m));
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const Message &a, const Message &b) {
        return a.ts < b.ts;
    });
    // Two transcripts, one list: a relayed reply and a subagent record in the
    // same microsecond would share a ts.
    for (size_t i = 1; i < out.size(); ++i)
        if (out[i].ts <= out[i - 1].ts)
            out[i].ts = out[i - 1].ts + 1;
    applyReactions(t.convId, out);
    return out;
}

void Backend::appendOutgoing(const Tracked &t, std::vector<Message> &out, Ts threadRoot) const {
    const auto add = [&](const Tracked::Outgoing &o) {
        TranscriptItem item;
        item.kind  = TranscriptItem::Kind::UserPrompt;
        item.ts    = o.ts;
        item.date  = o.ts;
        item.text  = takeAttachments(o.shown.empty() ? o.text : o.shown, &item.images);
        Message m  = toMessage(item, _store.me, _store.me);
        m.threadTs = o.threadRoot ? o.threadRoot : threadRoot;
        out.push_back(std::move(m));
    };
    if (t.flying)
        add(*t.flying);
    for (const auto &o : t.outbox)
        add(o);
}

bool Backend::isOutgoingCopy(const Tracked &t, Ts ts) const {
    return (t.flying && t.flying->ts == ts) ||
           std::any_of(t.outbox.begin(), t.outbox.end(), [&](const Tracked::Outgoing &o) {
               return o.ts == ts;
           });
}

void Backend::applyReactions(const std::string &convId, std::vector<Message> &msgs) const {
    const auto byTs = _reactions.find(convId);
    if (byTs == _reactions.end())
        return;
    for (auto &m : msgs)
        if (const auto r = byTs->second.find(m.ts); r != byTs->second.end())
            m.reactions = r->second;
}

// ── Branched sessions (/btw threads) ────────────────────────────────────────
// A fork starts with a copy of its parent's records — same timestamps, so the
// same leading items — and goes on with its own. Nothing in it names the
// parent, so the family is read off the transcripts: sessions whose first
// item is the same are one family, and each younger member is a fork of the
// older one it shares the longest start with. The thread's root is the fork's
// first prompt after that shared start.

bool Backend::asThread(const Tracked &t) const {
    return !t.forkOf.empty() && !t.standalone && t.forkOf != t.convId && find(t.forkOf);
}

Backend::Tracked *Backend::forkFor(const std::string &parentConv, Ts root) {
    for (auto &[id, fp] : _sessions) {
        Tracked &f = *fp;
        if (f.forkOf == parentConv && f.forkAt >= 0 && f.forkRoot == root && asThread(f))
            return &f;
    }
    return nullptr;
}

const Backend::Tracked *Backend::forkFor(const std::string &parentConv, Ts root) const {
    return const_cast<Backend *>(this)->forkFor(parentConv, root);
}

std::unordered_set<std::string> Backend::detectForks() {
    std::map<std::string, std::vector<Tracked *>> families; // by first item
    for (auto &[id, tp] : _sessions) {
        Tracked    &t     = *tp;
        const auto &items = t.parser.items();
        if (t.info.sessionId.empty() || items.empty() || t.transcriptPath.empty())
            continue;
        families[model::formatTs(items.front().ts) + "\n" + items.front().text].push_back(&t);
    }
    std::unordered_set<std::string> changed; // parents whose threads changed
    for (auto &[key, family] : families) {
        if (family.size() < 2)
            continue;
        std::vector<std::pair<int64_t, Tracked *>> byAge;
        for (Tracked *t : family)
            byAge.emplace_back(bornMs(t->transcriptPath), t);
        std::stable_sort(byAge.begin(), byAge.end(), [](const auto &a, const auto &b) {
            return a.first < b.first;
        });
        for (size_t i = 1; i < byAge.size(); ++i) {
            Tracked &f      = *byAge[i].second;
            Tracked *parent = nullptr;
            size_t   best   = 0;
            for (size_t j = 0; j < i; ++j) {
                const size_t n = sharedStart(byAge[j].second->parser.items(), f.parser.items());
                if (!parent || n > best) {
                    parent = byAge[j].second;
                    best   = n;
                }
            }
            const auto &items = f.parser.items();
            int         at    = -1;
            for (size_t k = best; k < items.size(); ++k)
                if (items[k].kind == TranscriptItem::Kind::UserPrompt) {
                    at = int(k);
                    break;
                }
            const Ts root = at >= 0 ? items[size_t(at)].ts : 0;
            if (f.forkOf != parent->convId || f.forkAt != at || f.forkRoot != root) {
                if (!f.forkOf.empty())
                    changed.insert(f.forkOf);
                changed.insert(parent->convId);
            }
            f.forkOf   = parent->convId;
            f.forkAt   = at;
            f.forkRoot = root;
        }
    }
    return changed;
}

std::string Backend::subagentOf(const Tracked &t, Ts root) const {
    for (const auto &item : t.parser.items())
        if (item.ts == root && item.kind == TranscriptItem::Kind::Subagent)
            return item.agentId;
    return {};
}

bool Backend::threadAcceptsReplies(ConvRef conv, Ts root) const {
    const Tracked *t = findRef(conv);
    if (!t)
        return false;
    if (forkFor(t->convId, root))
        return true;
    return !subagentOf(*t, root).empty();
}

// A /btw thread is the user's: its answers notify as replies to them.
bool Backend::threadFollowed(ConvRef conv, Ts root) const {
    const Tracked *t = findRef(conv);
    return t && forkFor(t->convId, root) != nullptr;
}

bool Backend::threadOpensAsSession(ConvRef conv, Ts root) const {
    const Tracked *t = findRef(conv);
    return t && forkFor(t->convId, root) != nullptr;
}

model::ConvRef Backend::openThreadAsSession(ConvRef conv, Ts root) {
    Tracked *p = findRef(conv);
    Tracked *f = p ? forkFor(p->convId, root) : nullptr;
    if (!f)
        return kNoConv;
    f->standalone    = true;
    f->announcedInit = false; // re-taken as a conversation of its own, silently
    sync(*f);
    syncMeta(*f); // the session joins the list
    sync(*p);     // the thread's root leaves the parent
    syncMeta(*p);
    saveKnown();
    return f->ref;
}

// ── Into the Store ──────────────────────────────────────────────────────────

void Backend::sync(Tracked &t) {
    // A branched session's news is its thread's, in its parent's conversation.
    const bool thread = asThread(t);
    if (thread != t.announcedAsThread) {
        t.announcedAsThread = thread;
        t.announcedInit     = false; // what was announced belongs to the other place
    }
    Tracked      *parent = thread ? find(t.forkOf) : nullptr;
    // Where it shows: the conversation's own list, or its parent's thread.
    const ConvRef target = thread ? (parent ? parent->ref : kNoConv) : t.ref;
    // A list the Store doesn't hold needs no messages, only what they are
    // (seenOf): no markdown rendered, no answer's files copied — the first
    // scan looks at every session, the UI opens a few.
    const bool    loaded = target != kNoConv && _shown.count({target, thread ? t.forkRoot : 0});
    std::vector<Message>        msgs;
    std::map<Ts, Tracked::Seen> now;
    const auto                  take = [&] {
        if (!loaded) {
            now = seenOf(t, thread);
            return;
        }
        msgs = thread ? threadMessages(t) : visibleMessages(t);
        now.clear();
        for (const auto &m : msgs)
            now[m.ts] = {m.user == _store.me, m.subtype() == kProgressSubtype, m.threadTs};
    };
    take();
    // The message on its way is delivered once its prompt is in the
    // transcript: msga's copy of it goes in the same breath.
    if (t.flying && t.announcedInit && std::any_of(now.begin(), now.end(), [&](const auto &n) {
            return n.second.mine && !t.announced.count(n.first) && !isOutgoingCopy(t, n.first);
        })) {
        t.flying.reset();
        take();
    }
    bool            sawOwnPrompt = false;
    std::vector<Ts> fresh; // news in a list the Store doesn't hold
    if (!t.announcedInit) {
        // First look at this session: its history is served by loadHistory,
        // nothing here is news.
        t.announced     = std::move(now);
        t.announcedInit = true;
        if (!thread && t.lastRead == 0)
            for (auto it = t.announced.rbegin(); it != t.announced.rend(); ++it)
                if (!it->second.thread) {
                    t.lastRead = it->first; // don't greet a new session with old unreads
                    break;
                }
    } else {
        for (const auto &[ts, seen] : now) {
            if (t.announced.count(ts))
                continue;
            if (seen.mine && !isOutgoingCopy(t, ts))
                sawOwnPrompt = true;
            // News in a list the Store doesn't hold (msga's EvMessageNew for
            // every session): an answer, a "Waiting for…" — announced on
            // its own, so it notifies. Opening the list later serves it as
            // history (its fingerprint taken then), never again as news.
            else if (!loaded && !seen.mine && !seen.progress && target != kNoConv)
                fresh.push_back(ts);
        }
        t.announced = std::move(now);
    }
    // A /btw is delivered once the branch's first prompt — the thread's root,
    // shown in the parent — is in its transcript.
    if (thread && t.awaitingRoot && t.forkAt >= 0) {
        t.awaitingRoot = false;
        sawOwnPrompt   = true;
    }
    // The turn msga started is under way once its prompt is in the transcript.
    if (sawOwnPrompt && t.sending && !t.promptLanded) {
        t.promptLanded   = true;
        t.promptLandedMs = nowMs();
        if (auto done = std::exchange(t.inFlight, {}))
            done(true, {});
    }

    // The Store's lists: the conversation's (once its history was loaded) and
    // its loaded threads.
    if (!loaded) {
        for (const Ts ts : fresh) {
            Message m;
            if (newsAt(t, thread, ts, &m))
                _store.announceReply(target, m);
        }
        return;
    }
    const auto syncList = [&](Ts list, std::vector<Message> want) {
        const auto key = std::make_pair(target, list);
        const auto it  = _shown.find(key);
        if (it == _shown.end())
            return; // not loaded: the Store doesn't hold it
        Shown                        &s = it->second;
        std::map<Ts, const Message *> byTs;
        for (const auto &m : want)
            if (m.ts >= s.from)
                byTs[m.ts] = &m;
        for (auto f = s.fp.begin(); f != s.fp.end();) {
            if (byTs.count(f->first)) {
                ++f;
                continue;
            }
            _store.removeMessage(target, f->first);
            f = s.fp.erase(f);
        }
        for (const auto &[ts, m] : byTs) {
            const uint64_t fp  = fingerprint(*m);
            const auto     old = s.fp.find(ts);
            if (old == s.fp.end()) {
                s.fp[ts] = fp;
                _store.addMessage(target, m->clone());
            } else if (old->second != fp) {
                old->second  = fp;
                Message copy = m->clone();
                _store.updateMessage(target, ts, [&](Message &x) { x = std::move(copy); });
            }
        }
    };
    if (thread) {
        syncList(t.forkRoot, std::move(msgs));
        return;
    }
    // Replies relayed to a subagent belong to its thread (subagentThread).
    std::vector<Message> top;
    top.reserve(msgs.size());
    for (auto &m : msgs)
        if (!m.threadTs)
            top.push_back(std::move(m));
    syncList(0, std::move(top));
}

// A loaded subagent thread follows its transcript: rebuilt whenever its root
// (reply count, newest reply) changed.
void Backend::syncThreads(Tracked &t) {
    if (t.ref == kNoConv || asThread(t))
        return;
    for (auto &[key, shown] : _shown) {
        if (key.first != t.ref || key.second == 0 || forkFor(t.convId, key.second))
            continue;
        const Ts                      root = key.second;
        const auto                    want = subagentThread(t, root);
        std::map<Ts, const Message *> byTs;
        for (const auto &m : want)
            byTs[m.ts] = &m;
        for (auto f = shown.fp.begin(); f != shown.fp.end();) {
            if (byTs.count(f->first)) {
                ++f;
                continue;
            }
            _store.removeMessage(t.ref, f->first);
            f = shown.fp.erase(f);
        }
        for (const auto &[ts, m] : byTs) {
            const uint64_t fp  = fingerprint(*m);
            const auto     old = shown.fp.find(ts);
            if (old == shown.fp.end()) {
                shown.fp[ts] = fp;
                _store.addMessage(t.ref, m->clone());
            } else if (old->second != fp) {
                old->second  = fp;
                Message copy = m->clone();
                _store.updateMessage(t.ref, ts, [&](Message &x) { x = std::move(copy); });
            }
        }
    }
}

// The conversation and the session's user, as the list shows them; pushed
// when they changed. Counts are the session's own (unread = what Claude said
// since the last read; only its answers count toward the red counter).
void Backend::syncMeta(Tracked &t) {
    if (asThread(t)) {
        // A thread, not a conversation — one listed before it was known for a
        // branch leaves the list.
        if (t.ref != kNoConv && _store.conversation(t.ref).member)
            _store.updateConversation(t.ref, [](model::Conversation &c) { c.member = false; });
        return;
    }
    const model::User u    = assistantUser(t);
    std::string       usig = str::concat(
        {u.name,
         "\x1f",
         u.statusText,
         "\x1f",
         u.title,
         "\x1f",
         u.avatar,
         "\x1f",
         u.active ? "1" : "0",
         u.unavailable ? "1" : "0"}
    );
    if (usig != t.userSig) {
        t.userSig = std::move(usig);
        putUser(u);
    }
    uint32_t unread = 0, mentions = 0;
    Ts       latest = 0;
    for (const auto &[ts, seen] : t.announced) {
        if (seen.thread)
            continue;
        latest = ts;
        if (seen.mine || (t.lastRead && ts <= t.lastRead))
            continue;
        ++unread;
        if (!seen.progress)
            ++mentions;
    }
    std::string topic = homeRelative(t.info.cwd);
    if (t.skipPermissionChecks)
        topic += tr(" \xC2\xB7 no permission checks");
    const std::string readOnly = readOnlyReason(t);
    const bool        isNew    = t.ref == kNoConv;
    if (isNew) {
        model::Conversation c;
        c.id   = t.convId;
        c.kind = model::ConvKind::Dm;
        t.ref  = _store.addConversation(std::move(c));
    }
    const UserRef     dm   = sessionUser(t);
    const auto       &cur  = _store.conversation(t.ref);
    const std::string name = titleOf(t);
    if (isNew || !cur.member || cur.name != name || cur.localName != t.localName ||
        cur.topic != topic || cur.readOnly != readOnly || cur.unread != unread ||
        cur.mentions != mentions || cur.latest != latest || cur.lastRead != t.lastRead ||
        cur.dmUser != dm || cur.starred != t.starred || cur.muted != t.muted ||
        cur.notify != t.notify)
        _store.updateConversation(t.ref, [&](model::Conversation &c) {
            c.name      = name;
            c.kind      = model::ConvKind::Dm;
            c.dmUser    = dm;
            c.localName = t.localName;
            c.topic     = topic;
            c.readOnly  = readOnly;
            c.member    = true;
            c.unread    = unread;
            c.mentions  = mentions;
            c.latest    = latest;
            c.lastRead  = t.lastRead;
            c.starred   = t.starred;
            c.muted     = t.muted;
            c.notify    = t.notify;
        });
}

void Backend::syncUsers() {
    ++_batchUsers;
    _store.me   = _store.addUser(meUser());
    _usersDirty = true;
    for (const std::string &role : roleIds())
        putUser(teammateUser(_team.resolve(role)));
    if (--_batchUsers == 0 && std::exchange(_usersDirty, false))
        _store.usersChanged();
}

void Backend::putUser(model::User u) {
    _store.addUser(std::move(u));
    if (_batchUsers)
        _usersDirty = true;
    else
        _store.usersChanged();
}

// ── Roster refresh ──────────────────────────────────────────────────────────

void Backend::scheduleRefresh() {
    if (!_started || _debounce)
        return;
    // A write burst (a streamed reply) → one refresh.
    _debounce = _app.addTimer(150, false, [this, alive = _alive] {
        if (!*alive)
            return;
        _debounce = 0;
        refresh();
    });
}

void Backend::refresh() {
    // Users changed during the scan are told once, at its end.
    ++_batchUsers;
    refreshScan();
    if (--_batchUsers == 0 && std::exchange(_usersDirty, false))
        _store.usersChanged();
}

void Backend::refreshScan() {
    const auto                      scanned = scanSessions(_paths);
    // A "+" session being started shows up in the roster a moment before the
    // launcher reports its id — don't list it twice meanwhile.
    std::unordered_set<std::string> startingIn;
    for (const auto &[id, t] : _sessions)
        if (t->sending && t->info.sessionId.empty())
            startingIn.insert(file::absolute(t->info.cwd));
    for (const auto &f : _forkingIn)
        startingIn.insert(f);                  // likewise a /btw branch being launched
    std::unordered_set<std::string> listedNow; // conversation ids
    for (const auto &s : scanned) {
        const std::string convId = convIdFor(s.sessionId);
        if (!find(convId) && startingIn.count(file::absolute(s.cwd)))
            continue;
        if (const auto h = _hidden.find(s.sessionId); h != _hidden.end()) {
            // Removed from msga: stays away until its transcript gets a new
            // turn. No transcript (none yet, or Claude Code already deleted it
            // while the job lingers) means no new activity either.
            const std::string path =
                s.transcriptPath.empty() ? h->second.transcript : s.transcriptPath;
            // While msga stops it, what it still writes isn't new activity either.
            const int64_t size = sizeOf(path);
            if (h->second.stopping || size < 0 || mtimeMs(path) <= h->second.atMs)
                continue;
            // Written since, but maybe only Claude Code's bookkeeping (its
            // daemon retiring the idle worker an hour on appends some). An
            // entry saved before sizes were kept has no size: look for a turn
            // timestamped after the removal anywhere in the file instead.
            if (h->second.seenSize < 0 || size >= h->second.seenSize) {
                const bool sized = h->second.seenSize >= 0;
                if (size == h->second.seenSize ||
                    !hasTurnSince(
                        path, sized ? h->second.seenSize : 0, sized ? 0 : h->second.atMs
                    )) {
                    h->second.seenSize = size;
                    scheduleSaveKnown();
                    continue;
                }
            }
            _hidden.erase(h);
        }
        // A session msga went on from in a copy (adoptCopy), should it be
        // listed again, never takes over the chat that is the copy's now.
        if (const Tracked *owner = find(convId);
            owner && !owner->info.sessionId.empty() && owner->info.sessionId != s.sessionId)
            continue;
        listedNow.insert(convId);
        const bool        isNew   = !find(convId);
        Tracked          &t       = ensureTracked(convId);
        // Keep a known name when the fresh entry has none. A background worker
        // is named after the prompt it was resumed with, and a slash command
        // ("/compact") is no name for a session.
        const std::string oldName = t.info.name;
        t.info                    = s;
        if (str::startsWith(t.info.name, "/"))
            t.info.name.clear();
        if (t.info.name.empty())
            t.info.name = oldName;
        if (!s.transcriptPath.empty())
            t.transcriptPath = s.transcriptPath;
        t.listed = true;
        if (isNew)
            t.announcedInit = false; // a session started elsewhere: its history isn't news
    }

    // Sessions that left the roster.
    std::vector<std::string> dropped;
    for (auto &[id, tp] : _sessions) {
        Tracked &t = *tp;
        if (listedNow.count(id))
            continue;
        t.listed       = false;
        t.info.running = false;
        t.info.status.clear();
        t.info.needs.clear();
        t.info.awaitsApproval = false;
        t.info.suggestedReply.clear();
        if (t.info.sessionId.empty() || t.sending)
            continue; // a "+" session not started yet, or one being started right now
        // Kept exactly as long as Claude Code keeps the session: a background
        // job whose state dir is gone was removed (`claude rm`); any other
        // session lives on in its transcript until Claude Code cleans that up.
        const bool jobGone = t.info.kind == SessionInfo::Kind::Background;
        if (jobGone || (!t.transcriptPath.empty() && !file::exists(t.transcriptPath)) ||
            (t.transcriptPath.empty() && _paths.findTranscript(t.info.sessionId).empty()))
            dropped.push_back(id);
    }
    for (const auto &id : dropped) {
        Tracked &t = *_sessions[id];
        _convOf.erase(t.info.sessionId);
        if (t.ref != kNoConv)
            _store.updateConversation(t.ref, [](model::Conversation &c) { c.member = false; });
        for (auto it = _shown.begin(); it != _shown.end();)
            it = it->first.first == t.ref ? _shown.erase(it) : std::next(it);
        _sessions.erase(id);
        clearOutputs(id);
    }

    // Read what's new (only a live session can have changed, or one that just
    // ended: its last answer stops being "pending"; an ended one was read
    // already), then sort out which sessions are branches of which.
    for (auto &[id, tp] : _sessions) {
        Tracked &t = *tp;
        if (!t.announcedInit || t.listed || t.sending || t.wasLive)
            tail(t);
    }
    std::unordered_set<std::string> parentsToSync = detectForks();

    // What changed, per session — the /btw threads' replies first: a reply
    // arriving bumps its root's count, so the root's own count (synced with
    // its parent) must already include it, never run ahead.
    std::vector<Tracked *> order;
    order.reserve(_sessions.size());
    for (auto &[id, tp] : _sessions)
        if (asThread(*tp))
            order.push_back(tp.get());
    for (auto &[id, tp] : _sessions)
        if (!asThread(*tp))
            order.push_back(tp.get());
    for (Tracked *session : order) {
        Tracked   &t    = *session;
        const bool live = t.listed || t.sending;
        if (!t.announcedInit || live || t.wasLive) {
            sync(t);
            if (asThread(t))
                parentsToSync.insert(t.forkOf); // its root's reply count
        }
        t.wasLive = live;

        // A turn that failed for want of a login: the login is checked again
        // before the next send, which is then told why.
        if (t.parser.loginFailedAt() > _loginFailedSeen) {
            _loginFailedSeen = t.parser.loginFailedAt();
            _loginCheckedMs  = 0;
        }
        settleTurn(t);
        dispatch(t); // the next queued message, if the turn is over
        readApproval(t);
        syncMeta(t);
        if (!asThread(t))
            t.lastBusy = busy(t);
    }
    for (const auto &id : parentsToSync)
        if (Tracked *p = find(id)) {
            sync(*p);
            syncMeta(*p);
        }
    for (auto &[id, tp] : _sessions)
        if (tp->listed || tp->wasLive)
            syncThreads(*tp);
    announceRoles();
    if (!_firstScanDone) {
        // Copies of sessions gone while msga wasn't looking.
        std::vector<std::string> keep;
        for (const auto &[id, t] : _sessions)
            keep.push_back(id);
        pruneOutputs(keep);
    }
    _firstScanDone = true;
    scheduleSaveKnown();
    pumpTyping();
}

// A teammate shows as working while any of its sessions does, or a subagent
// started as it does — which no roster scan need notice: pumpTyping asks too.
void Backend::announceRoles() {
    for (const std::string &role : roleIds()) {
        const bool b = roleBusy(role);
        const bool y = roleUnavailable(role);
        if (_roleBusy[role] == b && _roleUnavailable[role] == y && _firstScanDone)
            continue;
        _roleBusy[role]        = b;
        _roleUnavailable[role] = y;
        putUser(teammateUser(_team.resolve(role)));
    }
}

// Claude working on a turn shows as the session "thinking (8m 58s)" — the
// terminal's spinner. Re-evaluated every 3 s while any session is busy (a
// background subagent's run shows in no roster file).
void Backend::pumpTyping() {
    using Key = std::tuple<ConvRef, UserRef, Ts>;
    std::map<Key, int64_t> want;
    for (auto &[id, tp] : _sessions) {
        Tracked &t = *tp;
        if (!busy(t)) {
            t.busySinceMs = 0;
            continue;
        }
        // When the turn began, fixed for as long as it runs: msga's own turn
        // from its send, else from when the session's status last changed (it
        // turned busy, possibly before msga was looking) — or, busy under a
        // stale "blocked" (working), from the turn's first record.
        if (t.busySinceMs == 0) {
            const int64_t statusSince = statusIsBusy(t.info.status)
                                            ? t.info.statusSinceMs
                                            : t.parser.turnStartedAt() / 1000;
            t.busySinceMs             = t.sending && t.sendStartedMs > 0 ? t.sendStartedMs
                                        : statusSince > 0 ? std::min(statusSince, nowMs())
                                                          : nowMs();
        }
        // A /btw thread working shows nowhere: "typing" is the session's own.
        if (asThread(t) || t.ref == kNoConv)
            continue;
        want[{t.ref, roleUser(roleOf(t)), 0}] = t.busySinceMs;
    }
    // A background subagent working "thinks" in its thread. Only a live
    // worker runs one: a killed session never sends the notification that it
    // stopped.
    for (auto &[id, tp] : _sessions) {
        Tracked &t = *tp;
        if (!t.info.running || t.stopping || asThread(t) || t.ref == kNoConv)
            continue;
        tail(t); // the notification that it stopped lands in the session's transcript
        const UserRef assistant = roleUser(roleOf(t));
        for (const auto &item : t.parser.items()) {
            if (item.kind != TranscriptItem::Kind::Subagent || item.agentId.empty())
                continue;
            if (const int64_t since = subagentRunSinceMs(t, item.agentId))
                want[{t.ref, subagentAuthor(item, assistant), item.ts}] = since;
        }
    }
    for (const auto &[key, since] : _typing)
        if (!want.count(key))
            _store.setTyping(std::get<0>(key), std::get<1>(key), std::get<2>(key), false);
    // Always set again: a message of the typer's own clears its indicator.
    for (const auto &[key, since] : want)
        _store.setTyping(std::get<0>(key), std::get<1>(key), std::get<2>(key), true, since);
    _typing = std::move(want);
    if (!_typing.empty() && !_typingTimer)
        _typingTimer = _app.addTimer(3000, true, [this, alive = _alive] {
            if (*alive)
                pumpTyping();
        });
    else if (_typing.empty() && _typingTimer) {
        _app.cancelTimer(_typingTimer);
        _typingTimer = 0;
    }
    // A subagent starting or stopping turns its teammate's dot — the stop
    // too: this is the last tick when nothing else works.
    announceRoles();
}

// File watching, by looking: the roster dirs (sessions come and go), every
// live session's state file (status changes) and live transcripts (new
// messages). What changed since the last look schedules a refresh. Faster
// while something works, so a streamed answer shows as it comes.
void Backend::watchTick() {
    std::vector<std::string>    want = {_paths.sessionsDir(), _paths.jobsDir()};
    std::vector<file::DirEntry> entries;
    if (file::listDir(_paths.sessionsDir(), &entries))
        for (const auto &e : entries)
            if (!e.isDir && str::endsWith(e.name, ".json"))
                want.push_back(_paths.sessionsDir() + "/" + e.name);
    bool anyBusy = false;
    for (const auto &[id, tp] : _sessions) {
        const Tracked &t = *tp;
        anyBusy          = anyBusy || t.sending || busy(t);
        if ((!t.listed && !t.sending) || t.info.sessionId.empty())
            continue;
        if (t.info.kind == SessionInfo::Kind::Background)
            want.push_back(_paths.jobsDir() + "/" + t.info.sessionId.substr(0, 8) + "/state.json");
        if (!t.transcriptPath.empty())
            want.push_back(t.transcriptPath);
    }
    std::unordered_map<std::string, std::pair<int64_t, int64_t>> seen;
    for (const auto &p : want) {
        const Stat s = statPath(p);
        int64_t    n = s.size;
        if (s.ok && (p == _paths.sessionsDir() || p == _paths.jobsDir())) {
            // A directory: its entries' names say what came and went.
            std::vector<file::DirEntry> list;
            uint64_t                    h = 1469598103934665603ull;
            if (file::listDir(p, &list))
                for (const auto &e : list)
                    h ^= std::hash<std::string>{}(e.name);
            n = int64_t(h);
        }
        seen[p] = {s.ok ? n : -1, s.mtimeMs};
    }
    const bool changed = seen != _watched;
    _watched           = std::move(seen);
    if (changed)
        scheduleRefresh();
    const int next = anyBusy ? 400 : 1200;
    _watchTimer    = _app.addTimer(next, false, [this, alive = _alive] {
        if (!*alive)
            return;
        _watchTimer = 0;
        watchTick();
    });
}

// ── Lifecycle ───────────────────────────────────────────────────────────────

void Backend::connect(Done done) {
    _store.workspaceId        = kWorkspaceId;
    _store.workspaceName      = "Claude Code";
    _store.workspaceIcon      = agentAvatarPath();
    _store.answersAreMentions = true;
    syncUsers();
    if (!_started) {
        _started = true;
        refresh();
        // File watching can miss what a look in between saw already; a slow
        // sweep catches whatever it missed.
        _safetyPoll = _app.addTimer(10'000, true, [this, alive = _alive] {
            if (*alive)
                refresh();
        });
        watchTick();
    }
    post([done = std::move(done)] {
        if (done)
            done(true, {});
    });
}

void Backend::loadHistory(ConvRef conv, Ts before, Done done) {
    Tracked *t = findRef(conv);
    if (!t) {
        post([done = std::move(done)] {
            if (done)
                done(false, "not found");
        });
        return;
    }
    auto msgs = visibleMessages(*t);
    if (!t->announcedInit)
        sync(*t);
    // Replies in subagent threads are in the threads (loadThread).
    std::erase_if(msgs, [](const Message &m) { return m.threadTs != 0; });
    // Newest page first, then the 200 before `before`.
    constexpr size_t kPage = 200;
    size_t           end   = msgs.size();
    if (before)
        end = size_t(
            std::lower_bound(
                msgs.begin(), msgs.end(), before, [](const Message &m, Ts b) { return m.ts < b; }
            ) -
            msgs.begin()
        );
    const size_t begin = end > kPage ? end - kPage : 0;
    Shown       &s     = _shown[{conv, 0}];
    if (!before)
        s = {};
    std::vector<Message> page;
    page.reserve(end - begin);
    for (size_t i = begin; i < end; ++i) {
        s.fp[msgs[i].ts] = fingerprint(msgs[i]);
        page.push_back(std::move(msgs[i]));
    }
    s.from =
        begin < end ? page.front().ts : (before ? before : (msgs.empty() ? 0 : msgs.back().ts + 1));
    if (!before && begin == end)
        s.from = 0;
    _store.addPage(conv, std::move(page));
    _store.updateConversation(conv, [&](model::Conversation &c) { c.hasMoreBefore = begin > 0; });
    post([done = std::move(done)] {
        if (done)
            done(true, {});
    });
}

void Backend::loadThread(ConvRef conv, Ts root, Done done) {
    Tracked             *t = findRef(conv);
    std::vector<Message> replies;
    if (t) {
        if (Tracked *f = forkFor(t->convId, root))
            replies = threadMessages(*f); // a /btw thread: the rest of the branch
        else
            replies = subagentThread(*t, root);
    }
    Shown &s = _shown[{conv, root}];
    s        = {};
    for (const auto &m : replies)
        s.fp[m.ts] = fingerprint(m);
    _store.addPage(conv, std::move(replies));
    post([ok = t != nullptr, done = std::move(done)] {
        if (done)
            done(ok, ok ? std::string() : std::string("not found"));
    });
}

void Backend::setActiveConversation(ConvRef conv, Ts) {
    const Tracked    *t  = findRef(conv);
    const std::string id = t ? t->convId : std::string();
    if (id != _lastConv && !id.empty()) {
        _lastConv = id;
        scheduleSaveKnown();
    }
}

bool Backend::isAgentSession(ConvRef conv) const {
    return findRef(conv) != nullptr;
}

std::string Backend::promptSuggestion(ConvRef conv) const {
    const Tracked *t = findRef(conv);
    // Only while it still waits for that answer: once msga sends one, or the
    // reply typed in a terminal is under way, it answers nothing any more.
    if (!t || !needsUser(*t) || !readOnlyReason(*t).empty() || awaitsApproval(t->info))
        return {};
    return t->info.suggestedReply;
}

std::string Backend::agentSessionFolder(ConvRef conv) const {
    const Tracked *t = findRef(conv);
    return t ? t->info.cwd : std::string();
}

std::string Backend::agentSessionRole(ConvRef conv) const {
    const Tracked *t = findRef(conv);
    return t ? roleOf(*t) : std::string();
}

void Backend::markRead(ConvRef conv, Ts ts) {
    Tracked *t = findRef(conv);
    if (!t || (t->lastRead && ts <= t->lastRead))
        return;
    t->lastRead = ts;
    syncMeta(*t);
    scheduleSaveKnown();
}

void Backend::markUnread(ConvRef conv, Ts ts) {
    Tracked *t = findRef(conv);
    if (!t || ts <= 0)
        return;
    t->lastRead = ts - 1;
    syncMeta(*t);
    scheduleSaveKnown();
}

void Backend::setStarred(ConvRef conv, bool starred) {
    if (Tracked *t = findRef(conv)) {
        t->starred = starred;
        syncMeta(*t);
        scheduleSaveKnown();
    }
}

void Backend::setMuted(ConvRef conv, bool muted) {
    if (Tracked *t = findRef(conv)) {
        t->muted = muted;
        syncMeta(*t);
        scheduleSaveKnown();
    }
}

void Backend::setNotifyLevel(ConvRef conv, model::NotifyLevel level) {
    if (Tracked *t = findRef(conv)) {
        t->notify = level;
        syncMeta(*t);
        scheduleSaveKnown();
    }
}

// "Rename session…": a name only msga shows (Claude Code keeps its own). It
// goes on the session's user too, which titles the DM everywhere.
void Backend::setLocalName(ConvRef conv, std::string name) {
    Tracked *t = findRef(conv);
    if (!t)
        return;
    name = std::string(str::trim(name));
    if (t->localName == name)
        return;
    t->localName = std::move(name);
    syncMeta(*t);
    saveKnown();
}

// Zen mode hides the tool-call cards: what each loaded list shows is taken
// again, and what the switch hides or reveals isn't news.
void Backend::setZenMode(bool on) {
    if (_zen == on)
        return;
    _zen = on;
    for (auto &[id, tp] : _sessions)
        if (tp->announcedInit)
            sync(*tp);
    for (auto &[id, tp] : _sessions)
        syncThreads(*tp);
}

} // namespace claude
