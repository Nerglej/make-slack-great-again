// SlackBackend's read side (see slack_backend.h): connect (identity, roster,
// users, emoji, user groups, stars, saved items), history and threads, and
// the polling that stands in for realtime.
//
// A port of msga's PublicBackend read calls plus the Slack half of its
// Session (old-msga/src/backend/slack/public_backend.cpp,
// old-msga/src/session/session.cpp): the same endpoints, parameters, merge
// rules and poll cadences. A workspace with Socket Mode push (app keys:
// slack_realtime.cpp) polls only as msga's safety net (hasRealtimePush):
// no roster/counts/threads polls, and a poll that finds a message the
// socket should have pushed re-establishes it. The open-chat cadence is
// 5 s / 60 s (session / app keys, msga's foregroundPollGapMs —
// conversations.history is 1/min for unlisted apps).
#include "app/slack/slack_backend.h"

#include "app/cache/workspace_cache.h"
#include "app/slack/slack_json.h"
#include "base/i18n.h"
#include "base/log.h"
#include "base/str.h"
#include "base/time.h"
#include "net/net.h"
#include "plat/plat.h"

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <memory>
#include <tuple>
#include <unordered_set>

namespace slack {

using model::ConvRef;
using model::kNoConv;
using model::kNoUser;
using model::Ts;
using model::UserRef;

namespace {

// Session::checkRealtimeHealth's cadences (old-msga/src/session/session.h).
constexpr int64_t     kRosterReloadGapMs   = 60'000;
constexpr int64_t     kCountsPollGapMs     = 10'000;
constexpr int64_t     kThreadsPollGapMs    = 20'000;
constexpr int64_t     kBackgroundPollGapMs = 2 * 60'000;
constexpr int64_t     kPresencePollGapMs   = 60'000;
constexpr int64_t     kSelfPresenceGapMs   = 60'000; // _selfPresenceTimer
constexpr int64_t     kStarredGapMs        = 5 * 60'000;
constexpr int64_t     kSavedGapMs          = 5 * 60'000; // kRemindersRefreshGapMs
constexpr int64_t     kUsersRefreshGapMs   = 24 * 60 * 60'000;
constexpr int64_t     kDmSweepGapSecs      = 12 * 3600; // across restarts, via the cache
constexpr int         kOffRosterProbeMs    = 90'000;
constexpr int         kPacedMs             = 1200; // the _infoApi background lane
constexpr int         kPresenceHot = 12, kPresenceRotate = 8;
constexpr int         kMaxDiffPolls = 8, kCountsFailureLimit = 3;
constexpr int         kMaxThreadInjects = 12, kMaxThreadBacklog = 3;
// Lost connections and gateway pages: 10 tries back off 0, 1, 2 … 60 s, about
// four minutes in all (msga's HttpQueue retried idempotent calls with the
// same backoff and no end; a bound lets a long outage surface as an error).
constexpr int         kMaxUserReprobes = 20, kMaxTransientRetries = 6, kMaxTransportRetries = 10;
constexpr const char *kHistoryLimit            = "50";
constexpr int64_t     kRateLimitNoticeGapMs    = 15'000; // msga's kRateLimitNoticeGapMs
// A reminder more than a week overdue is marked fired without a notification
// (msga's kMaxReminderLatenessSecs); the alarm sleeps at most 6 h at a time.
constexpr int64_t     kMaxReminderLatenessSecs = 7 * 24 * 3600;
constexpr int64_t     kMaxReminderSleepSecs    = 6 * 3600;
// msga's ThreadExportJob backstop: 400 pages × 50 replies is far beyond any
// real thread; past it the cursor is looping.
constexpr int         kMaxThreadPages          = 400;

// Slack error codes that mean "this method is not ours to call" (never a
// transport failure): the endpoint is given up on for the run.
bool methodUnavailable(const std::string &e) {
    static const char *const kCodes[] = {
        "unknown_method",
        "method_deprecated",
        "method_not_supported_for_channel_type",
        "not_allowed_token_type",
        "missing_scope",
        "no_permission",
        "invalid_arguments",
        "org_login_required",
        "enterprise_is_restricted",
        "user_is_restricted",
        "ekm_access_denied",
    };
    for (const char *c : kCodes)
        if (e == c)
            return true;
    return false;
}

// Slack's "likely a transient issue on our end": retried with backoff.
bool transientSlack(const std::string &e) {
    return e == "internal_error" || e == "service_unavailable" || e == "fatal_error";
}

// net's reasons for "no answer" ("dns", "timeout: …"), or an answer that is
// not Slack's (a proxy's 5xx / HTML gateway page: "bad_json"). Reads retry
// them (msga retried 5xx on idempotent calls).
bool transportError(const std::string &e) {
    for (const char *p : {"dns", "connect", "tls", "timeout", "protocol", "bad_json"})
        if (str::startsWith(e, p))
            return true;
    return false;
}

bool looksLikeUserId(std::string_view id) {
    return id.size() > 1 && (id[0] == 'U' || id[0] == 'W');
}

std::string threadKey(ConvRef c, Ts root) {
    return str::concat({str::number(c), ":", str::number(root)});
}

// threadKey's parts back.
ConvRef keyConv(const std::string &k) {
    return ConvRef(std::atoll(k.c_str()));
}
Ts keyTs(const std::string &k) {
    const size_t colon = k.find(':');
    return colon == std::string::npos ? 0 : Ts(std::atoll(k.c_str() + colon + 1));
}

} // namespace

struct SlackBackend::Read {
    explicit Read(SlackBackend &b);
    ~Read();

    SlackBackend &b;
    model::Store &s;
    const bool    session;   // xoxc + cookie
    int64_t       speed = 1; // MSGA_SLACK_TEST_SPEEDUP: tests compress every delay

    // ── Plumbing ────────────────────────────────────────────────────────────
    enum class Lane : uint8_t { Normal, Background };
    struct Call {
        std::string method, form;
        ApiDone     done;
        int         attempt = 0;
        Lane        lane    = Lane::Normal;
    };
    std::vector<plat::TimerId>               timers; // pending one-shots
    plat::TimerId                            tickTimer = 0;
    std::deque<Call>                         paced;
    bool                                     pacedBusy     = false;
    // Normal-lane calls not yet answered (in flight, backing off or waiting
    // out a 429): the paced lane holds while any is (msga's tryNext).
    int                                      normalPending = 0;
    std::unordered_map<std::string, int64_t> readyAt; // per-method 429 cooldown

    int64_t now() const { return base::monotonicMs() * speed; }
    void    later(int64_t ms, std::function<void()> fn);
    void    call(std::string method, std::string form, ApiDone done, Lane lane = Lane::Normal);
    void    issue(Call c);
    void    pumpPaced();
    struct Pager {
        std::string                              method, form, key;
        std::function<void(const json::Value &)> onPage;
        std::function<void(const std::string &)> onDone;
        int                                      maxPages = 0, pages = 0; // 0: no cap
    };
    // maxPages > 0: a backstop against a cursor loop (a server repeating the
    // same next_cursor forever); reaching it fails with "page_limit".
    void paginate(
        std::string                              method,
        std::string                              form,
        std::string                              key,
        std::function<void(const json::Value &)> onPage,
        std::function<void(const std::string &)> onDone,
        int                                      maxPages = 0
    );
    void        pageFrom(std::shared_ptr<Pager> p, std::string cursor);
    std::string teamForm(std::initializer_list<std::pair<std::string_view, std::string_view>> kv);
    void        applyApiBase(std::string_view workspaceUrl);

    // ── Connect and the roster ──────────────────────────────────────────────
    Backend::Done connectDone;
    int           connectPending = 0;
    std::string   connectError;
    bool          usersLoaded = false, usersLoading = false, convsLoaded = false, started = false;
    bool          convListRestricted = false, dmActivitySwept = false;

    int64_t lastRateNotice = -1; // the rate-limit banner, at most every 15 s
    void    noteRateLimited(const std::string &method, int64_t secs);

    void                          startLoads();
    void                          connectSettled(const std::string &convErr);
    void                          loadUsers(std::function<void(const std::string &)> done);
    void                          mergeUsers(std::vector<model::User> users);
    void                          loadConversations(std::function<void(const std::string &)> done);
    void                          loadViaWebClient(std::function<void(const std::string &)> done);
    void                          applyRoster(std::vector<model::Conversation> convs);
    void                          fixGroupMembers();
    void                          enrichDmActivity();
    void                          loadEmoji();
    void                          loadUsergroups();
    void                          loadCommands();
    std::vector<Backend::Command> serverCommands; // commands.list (session tokens)
    bool                          commandsLoaded = false;

    // ── Users ───────────────────────────────────────────────────────────────
    std::unordered_set<std::string>          pendingUsers, presenceUnavailable, offRoster;
    std::unordered_map<std::string, int64_t> probedAt;
    SelfPresence                             self;
    size_t                                   presenceIdx = 0;

    void fetchUserIfNeeded(UserRef u);
    void fetchMissingDmUsers();
    void resolveAuthors(const std::vector<model::Message> &page);
    void reprobeOffRoster();
    void requestPresence(UserRef u, bool background);
    void pollDmPresence();
    void refreshSelfPresence(std::function<void()> then = {});

    // ── Stars and saved items (server snapshots, diffed) ────────────────────
    bool                                     starsUnavailable = false, starsPrimed = false;
    std::unordered_set<std::string>          serverStars;
    bool                                     savedUnavailable = false, savedPrimed = false;
    std::unordered_map<std::string, int64_t> serverSaved; // "conv:ts" → due (0 = none)
    void                                     refreshStarred();
    void                                     refreshSaved();

    // ── Message reminders (msga's armReminderTimer / fireDueReminders) ──────
    plat::TimerId reminderTimer = 0;
    void          armReminders();
    void          fireDueReminders();
    void          announceReminder(ConvRef c, Ts ts);

    // ── Dead conversations (msga's _deadConvIds): channel_not_found for us —
    // another workspace's over the shared socket, a dead DM. Persisted.
    std::unordered_set<std::string> dead;
    void                            markDead(const std::string &id);
    void                            markAlive(const std::string &id);
    void                            reconcileDead();

    // ── Mentioned channels the roster lacks (msga's fetchChannelIfNeeded) ───
    std::unordered_set<std::string> pendingChannels;

    // ── Huddles ─────────────────────────────────────────────────────────────
    // A head page's newest huddle_thread room (msga's first-page check):
    // never clears a live huddle (an older page may hold a long-ended one).
    void applyHuddleRoom(ConvRef c, const json::Value &messages);

    // ── History and threads ─────────────────────────────────────────────────
    std::vector<model::Message> mapPage(ConvRef c, const json::Value &arr, bool topLevel);
    void                        loadThreadPages(ConvRef c, Ts root, bool live, Backend::Done done);

    // ── Polling (msga's checkRealtimeHealth) ────────────────────────────────
    ConvRef openConv   = kNoConv;
    Ts      openThread = 0;
    int64_t lastRoster = 0, lastCounts = 0, lastThreads = 0, lastFg = 0, lastBg = 0;
    int64_t lastPresence = 0, lastSelf = 0, lastStarred = 0, lastSaved = 0, lastUsers = 0;
    std::unordered_map<ConvRef, Ts>       pollBaseline;
    std::unordered_map<ConvRef, uint64_t> completedPoll;
    uint64_t                              pollRevision = 0;
    ConvRef                               snapshotConv = kNoConv;
    std::vector<Ts>                       snapshotTs;
    size_t                                bgIdx = 0;
    bool countsUnavailable = false, countsDisabled = false, activityPrimed = false;
    int  countsFailures = 0;
    std::unordered_map<std::string, mapjson::Counts> activity;
    bool                                threadsUnavailable = false, threadsPrimed = false;
    std::unordered_map<std::string, Ts> threadBaseline;
    std::unordered_set<std::string>     followed; // threads I started, replied in, or follow
    // msga's _unreadThreads / _threadReadFloor: followed threads with an
    // unread reply (key → its newest), and how far I read each one.
    std::unordered_map<std::string, Ts> unreadThreads, threadReadFloor;
    void                                noteUnreadThreadReply(ConvRef c, Ts root, Ts ts);
    void                                threadRead(ConvRef c, Ts root, Ts upTo);
    void                                publishUnreadThreads();

    void    tick();
    void    pollUnreadCounts();
    void    applyActivity(const std::vector<mapjson::Counts> &snapshot);
    void    pollConversation(ConvRef c, bool foreground, Ts hint = 0);
    void    pollThreadReplies();
    ConvRef nextBackgroundTarget();
    bool    inject(ConvRef c, model::Message m, bool parentIsMe);
    void    carryLocal(model::Conversation &fresh, const model::Conversation &old) const;

    // ── The workspace cache ─────────────────────────────────────────────────
    int64_t sweepAt = 0; // the last DM activity sweep (unix secs)
    void    loadExtras(const json::Value &x);
    void    saveExtras(json::Writer &w);
    void    extrasChanged() {
        if (cache)
            cache->extrasChanged();
    }
    void                                   refreshCachedHead(ConvRef c, Ts cachedNewest);
    // Last: destroyed first, its final write still sees every member above.
    std::unique_ptr<cache::WorkspaceCache> cache;
};

// ── Plumbing ────────────────────────────────────────────────────────────────

SlackBackend::Read::Read(SlackBackend &b) : b(b), s(b.store()), session(b._creds.sessionAuth()) {
    if (const char *v = std::getenv("MSGA_SLACK_TEST_SPEEDUP"); v && std::atoi(v) > 1)
        speed = std::atoi(v);
    // OAuth workspaces have no client.counts (msga's loadUnreadCounts
    // answered "no snapshot" for them): the roster diff is the activity source.
    countsDisabled = !session;
    applyApiBase(b._creds.workspaceUrl);
}

SlackBackend::Read::~Read() {
    for (plat::TimerId id : timers)
        b._app.cancelTimer(id);
    for (plat::TimerId id : {tickTimer, reminderTimer})
        if (id)
            b._app.cancelTimer(id);
}

void SlackBackend::Read::later(int64_t ms, std::function<void()> fn) {
    auto id = std::make_shared<plat::TimerId>(0);
    *id     = b._app.addTimer(
        int(std::max<int64_t>(ms / speed, 0)), false, [this, id, fn = std::move(fn)] {
            std::erase(timers, *id);
            fn();
        }
    );
    timers.push_back(*id);
}

void SlackBackend::Read::call(std::string method, std::string form, ApiDone done, Lane lane) {
    Call c{std::move(method), std::move(form), std::move(done), 0, lane};
    if (lane == Lane::Normal) {
        ++normalPending;
        issue(std::move(c));
        return;
    }
    // Background sweeps (conversations.info, users.getPresence, users.info)
    // trickle out one per 1.2 s so a sweep of a busy workspace stays under
    // its rate tier and never crowds out what the user is waiting for.
    paced.push_back(std::move(c));
    pumpPaced();
}

void SlackBackend::Read::pumpPaced() {
    // Interactive work first: the lane only moves while no Normal call is
    // outstanding (one merely cooling down on a 429 still holds it).
    if (pacedBusy || paced.empty() || normalPending > 0)
        return;
    pacedBusy = true;
    issue(std::move(paced.front()));
    paced.pop_front();
    later(kPacedMs, [this] {
        pacedBusy = false;
        pumpPaced();
    });
}

void SlackBackend::Read::issue(Call c) {
    // Slack throttles per method: only this method waits out Retry-After.
    if (const auto it = readyAt.find(c.method); it != readyAt.end() && it->second > now()) {
        const int64_t wait = it->second - now();
        later(wait, [this, c = std::move(c)]() mutable { issue(std::move(c)); });
        return;
    }
    const std::string method = c.method, form = c.form;
    b.api(
        method,
        form,
        [this, c = std::move(c)](const json::Document &doc, const std::string &err) mutable {
            if (err == "ratelimited") {
                const int64_t secs = std::max<int64_t>(doc.root()["retry_after"].integer(1), 1);
                LOG_INFO(
                    "slack", "%s rate-limited, retrying in %llds", c.method.c_str(), (long long)secs
                );
                readyAt[c.method] = now() + secs * 1000;
                later(secs * 1000, [this, c = std::move(c)]() mutable { issue(std::move(c)); });
                return;
            }
            // Transient Slack errors and lost connections: a bounded backoff
            // (msga's HttpQueue / WebApiClient), then the caller hears it.
            const bool transient = transientSlack(err) && c.attempt < kMaxTransientRetries;
            const bool transport = transportError(err) && c.attempt < kMaxTransportRetries;
            if (transient || transport) {
                const int64_t delay =
                    c.attempt == 0 ? 0 : std::min<int64_t>(1000LL << (c.attempt - 1), 60'000);
                ++c.attempt;
                later(delay, [this, c = std::move(c)]() mutable { issue(std::move(c)); });
                return;
            }
            const bool held  = c.lane == Lane::Normal && --normalPending == 0;
            const auto alive = b._alive; // `done` may end the backend
            if (c.done)
                c.done(doc, err);
            if (held && *alive)
                pumpPaced();
        }
    );
}

// msga's EvRateLimited: the error banner names the first method that
// tripped, at most once per 15 s.
void SlackBackend::Read::noteRateLimited(const std::string &method, int64_t secs) {
    const int64_t t = now();
    if (lastRateNotice >= 0 && t - lastRateNotice < kRateLimitNoticeGapMs)
        return;
    lastRateNotice = t;
    if (b.onError)
        b.onError(
            i18n::arg(
                i18n::trn(
                    "Slack is rate-limiting requests (%1) \xE2\x80\x94 retrying in %n second.",
                    "Slack is rate-limiting requests (%1) \xE2\x80\x94 retrying in %n seconds.",
                    secs
                ),
                method
            )
        );
}

void SlackBackend::Read::paginate(
    std::string                              method,
    std::string                              form,
    std::string                              key,
    std::function<void(const json::Value &)> onPage,
    std::function<void(const std::string &)> onDone,
    int                                      maxPages
) {
    auto p      = std::make_shared<Pager>(Pager{
        std::move(method), std::move(form), std::move(key), std::move(onPage), std::move(onDone)
    });
    p->maxPages = maxPages;
    pageFrom(std::move(p), {});
}

void SlackBackend::Read::pageFrom(std::shared_ptr<Pager> p, std::string cursor) {
    if (p->maxPages > 0 && ++p->pages > p->maxPages) {
        p->onDone("page_limit");
        return;
    }
    std::string form = p->form;
    if (!cursor.empty())
        form.append(str::concat({form.empty() ? "" : "&", "cursor=", net::percentEncode(cursor)}));
    call(p->method, std::move(form), [this, p](const json::Document &doc, const std::string &err) {
        if (!err.empty()) {
            p->onDone(err);
            return;
        }
        const json::Value root = doc.root();
        p->onPage(root[p->key]);
        const std::string_view next = root["response_metadata"]["next_cursor"].str();
        if (next.empty())
            p->onDone({});
        else
            pageFrom(p, std::string(next));
    });
}

// team_id is required for an org-level (Enterprise Grid) token and ignored
// for a workspace one, so msga always sent it on the listing methods.
std::string SlackBackend::Read::teamForm(
    std::initializer_list<std::pair<std::string_view, std::string_view>> kv
) {
    std::string        f    = net::formEncode(kv);
    const std::string &team = s.workspaceId.empty() ? b._creds.teamId : s.workspaceId;
    if (!team.empty())
        f.append(str::concat({f.empty() ? "" : "&", "team_id=", net::percentEncode(team)}));
    return f;
}

// A session token resolves in the context of the host it is called on; on
// slack.com a Grid token lands in the org and is refused workspace methods
// (issue #49). OAuth tokens are workspace-scoped: slack.com is fine.
void SlackBackend::Read::applyApiBase(std::string_view workspaceUrl) {
    net::Url u;
    if (!session || !u.parse(str::trim(workspaceUrl)) || u.scheme != "https" || u.host.empty())
        return;
    b._auth.base = str::concat({"https://", u.host, "/api/"});
}

// ── Connect ─────────────────────────────────────────────────────────────────

void SlackBackend::connect(Done done) {
    _read->call(
        "auth.test",
        {},
        [this, done = std::move(done)](const json::Document &doc, const std::string &err) {
            if (!err.empty()) {
                LOG_WARN("slack", "%s: auth.test: %s", _creds.teamId.c_str(), err.c_str());
                if (done)
                    done(false, err);
                return;
            }
            const json::Value o  = doc.root();
            _store.workspaceId   = std::string(o["team_id"].str(_creds.teamId));
            _store.workspaceName = std::string(o["team"].str(_creds.teamName));
            _store.workspaceUrl  = std::string(o["url"].str(_creds.workspaceUrl));
            // The icon is the one sign-in stored (team.info, image_88), as in msga.
            if (_store.workspaceIcon.empty())
                _store.workspaceIcon = _creds.iconUrl;
            // auth.test's url is the authoritative host (stored credentials may
            // predate workspaceUrl or carry a stale one).
            _read->applyApiBase(_store.workspaceUrl);
            if (o.has("enterprise_id"))
                LOG_INFO(
                    "slack",
                    "%s is part of Enterprise Grid org %.*s",
                    _store.workspaceId.c_str(),
                    int(o["enterprise_id"].str().size()),
                    o["enterprise_id"].str().data()
                );
            _store.me          = _store.internUser(o["user_id"].str());
            _read->connectDone = std::move(done);
            _read->startLoads();
        }
    );
}

void SlackBackend::Read::startLoads() {
    // Session::start's order: self presence, conversations (then emoji), stars,
    // users, user groups, saved items.
    connectPending = 2;
    connectError.clear();
    refreshSelfPresence();
    loadConversations([this](const std::string &err) {
        if (!err.empty()) {
            connectError = err;
        } else if (connectDone) {
            // The conversation list is what the window waits for (msga showed
            // its column the moment it arrived); users.list may take many
            // pages more, and names fill in as it lands.
            Backend::Done done = std::move(connectDone);
            connectDone        = nullptr;
            done(true, {});
        }
        connectSettled(err);
    });
    loadUsers([this](const std::string &) { connectSettled({}); });
    refreshStarred();
    loadUsergroups();
    refreshSaved();
    loadCommands();
}

void SlackBackend::Read::connectSettled(const std::string &) {
    if (--connectPending > 0)
        return;
    Backend::Done done = std::move(connectDone);
    connectDone        = nullptr;
    if (!convsLoaded) {
        // No roster: the caller retries connect (nothing to render yet).
        if (done)
            done(false, connectError.empty() ? std::string("no_conversations") : connectError);
        return;
    }
    if (done)
        done(true, {});
    if (started)
        return;
    started         = true;
    const int64_t t = now();
    lastRoster = lastUsers = lastStarred = lastSaved = lastSelf = lastPresence = t;
    lastBg                                                                     = t;
    // The first activity snapshot seeds the unread badges at once (msga
    // also had them from its cache); presence starts with the hot set.
    pollUnreadCounts();
    lastCounts = t;
    pollDmPresence();
    loadEmoji();
    // msga's safety timer: min(15 s, the open-chat cadence).
    const int tickMs = int((session ? 5'000 : 15'000) / speed);
    tickTimer        = b._app.addTimer(std::max(tickMs, 1), true, [this] { tick(); });
}

void SlackBackend::Read::loadUsers(std::function<void(const std::string &)> done) {
    auto acc     = std::make_shared<std::vector<model::User>>();
    usersLoading = true;
    paginate(
        "users.list",
        teamForm({{"limit", "200"}}),
        "members",
        [acc](const json::Value &arr) {
            for (const json::Value u : arr) {
                model::User m = mapjson::toUser(u);
                if (!m.id.empty())
                    acc->push_back(std::move(m));
            }
        },
        [this, acc, done = std::move(done)](const std::string &err) {
            usersLoading = false;
            if (!err.empty()) {
                LOG_WARN("slack", "users.list: %s", err.c_str());
            } else {
                mergeUsers(std::move(*acc));
                usersLoaded = true;
                fetchMissingDmUsers();
                fixGroupMembers();
                later(kOffRosterProbeMs, [this] { reprobeOffRoster(); });
            }
            done(err);
        }
    );
}

// msga's mergeUserSnapshot: snapshot rows win, known enrichment fills their
// gaps, and users the snapshot omits are kept (Slack Connect peers are never
// in users.list; departed members come back as deleted rows, not absences).
void SlackBackend::Read::mergeUsers(std::vector<model::User> users) {
    if (users.empty())
        return; // an empty snapshot says nothing
    std::unordered_set<std::string> inSnapshot;
    for (model::User &u : users) {
        inSnapshot.insert(u.id);
        if (const UserRef r = s.findUser(u.id); r != kNoUser) {
            const model::User &old = s.user(r);
            if (!old.placeholder) {
                if (u.avatar.empty())
                    u.avatar = old.avatar;
                if (u.displayName.empty())
                    u.displayName = old.displayName;
                if (u.name.empty())
                    u.name = old.name;
            }
            // Presence is polled separately (and may already have answered
            // for a user only interned so far).
            u.active = old.active;
            u.dnd    = old.dnd;
        }
        s.addUser(std::move(u));
    }
    std::unordered_set<std::string> off;
    for (size_t i = 0; i < s.userCount(); ++i) {
        const model::User &u = s.user(UserRef(i));
        if (!u.placeholder && !inSnapshot.count(u.id))
            off.insert(u.id);
    }
    offRoster = std::move(off);
    for (auto it = probedAt.begin(); it != probedAt.end();)
        it = offRoster.count(it->first) ? std::next(it) : probedAt.erase(it);
    s.usersChanged();
}

void SlackBackend::Read::loadConversations(std::function<void(const std::string &)> done) {
    if (convListRestricted) {
        loadViaWebClient(std::move(done));
        return;
    }
    auto acc = std::make_shared<std::vector<model::Conversation>>();
    // limit 1000: conversations.list is Tier 2, fewer pages = fewer 429s.
    paginate(
        "conversations.list",
        teamForm(
            {{"types", "public_channel,private_channel,im,mpim"},
             {"exclude_archived", "true"},
             {"limit", "1000"}}
        ),
        "channels",
        [this, acc](const json::Value &arr) {
            for (const json::Value c : arr) {
                model::Conversation m = mapjson::toConversation(c, s);
                if (!m.id.empty())
                    acc->push_back(std::move(m));
            }
        },
        [this, acc, done = std::move(done)](const std::string &err) mutable {
            if (err == "enterprise_is_restricted") {
                // Grid: conversations.list is never served to a session token.
                if (!convListRestricted)
                    LOG_INFO(
                        "slack", "conversations.list restricted (Grid): client.userBoot + im.list"
                    );
                convListRestricted = true;
                loadViaWebClient(std::move(done));
                return;
            }
            if (!err.empty()) {
                LOG_WARN("slack", "conversations.list: %s", err.c_str());
                done(err);
                return;
            }
            applyRoster(std::move(*acc));
            done({});
        }
    );
}

// How Slack's own client boots: client.userBoot (joined channels and MPDMs)
// + im.list (every DM; userBoot only carries the open ones). Both halves
// must land, or the roster would be replaced by half of itself.
void SlackBackend::Read::loadViaWebClient(std::function<void(const std::string &)> done) {
    struct Acc {
        std::vector<model::Conversation>         convs;
        int                                      pending = 2;
        std::string                              error;
        std::function<void(const std::string &)> done;
    };
    auto acc  = std::make_shared<Acc>();
    acc->done = std::move(done);
    auto add  = [this, acc](const json::Value &arr) {
        for (const json::Value c : arr)
            if (!c["is_archived"].boolean()) // userBoot has no exclude_archived
                if (model::Conversation m = mapjson::toConversation(c, s); !m.id.empty())
                    acc->convs.push_back(std::move(m));
    };
    auto finish = [this, acc](const std::string &err) {
        if (!err.empty())
            acc->error = err;
        if (--acc->pending > 0)
            return;
        if (acc->error.empty())
            applyRoster(std::move(acc->convs));
        acc->done(acc->error);
    };
    call(
        "client.userBoot",
        "min_channel_updated=0",
        [add, finish](const json::Document &doc, const std::string &err) {
            if (err.empty())
                add(doc.root()["channels"]);
            else
                LOG_WARN("slack", "client.userBoot: %s", err.c_str());
            finish(err);
        }
    );
    paginate(
        "im.list",
        "get_latest=true&get_read_state=true&limit=1000",
        "ims",
        add,
        [finish](const std::string &err) {
            if (!err.empty())
                LOG_WARN("slack", "im.list: %s", err.c_str());
            finish(err);
        }
    );
}

// msga's reloadConversations + carryLocalConvState: what the API cannot
// tell (local badges, mute, notify level, cursors, members, local name)
// survives a reload.
void SlackBackend::Read::applyRoster(std::vector<model::Conversation> convs) {
    // The roster's own numbers are the activity source once client.counts is
    // unavailable (OAuth, or given up on) — taken before the local merge.
    std::vector<mapjson::Counts> serverActivity;
    if (countsDisabled)
        for (const model::Conversation &c : convs)
            serverActivity.push_back({c.id, c.latest, c.lastRead, c.unread, c.mentions});

    std::unordered_set<std::string> listed;
    for (model::Conversation &fresh : convs) {
        listed.insert(fresh.id);
        if (const ConvRef r = s.findConversation(fresh.id); r != kNoConv) {
            carryLocal(fresh, s.conversation(r));
        } else if (starsPrimed && serverStars.count(fresh.id)) {
            fresh.starred = true; // stars.list answered before the roster did
        }
        s.addConversation(std::move(fresh));
    }
    // msga replaced its list: what is no longer listed (left, archived) is
    // no longer a member conversation.
    for (ConvRef r = 0; r < s.conversationCount(); ++r)
        if (s.conversation(r).member && !listed.count(s.conversation(r).id))
            s.updateConversation(r, [](model::Conversation &c) { c.member = false; });
    convsLoaded = true;
    reconcileDead();
    if (!serverActivity.empty())
        applyActivity(serverActivity);
    fixGroupMembers();
    fetchMissingDmUsers();
    enrichDmActivity();
}

void SlackBackend::Read::carryLocal(
    model::Conversation &fresh, const model::Conversation &old
) const {
    fresh.unread   = std::max(fresh.unread, old.unread);
    fresh.mentions = std::max(fresh.mentions, old.mentions);
    fresh.starred  = fresh.starred || old.starred; // stars.list decides
    if (fresh.notify == model::NotifyLevel::Default)
        fresh.notify = old.notify; // no read API for the per-channel level
    fresh.muted = fresh.muted || old.muted;
    if (fresh.localName.empty())
        fresh.localName = old.localName;
    if (fresh.members.empty())
        fresh.members = old.members;
    fresh.lastRead = std::max(fresh.lastRead, old.lastRead);
    fresh.latest   = std::max(fresh.latest, old.latest);
    if (fresh.canvasTitle.empty()) { // conversations.list omits properties
        fresh.canvasTitle = old.canvasTitle;
        fresh.canvasId    = old.canvasId;
    }
    if (fresh.memberCount == 0)
        fresh.memberCount = old.memberCount;
    // The list carries no room: a live huddle stays until its end is seen
    // (msga's reloadConversations merge).
    if (old.huddleActive && !fresh.huddleActive) {
        fresh.huddleActive       = true;
        fresh.huddleLink         = old.huddleLink;
        fresh.huddleParticipants = old.huddleParticipants;
    }
}

// conversations.list leaves a group DM's members out; msga's sidebar read
// them from the "mpdm-alice--bob-1" name. The Store names groups by members.
void SlackBackend::Read::fixGroupMembers() {
    if (!usersLoaded)
        return;
    std::unordered_map<std::string, UserRef> byHandle;
    for (ConvRef r = 0; r < s.conversationCount(); ++r) {
        const model::Conversation &c = s.conversation(r);
        if (c.kind != model::ConvKind::Group || !c.members.empty() ||
            !str::startsWith(c.name, "mpdm-"))
            continue;
        if (byHandle.empty())
            for (size_t i = 0; i < s.userCount(); ++i)
                if (!s.user(UserRef(i)).name.empty())
                    byHandle.emplace(s.user(UserRef(i)).name, UserRef(i));
        std::string_view n = std::string_view(c.name).substr(5);
        if (const size_t dash = n.rfind('-');
            dash != std::string_view::npos && dash + 1 < n.size() &&
            std::all_of(n.begin() + dash + 1, n.end(), [](char ch) {
                return ch >= '0' && ch <= '9';
            }))
            n = n.substr(0, dash);
        std::vector<UserRef> members;
        while (!n.empty()) {
            const size_t     sep    = n.find("--");
            std::string_view handle = n.substr(0, sep);
            if (const auto it = byHandle.find(std::string(handle)); it != byHandle.end())
                members.push_back(it->second);
            n = sep == std::string_view::npos ? std::string_view() : n.substr(sep + 2);
        }
        if (s.me != kNoUser && std::find(members.begin(), members.end(), s.me) == members.end())
            members.push_back(s.me);
        if (members.size() > 1)
            s.updateConversation(r, [&](model::Conversation &x) {
                x.members = std::move(members);
            });
    }
}

// msga's enrichDmActivity: conversations.list carries no last_read / latest,
// so DMs and MPDMs get them from conversations.info on the paced lane. Only
// needed without client.counts (which reports them for everything); msga ran
// it once per 12 h across restarts, here once per run.
void SlackBackend::Read::enrichDmActivity() {
    if (!countsDisabled || dmActivitySwept)
        return;
    dmActivitySwept = true;
    // msga: one sweep per 12 h across restarts (the cache keeps the stamp
    // and the cursors it found).
    if (base::nowSecs() - sweepAt < kDmSweepGapSecs)
        return;
    std::vector<std::string> ids;
    for (int pass = 0; pass < 2; ++pass) // 1:1 DMs first, then MPDMs
        for (ConvRef r = 0; r < s.conversationCount(); ++r)
            if (s.conversation(r).kind == (pass ? model::ConvKind::Group : model::ConvKind::Dm) &&
                !dead.count(s.conversation(r).id))
                ids.push_back(s.conversation(r).id);
    auto remaining = std::make_shared<size_t>(ids.size());
    for (const std::string &id : ids)
        call(
            "conversations.info",
            net::formEncode({{"channel", id}}),
            [this, id, remaining](const json::Document &doc, const std::string &err) {
                if (--*remaining == 0) { // every call settled: the sweep is done
                    sweepAt = base::nowSecs();
                    extrasChanged();
                }
                if (err == "channel_not_found")
                    markDead(id);
                const ConvRef r = s.findConversation(id);
                if (!err.empty() || r == kNoConv)
                    return;
                const model::Conversation  info = mapjson::toConversation(doc.root()["channel"], s);
                const model::Conversation &c    = s.conversation(r);
                if (info.lastRead <= c.lastRead && info.latest <= c.latest)
                    return;
                s.updateConversation(r, [&](model::Conversation &x) {
                    x.lastRead = std::max(x.lastRead, info.lastRead);
                    x.latest   = std::max(x.latest, info.latest);
                });
            },
            Lane::Background
        );
}

void SlackBackend::Read::loadEmoji() {
    call("emoji.list", {}, [this](const json::Document &doc, const std::string &err) {
        if (!err.empty()) {
            LOG_WARN("slack", "emoji.list: %s", err.c_str());
            return;
        }
        // name → image URL, or "alias:other" (resolved by the Store).
        for (const json::Value e : doc.root()["emoji"])
            s.setCustomEmoji(std::string(e.key()), std::string(e.str()));
        s.usersChanged(); // repaint: emoji in names, statuses and messages
        if (cache)
            cache->emojiChanged();
    });
}

void SlackBackend::Read::loadUsergroups() {
    // msga's loadUsergroupsFromBackend: every group with its handle and name
    // (mentions show "@handle"); include_users: a mention of a group I
    // belong to counts as a mention.
    call(
        "usergroups.list",
        teamForm({{"include_users", "1"}}),
        [this](const json::Document &doc, const std::string &err) {
            if (!err.empty()) // missing_scope on an older OAuth token: the cache stays
                return;
            std::vector<model::Store::Usergroup> groups;
            for (const json::Value g : doc.root()["usergroups"]) {
                model::Store::Usergroup x;
                x.id     = std::string(g["id"].str());
                x.handle = std::string(g["handle"].str());
                x.name   = std::string(g["name"].str());
                for (const json::Value u : g["users"])
                    x.users.emplace_back(u.str());
                if (!x.id.empty())
                    groups.push_back(std::move(x));
            }
            // An empty snapshot says nothing (msga kept its cache).
            if (groups.empty() || groups == s.usergroups())
                return;
            s.setUsergroups(std::move(groups));
            extrasChanged(); // meta.json carries them
        }
    );
}

// msga's loadCommands: the workspace's slash commands (an array, or an
// object keyed by name); session tokens only (OAuth answers
// not_allowed_token_type).
void SlackBackend::Read::loadCommands() {
    if (!session)
        return;
    call("commands.list", {}, [this](const json::Document &doc, const std::string &err) {
        if (!err.empty()) {
            if (err != "not_allowed_token_type" && err != "cancelled")
                LOG_WARN("slack", "commands.list: %s", err.c_str());
            return;
        }
        std::vector<Backend::Command> out;
        for (const json::Value c : doc.root()["commands"]) {
            Backend::Command x;
            std::string_view name = c["name"].str();
            if (name.empty())
                name = c.key();
            if (!name.empty() && name.front() == '/')
                name.remove_prefix(1);
            if (name.empty())
                continue;
            x.name  = std::string(name);
            x.desc  = std::string(c["desc"].str());
            x.usage = std::string(c["usage"].str());
            x.local = true; // run by runLocalCommand (chat.command), never sent as text
            // msga's row label: "App · <name>" for an app's, "Slack" else.
            x.app   = c["type"].str() == "app";
            if (x.app) {
                const std::string_view appName = c["app_name"].str();
                x.source = appName.empty() ? std::string(i18n::tr("App"))
                                           : str::concat({i18n::tr("App"), " \xC2\xB7 ", appName});
                x.icon   = std::string(c["icon_url"].str());
                for (const char *k : {"image_48", "image_36"})
                    if (x.icon.empty())
                        x.icon = std::string(c["icons"][k].str());
            } else {
                x.source = "Slack";
            }
            out.push_back(std::move(x));
        }
        serverCommands = std::move(out);
        commandsLoaded = true;
    });
}

// ── Users ───────────────────────────────────────────────────────────────────

// users.info for someone users.list never lists (Slack Connect peers, USLACK,
// deactivated accounts) — msga's fetchUserIfNeeded.
void SlackBackend::Read::fetchUserIfNeeded(UserRef ref) {
    if (ref == kNoUser || !s.user(ref).placeholder)
        return;
    const std::string id = s.user(ref).id;
    if (pendingUsers.count(id))
        return;
    const bool bot = id.size() > 1 && id[0] == 'B';
    if (!bot && !looksLikeUserId(id))
        return;
    pendingUsers.insert(id);
    // A bot id (a bot post without a profile) resolves through bots.info.
    call(
        bot ? "bots.info" : "users.info",
        net::formEncode({{bot ? "bot" : "user", id}}),
        [this, id, bot](const json::Document &doc, const std::string &err) {
            pendingUsers.erase(id);
            if (!err.empty())
                return;
            model::User u;
            if (bot) {
                const json::Value o = doc.root()["bot"], icons = o["icons"];
                u.id   = id;
                u.name = u.displayName = std::string(o["name"].str());
                for (const char *k : {"image_72", "image_48", "image_36"})
                    if (u.avatar.empty())
                        u.avatar = std::string(icons[k].str());
                u.bot = true;
            } else {
                u = mapjson::toUser(doc.root()["user"]);
                if (u.id.empty())
                    return;
                offRoster.insert(u.id); // only the daily re-probe refreshes it
                probedAt[u.id] = now();
                extrasChanged();
            }
            s.addUser(std::move(u));
            s.usersChanged();
        }
    );
}

void SlackBackend::Read::fetchMissingDmUsers() {
    if (!usersLoaded)
        return; // every peer would look missing; the users load calls again
    for (ConvRef r = 0; r < s.conversationCount(); ++r) {
        const model::Conversation &c = s.conversation(r);
        if (c.kind == model::ConvKind::Dm)
            fetchUserIfNeeded(c.dmUser);
        else if (c.kind == model::ConvKind::Group)
            for (UserRef u : std::vector<UserRef>(c.members))
                fetchUserIfNeeded(u);
    }
}

// Authors the roster doesn't know (msga's message list asked as it painted).
void SlackBackend::Read::resolveAuthors(const std::vector<model::Message> &page) {
    if (!usersLoaded)
        return;
    for (const model::Message &m : page) {
        fetchUserIfNeeded(m.user);
        if (m.isHuddle()) // msga fetched a huddle row's attendees as it painted
            for (UserRef u : m.extra->huddle.attendees)
                fetchUserIfNeeded(u);
    }
}

// msga's reprobeOffRosterUsers: users known only through users.info get a
// daily refresh, oldest first, capped per pass, on the paced lane.
void SlackBackend::Read::reprobeOffRoster() {
    const int64_t                                t = now();
    std::vector<std::pair<int64_t, std::string>> due;
    for (const std::string &id : offRoster) {
        if (!looksLikeUserId(id) || mapjson::isSlackSystemUser(id) || pendingUsers.count(id))
            continue;
        const auto    it   = probedAt.find(id);
        const int64_t last = it == probedAt.end() ? 0 : it->second;
        if (it != probedAt.end() && t - last < kUsersRefreshGapMs)
            continue;
        due.emplace_back(last, id);
    }
    std::sort(due.begin(), due.end());
    if (due.size() > size_t(kMaxUserReprobes))
        due.resize(kMaxUserReprobes);
    if (!due.empty())
        extrasChanged();
    for (const auto &[last, id] : due) {
        probedAt[id] = t; // at request time: a failure waits a day
        call(
            "users.info",
            net::formEncode({{"user", id}}),
            [this](const json::Document &doc, const std::string &err) {
                if (!err.empty())
                    return;
                model::User   u = mapjson::toUser(doc.root()["user"]);
                const UserRef r = s.findUser(u.id);
                if (u.id.empty() || r == kNoUser)
                    return;
                u.active = s.user(r).active;
                u.dnd    = s.user(r).dnd;
                s.addUser(std::move(u));
                s.usersChanged();
            },
            Lane::Background
        );
    }
}

// msga's requestPresence: users.getPresence answers internal_error for bots,
// system accounts and users not presence-visible to us — skip those, and
// stop sweeping someone after a failed sweep probe.
void SlackBackend::Read::requestPresence(UserRef ref, bool background) {
    const model::User &u = s.user(ref);
    if (ref == kNoUser || u.placeholder || u.bot || mapjson::isSlackSystemUser(u.id))
        return;
    if (background && presenceUnavailable.count(u.id))
        return;
    const std::string id = u.id;
    call(
        "users.getPresence",
        net::formEncode({{"user", id}}),
        [this, id, background](const json::Document &doc, const std::string &err) {
            if (!err.empty()) {
                if (background)
                    presenceUnavailable.insert(id);
                return;
            }
            presenceUnavailable.erase(id);
            const bool    active = doc.root()["presence"].str() == "active";
            const UserRef r      = s.findUser(id);
            if (r == kNoUser || s.user(r).active == active)
                return; // unchanged: no repaint per sweep
            s.user(r).active = active;
            s.usersChanged();
        },
        background ? Lane::Background : Lane::Normal
    );
}

// msga's pollDmPresence: the 12 most recently active DM partners every
// round, plus a rotating window of 8 over the rest.
void SlackBackend::Read::pollDmPresence() {
    std::vector<std::pair<Ts, UserRef>> dms;
    for (ConvRef r = 0; r < s.conversationCount(); ++r) {
        const model::Conversation &c = s.conversation(r);
        if (c.kind == model::ConvKind::Dm && c.dmUser != kNoUser && c.dmUser != s.me)
            dms.emplace_back(c.latest, c.dmUser);
    }
    std::sort(dms.begin(), dms.end(), [](const auto &a, const auto &b) {
        return a.first > b.first;
    });
    const size_t n = dms.size(), hot = std::min<size_t>(kPresenceHot, n);
    for (size_t i = 0; i < hot; ++i)
        requestPresence(dms[i].second, true);
    const size_t rest = n - hot;
    if (rest == 0) {
        presenceIdx = 0;
        return;
    }
    presenceIdx %= rest;
    const size_t batch = std::min<size_t>(kPresenceRotate, rest);
    for (size_t i = 0; i < batch; ++i)
        requestPresence(dms[hot + (presenceIdx + i) % rest].second, true);
    presenceIdx = (presenceIdx + batch) % rest;
}

void SlackBackend::Read::refreshSelfPresence(std::function<void()> then) {
    // No "user": Slack answers the rich self snapshot (online, manual_away…).
    call(
        "users.getPresence",
        {},
        [this, then = std::move(then)](const json::Document &doc, const std::string &err) {
            if (err == "cancelled")
                return;
            if (err.empty()) {
                const json::Value  o    = doc.root();
                const SelfPresence prev = self;
                self.loaded             = true;
                self.active             = o["presence"].str() == "active";
                self.online             = o["online"].boolean();
                self.manualAway         = o["manual_away"].boolean();
                bool changed = !prev.loaded || prev.active != self.active ||
                               prev.online != self.online || prev.manualAway != self.manualAway;
                if (s.me != kNoUser && s.user(s.me).active != self.active) {
                    s.user(s.me).active = self.active;
                    changed             = true;
                }
                // msga's selfPresence() is a value the footer follows: a new
                // manual_away alone must reach it too.
                if (changed && s.me != kNoUser)
                    s.usersChanged();
            }
            if (then)
                then();
        }
    );
}

// ── Stars and saved items ───────────────────────────────────────────────────

// msga's refreshStarred: stars.list is the only read path for conversation
// stars. msga skipped rows toggled locally in the last minute; here only
// rows whose SERVER state moved since the previous snapshot change, which
// never fights a local toggle still on its way.
void SlackBackend::Read::refreshStarred() {
    lastStarred = now();
    if (starsUnavailable)
        return;
    auto ids = std::make_shared<std::vector<std::string>>();
    paginate(
        "stars.list",
        "limit=200",
        "items",
        [ids](const json::Value &items) { mapjson::starredConversationIds(items, *ids); },
        [this, ids](const std::string &err) {
            if (!err.empty()) {
                if (methodUnavailable(err))
                    starsUnavailable = true;
                return;
            }
            std::unordered_set<std::string> now(ids->begin(), ids->end());
            for (ConvRef r = 0; r < s.conversationCount(); ++r) {
                const std::string &id   = s.conversation(r).id;
                const bool         star = now.count(id) > 0;
                if (starsPrimed && star == (serverStars.count(id) > 0))
                    continue;
                if (s.conversation(r).starred != star)
                    s.updateConversation(r, [star](model::Conversation &c) { c.starred = star; });
            }
            serverStars = std::move(now);
            starsPrimed = true;
        }
    );
}

// msga's refreshReminders (saved.list, session tokens only): "save for later"
// items and their due times, diffed against the previous snapshot as above.
void SlackBackend::Read::refreshSaved() {
    lastSaved = now();
    if (!session || savedUnavailable)
        return;
    call(
        "saved.list",
        "limit=50&filter=saved",
        [this](const json::Document &doc, const std::string &err) {
            if (!err.empty()) {
                if (methodUnavailable(err))
                    savedUnavailable = true;
                return;
            }
            std::unordered_map<std::string, int64_t>      fresh;
            std::vector<std::tuple<ConvRef, Ts, int64_t>> refs; // + date_created
            for (const json::Value it : doc.root()["saved_items"]) {
                if (it["item_type"].str() != "message" || it["state"].str() == "completed" ||
                    it["is_archived"].boolean())
                    continue;
                const ConvRef c  = s.findConversation(it["item_id"].str());
                const Ts      ts = model::parseTs(it["ts"].str());
                if (c == kNoConv || !ts)
                    continue;
                fresh[threadKey(c, ts)] = it["date_due"].integer();
                refs.emplace_back(c, ts, it["date_created"].integer());
            }
            auto apply = [this](ConvRef c, Ts ts, bool saved, int64_t due, int64_t savedAt) {
                s.updateMessage(c, ts, [saved](model::Message &m) { m.saved = saved; });
                s.setReminderAt(c, ts, due);
                s.setSavedItem(c, ts, saved, due, savedAt); // the Saved messages page
            };
            for (const auto &[c, ts, created] : refs) {
                const std::string k  = threadKey(c, ts);
                const auto        it = serverSaved.find(k);
                if (!savedPrimed || it == serverSaved.end() || it->second != fresh[k])
                    apply(c, ts, true, fresh[k], created);
            }
            for (const auto &[k, due] : serverSaved)
                if (!fresh.count(k))
                    apply(keyConv(k), keyTs(k), false, 0, 0);
            serverSaved = std::move(fresh);
            savedPrimed = true;
            extrasChanged();
            armReminders();
        }
    );
}

// ── Message reminders ───────────────────────────────────────────────────────

// msga's armReminderTimer: wake at the nearest due reminder not fired yet
// (at least 1 s out, so firing never runs inside a Store change; at most
// 6 h, then look again).
void SlackBackend::Read::armReminders() {
    if (reminderTimer)
        b._app.cancelTimer(reminderTimer);
    reminderTimer   = 0;
    int64_t nearest = 0;
    for (const model::Store::SavedItem &it : s.savedItems())
        if (it.due > 0 && !it.fired && (!nearest || it.due < nearest))
            nearest = it.due;
    if (!nearest)
        return;
    const int64_t secs = std::clamp<int64_t>(nearest - base::nowSecs(), 1, kMaxReminderSleepSecs);
    reminderTimer      = b._app.addTimer(int(secs * 1000 / speed), false, [this] {
        reminderTimer = 0;
        fireDueReminders();
    });
}

// msga's fireDueReminders: each due reminder is marked fired (and saved)
// first, then announced — unless it is over a week late (a machine that was
// off), which goes quietly. The item stays listed.
void SlackBackend::Read::fireDueReminders() {
    const int64_t                       t = base::nowSecs();
    std::vector<std::pair<ConvRef, Ts>> due;
    for (const model::Store::SavedItem &it : s.savedItems()) {
        if (it.due <= 0 || it.fired || it.due > t)
            continue;
        s.setReminderFired(it.conv, it.ts, true);
        if (t - it.due <= kMaxReminderLatenessSecs)
            due.emplace_back(it.conv, it.ts);
    }
    if (!due.empty())
        extrasChanged();
    armReminders();
    for (const auto &[c, ts] : due)
        announceReminder(c, ts);
}

// msga's announceReminderDue: the notification wants the message's text; a
// reminder whose preview isn't known looks it up first (once).
void SlackBackend::Read::announceReminder(ConvRef c, Ts ts) {
    const model::Store::SavedItem *it = s.findSaved(c, ts);
    if (!it)
        return; // removed meanwhile
    auto emit = [this, c, ts] {
        if (s.findSaved(c, ts) && b.onReminderDue) {
            auto fn = b.onReminderDue;
            fn(c, ts);
        }
    };
    if (it->previewed) {
        emit();
        return;
    }
    if (const model::Message *m = s.findMessage(c, ts)) {
        s.setSavedPreview(c, ts, m);
        emit();
        return;
    }
    b.loadMessage(c, ts, [this, c, ts, emit](bool ok, model::Message m) {
        s.setSavedPreview(c, ts, ok ? &m : nullptr);
        emit();
    });
}

// ── Dead conversations ──────────────────────────────────────────────────────

void SlackBackend::Read::markDead(const std::string &id) {
    if (!id.empty() && dead.insert(id).second)
        extrasChanged();
}

void SlackBackend::Read::markAlive(const std::string &id) {
    if (dead.erase(id))
        extrasChanged();
}

// msga's reconcileDeadConvIds: a fresh roster revives what it lists — a DM
// only once its peer is known and not deactivated.
void SlackBackend::Read::reconcileDead() {
    if (dead.empty())
        return;
    for (ConvRef r = 0; r < s.conversationCount(); ++r) {
        const model::Conversation &c = s.conversation(r);
        if (!c.member || !dead.count(c.id))
            continue;
        if (c.kind == model::ConvKind::Dm) {
            const model::User &u = s.user(c.dmUser);
            if (u.placeholder || u.deleted)
                continue;
        }
        markAlive(c.id);
    }
}

// ── Huddles ─────────────────────────────────────────────────────────────────

void SlackBackend::Read::applyHuddleRoom(ConvRef c, const json::Value &messages) {
    mapjson::HuddleRoom h;
    if (c >= s.conversationCount() || !mapjson::newestHuddleRoom(messages, s, h))
        return;
    b.setHuddle(c, h.active, std::move(h.link), std::move(h.participants));
}

void SlackBackend::setHuddle(
    ConvRef c, bool active, std::string link, std::vector<UserRef> participants
) {
    if (c >= _store.conversationCount())
        return;
    const model::Conversation &x = _store.conversation(c);
    if (x.huddleActive == active && x.huddleLink == link && x.huddleParticipants == participants)
        return;
    _store.updateConversation(c, [&](model::Conversation &y) {
        y.huddleActive       = active;
        y.huddleLink         = std::move(link);
        y.huddleParticipants = std::move(participants);
    });
}

// msga's isThreadFollowed + isFollowedThreadReply: a thread I follow, or
// one I started.
bool SlackBackend::threadFollowed(ConvRef c, Ts root) const {
    if (_read->followed.count(threadKey(c, root)))
        return true;
    const model::Message *r = _store.findMessage(c, root);
    return r && _store.me != kNoUser && r->user == _store.me;
}

void SlackBackend::resolveUser(UserRef u) {
    _read->fetchUserIfNeeded(u);
}

void SlackBackend::requestPresence(UserRef u) {
    if (u != kNoUser && u != _store.me)
        _read->requestPresence(u, false);
}

// ── Mentioned channels ──────────────────────────────────────────────────────

void SlackBackend::resolveChannel(const std::string &id) {
    Read &r = *_read;
    if (id.empty() || _store.findConversation(id) != kNoConv || _store.channelName(id) ||
        !r.pendingChannels.insert(id).second)
        return;
    r.call(
        "conversations.info",
        net::formEncode({{"channel", id}}),
        [this, id](const json::Document &doc, const std::string &err) {
            _read->pendingChannels.erase(id); // a passing failure tries again later
            if (err == "channel_not_found") {
                _store.setChannelName(id, {});
                return;
            }
            const std::string_view name = doc.root()["channel"]["name"].str();
            if (err.empty() && !name.empty())
                _store.setChannelName(id, std::string(name));
        },
        Read::Lane::Background
    );
}

// ── History and threads ─────────────────────────────────────────────────────

// Maps a page (any order) oldest first. History pages are top-level lists:
// an "also sent to channel" reply sits there as a top-level copy. Local
// state the server doesn't echo (my saved flag) carries over.
std::vector<model::Message>
SlackBackend::Read::mapPage(ConvRef c, const json::Value &arr, bool topLevel) {
    std::vector<model::Message> page;
    page.reserve(arr.size());
    for (const json::Value v : arr) {
        model::Message m = mapjson::toMessage(v, s);
        if (!m.ts)
            continue;
        if (topLevel && m.isReply())
            m.threadTs = 0;
        if (const model::Message *old = s.findMessage(c, m.ts))
            m.saved = old->saved;
        else
            m.saved = serverSaved.count(threadKey(c, m.ts)) > 0;
        page.push_back(std::move(m));
    }
    std::sort(page.begin(), page.end(), [](const model::Message &a, const model::Message &b) {
        return a.ts < b.ts;
    });
    return page;
}

void SlackBackend::loadHistory(ConvRef conv, Ts before, Done done) {
    const std::string &id = convId(conv);
    if (id.empty()) {
        _app.post([done, alive = _alive] {
            if (*alive && done)
                done(false, "channel_not_found");
        });
        return;
    }
    // msga paged with next_cursor; `latest` (exclusive) is the same page.
    std::string form = net::formEncode({{"channel", id}, {"limit", kHistoryLimit}});
    if (before)
        form.append(str::concat({"&latest=", model::formatTs(before), "&inclusive=false"}));
    _read->call(
        "conversations.history",
        std::move(form),
        [this, conv, before, done = std::move(done)](
            const json::Document &doc, const std::string &err
        ) {
            if (!err.empty()) {
                LOG_WARN("slack", "conversations.history: %s", err.c_str());
                if (done)
                    done(false, err);
                return;
            }
            std::vector<model::Message> page = _read->mapPage(conv, doc.root()["messages"], true);
            _read->resolveAuthors(page);
            if (!before)
                _read->applyHuddleRoom(conv, doc.root()["messages"]);
            const bool more = doc.root()["has_more"].boolean();
            _store.addPage(conv, std::move(page));
            if (_store.conversation(conv).hasMoreBefore != more)
                _store.updateConversation(conv, [more](model::Conversation &c) {
                    c.hasMoreBefore = more;
                });
            if (done)
                done(true, {});
        }
    );
}

void SlackBackend::loadThread(ConvRef conv, Ts root, Done done) {
    _read->loadThreadPages(conv, root, false, std::move(done));
}

// conversations.replies, every page (oldest first; the first row is the
// root). live: a refresh of the open thread — replies that are new to us are
// delivered as live messages (badges, reply counters), as msga's poll did.
void SlackBackend::Read::loadThreadPages(ConvRef c, Ts root, bool live, Backend::Done done) {
    const std::string &id = b.convId(c);
    if (id.empty() || !root) {
        if (done)
            later(0, [done] { done(false, "thread_not_found"); });
        return;
    }
    struct Acc {
        std::vector<model::Message> replies;
        model::Message              rootMsg;
        bool                        haveRoot = false;
        std::vector<char>           parentIsMe; // per reply
    };
    auto acc = std::make_shared<Acc>();
    paginate(
        "conversations.replies",
        net::formEncode({{"channel", id}, {"ts", model::formatTs(root)}, {"limit", kHistoryLimit}}),
        "messages",
        [this, c, root, acc](const json::Value &arr) {
            const std::string &me = s.user(s.me).id;
            for (const json::Value v : arr) {
                model::Message m = mapjson::toMessage(v, s);
                if (m.ts == root) {
                    acc->rootMsg  = std::move(m);
                    acc->haveRoot = true;
                } else if (m.ts) {
                    m.threadTs = root;
                    if (const model::Message *old = s.findMessage(c, m.ts))
                        m.saved = old->saved;
                    acc->parentIsMe.push_back(!me.empty() && v["parent_user_id"].str() == me);
                    acc->replies.push_back(std::move(m));
                }
            }
        },
        [this, c, root, live, acc, done = std::move(done)](const std::string &err) {
            if (!err.empty()) {
                LOG_WARN("slack", "conversations.replies: %s", err.c_str());
                if (done)
                    done(false, err);
                return;
            }
            if (c >= s.conversationCount()) { // the Store was cleared meanwhile
                if (done)
                    done(false, "channel_not_found");
                return;
            }
            // The root stays in the channel list: refresh its thread fields.
            if (acc->haveRoot)
                s.updateMessage(c, root, [&](model::Message &r) {
                    r.replyCount  = acc->rootMsg.replyCount;
                    r.latestReply = acc->rootMsg.latestReply;
                    r.replyUsers  = acc->rootMsg.replyUsers;
                    r.threadTs    = root;
                });
            resolveAuthors(acc->replies);
            const std::vector<model::Message> *have = s.replies(c, root);
            // Every page was read, so the set is complete: a reply we hold
            // that the server no longer has was deleted elsewhere.
            if (have) {
                std::vector<Ts> gone;
                for (const model::Message &m : *have)
                    if (!m.pending && std::none_of(
                                          acc->replies.begin(),
                                          acc->replies.end(),
                                          [&](const model::Message &x) { return x.ts == m.ts; }
                                      ))
                        gone.push_back(m.ts);
                for (Ts ts : gone)
                    s.removeMessage(c, ts);
                have = s.replies(c, root);
            }
            if (live && have) {
                const Ts newest = have->empty() ? 0 : have->back().ts;
                for (size_t i = 0; i < acc->replies.size(); ++i)
                    if (acc->replies[i].ts > newest)
                        inject(c, acc->replies[i].clone(), acc->parentIsMe[i]);
            }
            if (!acc->replies.empty())
                s.addPage(c, std::move(acc->replies));
            if (done)
                done(true, {});
        },
        kMaxThreadPages
    );
}

void SlackBackend::setActiveConversation(ConvRef conv, Ts thread) {
    Read &r = *_read;
    if (r.openConv != conv) {
        // The deletion baseline belongs to the chat that was open; and the
        // newly opened one is polled on the next tick (msga reset the shared
        // cooldown so hopping between chats can't starve one).
        r.snapshotConv = kNoConv;
        r.snapshotTs.clear();
        r.lastFg = 0;
    }
    r.openConv   = conv;
    r.openThread = thread;
    if (!r.cache || conv == kNoConv || conv >= _store.conversationCount())
        return;
    // msga's openConversation: the cached messages show at once, the network
    // page is merged in when it comes; and this is the chat to reopen.
    r.cache->setLastConversation(conv);
    if (const Ts newest = r.cache->loadMessages(conv))
        r.refreshCachedHead(conv, newest);
}

// ── The workspace cache ─────────────────────────────────────────────────────

bool SlackBackend::openCache(std::string dir) {
    Read &r = *_read;
    if (r.cache)
        return false;
    r.cache             = std::make_unique<cache::WorkspaceCache>(_app, _store, std::move(dir));
    r.cache->saveExtras = [&r](json::Writer &w) { r.saveExtras(w); };
    json::Document meta;
    const bool     warm = r.cache->load(&meta);
    if (warm)
        r.loadExtras(meta.root()["x"]);
    // A cache written before toUser flagged them still says is_bot=false.
    for (const char *id : {"USLACKBOT", "USLACK"})
        if (const UserRef u = _store.findUser(id); u != kNoUser)
            _store.user(u).bot = true;
    return warm;
}

void SlackBackend::closeCache(bool keep) {
    if (_read->cache)
        _read->cache->close(keep);
}

ConvRef SlackBackend::lastConversation() const {
    return _read->cache ? _store.findConversation(_read->cache->lastConversation()) : kNoConv;
}

bool SlackBackend::connecting() const {
    return _read->connectPending > 0;
}

// A saved item's preview as meta.json keeps it: the card shows one line, so
// a long message is cut (at a character, never inside a <@U…> token).
namespace {
std::string previewText(const std::string &text) {
    constexpr size_t kMax = 600;
    if (text.size() <= kMax)
        return text;
    size_t n = kMax;
    while (n > 0 && (uint8_t(text[n]) & 0xC0) == 0x80)
        --n;
    const size_t open = text.rfind('<', n);
    if (open != std::string::npos && text.find('>', open) >= n)
        n = open;
    return text.substr(0, n);
}
} // namespace

// What only this backend knows, kept in meta.json's "x": the saved items
// (msga's reminders list: the saved flag of a message not loaded yet, and
// what to unsave when the server's list drops it), the threads I follow
// (a reply right after a start still badges as a followed-thread one), when
// each off-roster user was last re-probed, and the DM activity sweep.
void SlackBackend::Read::saveExtras(json::Writer &w) {
    const auto conv = [this](const std::string &k) -> const std::string & {
        return b.convId(keyConv(k));
    };
    w.key("saved").beginArray();
    for (const auto &[k, due] : serverSaved)
        if (!conv(k).empty()) {
            const model::Store::SavedItem *it = s.findSaved(keyConv(k), keyTs(k));
            w.beginArray().value(conv(k)).value(int64_t(keyTs(k))).value(due);
            w.value(it ? it->savedAt : int64_t(0)).value(it && it->fired);
            // msga's reminderPreviews: what the message said, so the Saved
            // page and a due reminder show it without fetching it again.
            if (it && it->previewed &&
                (!it->text.empty() || it->author != kNoUser || !it->botName.empty())) {
                w.value(previewText(it->text))
                    .value(it->author != kNoUser ? s.user(it->author).id : std::string())
                    .value(int64_t(it->thread))
                    .value(it->botName)
                    .value(it->botAvatar);
            }
            w.endArray();
        }
    w.endArray().key("followed").beginArray();
    for (const std::string &k : followed)
        if (!conv(k).empty())
            w.beginArray().value(conv(k)).value(int64_t(keyTs(k))).endArray();
    // Probe times run on the monotonic clock (now()); disk keeps unix secs.
    const int64_t t = now(), wall = base::nowSecs();
    w.endArray().key("probed").beginArray();
    for (const auto &[id, at] : probedAt)
        w.beginArray().value(id).value(wall - (t - at) / (1000 * speed)).endArray();
    w.endArray().key("sweep").value(sweepAt);
    w.key("dead").beginArray();
    for (const std::string &id : dead)
        w.value(id);
    // msga's usergroups.json: the groups as last listed.
    w.endArray().key("ug").beginArray();
    for (const model::Store::Usergroup &g : s.usergroups()) {
        w.beginArray().value(g.id).value(g.handle).value(g.name).beginArray();
        for (const std::string &u : g.users)
            w.value(u);
        w.endArray().endArray();
    }
    w.endArray();
}

void SlackBackend::Read::loadExtras(const json::Value &x) {
    for (const json::Value v : x["saved"])
        if (const ConvRef c = s.findConversation(v[0].str()); c != kNoConv && v[1].integer()) {
            serverSaved[threadKey(c, v[1].integer())] = v[2].integer();
            // Saved messages lists it before the first saved.list answers.
            s.setSavedItem(c, v[1].integer(), true, v[2].integer(), v[3].integer());
            if (v[4].boolean())
                s.setReminderFired(c, v[1].integer(), true);
            if (v[5].isString()) {
                model::Message m;
                m.ts       = v[1].integer();
                m.threadTs = v[7].integer();
                m.text     = v[5].str();
                if (!v[6].str().empty())
                    m.user = s.internUser(v[6].str());
                if (!v[8].str().empty() || !v[9].str().empty()) {
                    m.extra            = std::make_unique<model::MessageExtras>();
                    m.extra->botName   = v[8].str();
                    m.extra->botAvatar = v[9].str();
                }
                s.setSavedPreview(c, m.ts, &m);
            }
        }
    for (const json::Value v : x["followed"])
        if (const ConvRef c = s.findConversation(v[0].str()); c != kNoConv && v[1].integer())
            followed.insert(threadKey(c, v[1].integer()));
    const int64_t t = now(), wall = base::nowSecs();
    for (const json::Value v : x["probed"])
        if (!v[0].str().empty())
            probedAt[std::string(v[0].str())] =
                t - std::max<int64_t>(wall - v[1].integer(), 0) * 1000 * speed;
    sweepAt = x["sweep"].integer();
    for (const json::Value v : x["dead"])
        if (!v.str().empty())
            dead.emplace(v.str());
    std::vector<model::Store::Usergroup> groups;
    for (const json::Value v : x["ug"]) {
        model::Store::Usergroup g{
            std::string(v[0].str()), std::string(v[1].str()), std::string(v[2].str()), {}
        };
        for (const json::Value u : v[3])
            g.users.emplace_back(u.str());
        if (!g.id.empty())
            groups.push_back(std::move(g));
    }
    if (!groups.empty())
        s.setUsergroups(std::move(groups));
    armReminders();
}

// The head page of a conversation shown from the cache (the old message
// list's mergeHeadPage): what the server no longer has inside the page's
// span was deleted while we were away, and a cached run that doesn't reach
// the page is cut loose — kept, it would leave a hole that paging from its
// oldest message could never fill, so it goes and paging starts from the
// head. Only messages that came from the cache are judged (ts at most
// cachedNewest); anything that arrived live since is left alone.
void SlackBackend::Read::refreshCachedHead(ConvRef c, Ts cachedNewest) {
    call(
        "conversations.history",
        net::formEncode({{"channel", b.convId(c)}, {"limit", kHistoryLimit}}),
        [this, c, cachedNewest](const json::Document &doc, const std::string &err) {
            if (!err.empty() || c >= s.conversationCount())
                return;
            std::vector<model::Message> page = mapPage(c, doc.root()["messages"], true);
            const bool                  more = doc.root()["has_more"].boolean();
            applyHuddleRoom(c, doc.root()["messages"]);
            const auto inPage = [&page](Ts ts) {
                const auto it = std::lower_bound(
                    page.begin(), page.end(), ts, [](const model::Message &m, Ts t) {
                        return m.ts < t;
                    }
                );
                return it != page.end() && it->ts == ts;
            };
            const Ts lo      = page.empty() ? INT64_MAX : page.front().ts;
            bool     overlap = false;
            for (const model::Message &m : s.conversation(c).messages)
                overlap = overlap || (!m.pending && m.ts <= cachedNewest && inPage(m.ts));
            const bool      cut = more && !page.empty() && !overlap;
            std::vector<Ts> gone;
            for (const model::Message &m : s.conversation(c).messages) {
                if (m.pending || m.ts > cachedNewest || inPage(m.ts))
                    continue;
                if (m.ts >= lo || !more || cut)
                    gone.push_back(m.ts);
            }
            for (Ts ts : gone)
                s.removeMessage(c, ts);
            resolveAuthors(page);
            if (!page.empty()) {
                Ts &base = pollBaseline[c];
                base     = std::max(base, page.back().ts);
            }
            s.addPage(c, std::move(page));
            if ((cut || !more) && s.conversation(c).hasMoreBefore != more)
                s.updateConversation(c, [more](model::Conversation &x) { x.hasMoreBefore = more; });
        }
    );
}

// ── Polling ─────────────────────────────────────────────────────────────────

void SlackBackend::Read::tick() {
    if (b._authLost)
        return;
    const int64_t t = now();
    // (1) The socket (if any) is still connected: a no-op while healthy.
    b.realtimeTick();
    const bool push = b.hasRealtimePush();
    // (0) No push: reload the roster ourselves (new DMs and channels). A
    // push workspace hears of them (and backfills on a reconnect).
    if (!push && t - lastRoster >= kRosterReloadGapMs) {
        lastRoster = t;
        loadConversations([](const std::string &) {});
    }
    // (0b) One request reports every conversation's activity.
    if (!push && !countsDisabled && t - lastCounts >= kCountsPollGapMs) {
        lastCounts = t;
        pollUnreadCounts();
    }
    // (0c) Thread replies move no channel's `latest`: only the feed sees them.
    if (!push && session && !threadsUnavailable && t - lastThreads >= kThreadsPollGapMs) {
        lastThreads = t;
        pollThreadReplies();
    }
    // (2) The open chat.
    if (openConv != kNoConv && t - lastFg >= (session ? 5'000 : 60'000)) {
        lastFg = t;
        pollConversation(openConv, true);
    }
    // (2b) Renames and avatars: once per day of uptime.
    if (t - lastUsers >= kUsersRefreshGapMs) {
        lastUsers = t;
        loadUsers([](const std::string &) {});
        loadUsergroups();
    } else if (!usersLoaded && !usersLoading && t - lastUsers >= kRosterReloadGapMs) {
        // users.list never came (a flaky start outlasted its retries):
        // without it nobody gets a name, so again at the roster cadence.
        lastUsers = t;
        loadUsers([](const std::string &) {});
    }
    if (t - lastSaved >= kSavedGapMs)
        refreshSaved();
    if (t - lastStarred >= kStarredGapMs)
        refreshStarred();
    if (t - lastPresence >= kPresencePollGapMs) {
        lastPresence = t;
        pollDmPresence();
    }
    if (t - lastSelf >= kSelfPresenceGapMs) {
        lastSelf = t;
        refreshSelfPresence();
    }
    // Background rotation over the other member conversations.
    if (t - lastBg >= kBackgroundPollGapMs) {
        lastBg = t;
        if (const ConvRef bg = nextBackgroundTarget(); bg != kNoConv)
            pollConversation(bg, false);
    }
}

// Most recently active first, but a cursor walks the whole list so quiet
// channels get their turn too.
ConvRef SlackBackend::Read::nextBackgroundTarget() {
    std::vector<ConvRef> cands;
    for (ConvRef r = 0; r < s.conversationCount(); ++r)
        if (s.conversation(r).member && r != openConv)
            cands.push_back(r);
    if (cands.empty())
        return kNoConv;
    std::sort(cands.begin(), cands.end(), [this](ConvRef a, ConvRef b2) {
        return s.conversation(a).latest > s.conversation(b2).latest;
    });
    bgIdx %= cands.size();
    return cands[bgIdx++];
}

void SlackBackend::Read::pollUnreadCounts() {
    if (countsDisabled || countsUnavailable)
        return;
    call("client.counts", {}, [this](const json::Document &doc, const std::string &err) {
        if (err.empty()) {
            countsFailures = 0;
            applyActivity(mapjson::toCounts(doc.root()));
            return;
        }
        if (methodUnavailable(err))
            countsUnavailable = true;
        // A blip must not cost the mechanism; a few failures in a row do.
        if (countsUnavailable || ++countsFailures >= kCountsFailureLimit) {
            LOG_WARN(
                "slack",
                "client.counts unavailable (%s): falling back to the roster diff",
                err.c_str()
            );
            countsDisabled = true;
            activity.clear();
            activityPrimed = false;
        }
    });
}

// msga's applyActivitySnapshot: fold the cursors in (upward only), seed the
// badges of conversations seen for the first time, and poll the ones that
// moved since the previous snapshot.
void SlackBackend::Read::applyActivity(const std::vector<mapjson::Counts> &snapshot) {
    if (snapshot.empty())
        return;
    const bool priming = !activityPrimed;
    activityPrimed     = true;
    struct Moved {
        ConvRef         conv;
        mapjson::Counts prev, now;
    };
    std::vector<Moved> moved;
    for (const mapjson::Counts &c : snapshot) {
        const auto    it        = activity.find(c.id);
        const bool    firstSeen = priming || it == activity.end();
        const ConvRef r         = s.findConversation(c.id);
        if (r != kNoConv) {
            const model::Conversation &x      = s.conversation(r);
            uint32_t                   unread = x.unread, mentions = x.mentions;
            if (firstSeen && x.member && r != openConv) {
                // Every DM unread is a red-badge "mention"; a muted
                // conversation badges only explicit mentions.
                const bool     muted = x.muted || x.notify == model::NotifyLevel::Nothing;
                const uint32_t m     = x.isDirect() ? std::max(c.unread, c.mentions) : c.mentions;
                unread               = std::max(unread, muted ? m : std::max(c.unread, m));
                mentions             = std::max(mentions, m);
            }
            if (c.latest > x.latest || c.lastRead > x.lastRead || unread != x.unread ||
                mentions != x.mentions)
                s.updateConversation(r, [&](model::Conversation &y) {
                    y.latest   = std::max(y.latest, c.latest);
                    y.lastRead = std::max(y.lastRead, c.lastRead);
                    y.unread   = unread;
                    y.mentions = mentions;
                });
        }
        if (firstSeen) {
            activity[c.id] = c;
            continue;
        }
        const mapjson::Counts prev = it->second;
        if (c.latest > prev.latest || c.unread > prev.unread || c.mentions > prev.mentions)
            moved.push_back({r, prev, c});
        else
            it->second = c;
    }
    std::sort(moved.begin(), moved.end(), [](const Moved &a, const Moved &b2) {
        return a.now.latest > b2.now.latest;
    });
    int budget = kMaxDiffPolls;
    for (const Moved &m : moved) {
        const model::Conversation *c = m.conv == kNoConv ? nullptr : &s.conversation(m.conv);
        const bool mutedQuiet = c && (c->muted || c->notify == model::NotifyLevel::Nothing) &&
                                m.now.mentions <= m.prev.mentions;
        if (!c || !c->member || mutedQuiet || m.conv == openConv) {
            activity[m.now.id] = m.now;
            continue;
        }
        if (budget-- <= 0)
            break; // left stale on purpose: still "moved" next time
        activity[m.now.id] = m.now;
        pollConversation(m.conv, false, m.prev.latest);
    }
}

// msga's pollConversationForMissed: the head page of a conversation; what is
// newer than the baseline arrives as live messages. Foreground (the open
// chat) also merges the whole page (edits, reactions, reply counts, a buried
// gap), detects deletions and refreshes an open thread whose root moved.
void SlackBackend::Read::pollConversation(ConvRef c, bool foreground, Ts hint) {
    if (c >= s.conversationCount())
        return;
    Ts lastKnown = 0;
    if (const auto it = pollBaseline.find(c); it != pollBaseline.end())
        lastKnown = it->second;
    if (!lastKnown)
        lastKnown = hint;
    if (!lastKnown)
        lastKnown = s.conversation(c).latest;
    // Never scanned: record the head without injecting (that page is old news).
    const bool     priming  = !lastKnown;
    const uint64_t revision = ++pollRevision;
    call(
        "conversations.history",
        net::formEncode({{"channel", b.convId(c)}, {"limit", kHistoryLimit}}),
        [this, c, foreground, lastKnown, priming, revision](
            const json::Document &doc, const std::string &err
        ) {
            if (!err.empty() || c >= s.conversationCount())
                return;
            // Stale intent: the open chat moved on (foreground), or this one
            // became the open chat (the foreground poll covers it now).
            if (foreground ? openConv != c : openConv == c)
                return;
            if (revision < completedPoll[c])
                return;
            completedPoll[c]                 = revision;
            std::vector<model::Message> page = mapPage(c, doc.root()["messages"], true);
            applyHuddleRoom(c, doc.root()["messages"]);
            if (!page.empty())
                pollBaseline[c] = page.back().ts;
            if (priming && !foreground)
                return;
            resolveAuthors(page);
            bool missed = false;
            if (!priming)
                for (const model::Message &m : page)
                    if (m.ts > lastKnown)
                        missed = inject(c, m.clone(), false) || missed;
            // The socket should have pushed that: it is compromised (msga's
            // reestablishRealtime, throttled there). On a poll-only workspace
            // this poll IS the delivery, not a miss.
            if (missed && b.hasRealtimePush())
                b.realtimeMissed();
            if (!foreground || page.empty())
                return;
            // Deleted elsewhere: in the last snapshot, gone now, and not
            // merely pushed below the page by newer traffic.
            const Ts oldest = page.front().ts;
            if (snapshotConv == c)
                for (Ts ts : snapshotTs)
                    if (ts >= oldest &&
                        std::none_of(page.begin(), page.end(), [ts](const model::Message &m) {
                            return m.ts == ts;
                        }))
                        s.removeMessage(c, ts);
            snapshotConv = c;
            snapshotTs.clear();
            for (const model::Message &m : page)
                snapshotTs.push_back(m.ts);
            // An open thread: its root's latest_reply moving (or the root
            // being off the page) is the cue to re-read the replies.
            if (openThread) {
                const model::Message *root = nullptr;
                for (const model::Message &m : page)
                    if (m.ts == openThread)
                        root = &m;
                const std::vector<model::Message> *have   = s.replies(c, openThread);
                Ts                                 newest = 0;
                if (have)
                    for (const model::Message &m : *have)
                        if (!m.pending)
                            newest = std::max(newest, m.ts);
                if (!root || (root->latestReply ? root->latestReply : root->ts) !=
                                 (newest ? newest : root->ts))
                    loadThreadPages(c, openThread, true, nullptr);
            }
            s.addPage(c, std::move(page));
        }
    );
}

// msga's pollThreadReplies (subscriptions.thread.getView, session tokens):
// the one endpoint that reports thread replies workspace-wide.
void SlackBackend::Read::pollThreadReplies() {
    call(
        "subscriptions.thread.getView",
        "limit=10&priority_mode=all",
        [this](const json::Document &doc, const std::string &err) {
            if (!err.empty()) {
                if (methodUnavailable(err))
                    threadsUnavailable = true;
                return;
            }
            const bool priming          = !threadsPrimed;
            threadsPrimed               = true;
            int                injected = 0;
            const std::string &me       = s.user(s.me).id;
            for (const json::Value t : doc.root()["threads"]) {
                const json::Value rootObj = t["root_msg"];
                if (rootObj.has("subscribed") && !rootObj["subscribed"].boolean())
                    continue;
                const ConvRef c    = s.findConversation(rootObj["channel"].str());
                const Ts      root = model::parseTs(rootObj["ts"].str());
                if (c == kNoConv || !root)
                    continue;
                const std::string key = threadKey(c, root);
                if (followed.insert(key).second) // the feed IS the subscription list
                    extrasChanged();
                struct Reply {
                    model::Message m;
                    bool           parentIsMe;
                };
                std::vector<Reply> replies;
                for (const char *k : {"latest_replies", "unread_replies"})
                    for (const json::Value r : t[k]) {
                        model::Message m = mapjson::toMessage(r, s);
                        if (!m.ts ||
                            std::any_of(replies.begin(), replies.end(), [&](const Reply &x) {
                                return x.m.ts == m.ts;
                            }))
                            continue;
                        m.threadTs = root;
                        replies.push_back(
                            {std::move(m), !me.empty() && r["parent_user_id"].str() == me}
                        );
                    }
                std::sort(replies.begin(), replies.end(), [](const Reply &a, const Reply &b2) {
                    return a.m.ts < b2.m.ts;
                });
                const Ts newest = replies.empty() ? 0 : replies.back().m.ts;
                // The Threads entry (msga's _unreadThreads): the feed's own
                // read cursor or mine, whichever is further; the first page
                // after a start restores it.
                const Ts floor =
                    std::max(model::parseTs(rootObj["last_read"].str()), threadReadFloor[key]);
                if (newest > floor && !s.threadMuted(c, root))
                    unreadThreads[key] = newest;
                else
                    unreadThreads.erase(key);
                Ts baseline = 0;
                if (const auto it = threadBaseline.find(key); it != threadBaseline.end())
                    baseline = it->second;
                if (!baseline) {
                    // First page of the run primes; a thread that shows up
                    // later uses its own read cursor as the floor.
                    baseline = priming ? newest : model::parseTs(rootObj["last_read"].str());
                    if (!baseline) {
                        threadBaseline[key] = newest;
                        continue;
                    }
                }
                // At most the newest few of a backlog are announced.
                size_t unseen = 0;
                for (const Reply &r : replies)
                    unseen += r.m.ts > baseline;
                if (unseen > size_t(kMaxThreadBacklog))
                    baseline = replies[replies.size() - kMaxThreadBacklog - 1].m.ts;
                Ts reached = newest;
                for (Reply &r : replies) {
                    if (r.m.ts <= baseline)
                        continue;
                    if (injected >= kMaxThreadInjects) {
                        reached = baseline; // the rest drains next tick
                        break;
                    }
                    if (!s.findMessage(c, r.m.ts)) {
                        inject(c, std::move(r.m), r.parentIsMe);
                        ++injected;
                    }
                    baseline = std::max(baseline, r.m.ts);
                }
                threadBaseline[key] = std::max(baseline, reached);
            }
            // "All clear" from the server (the count is threads, not replies).
            if (doc.root()["total_unread_replies"].integer() == 0)
                unreadThreads.clear();
            publishUnreadThreads();
        }
    );
}

// msga's noteUnreadThreadReply: a live reply in a thread I follow (or one
// that mentions me) lights the Threads entry until I read up to it.
void SlackBackend::Read::noteUnreadThreadReply(ConvRef c, Ts root, Ts ts) {
    const std::string key = threadKey(c, root);
    if (ts <= threadReadFloor[key])
        return;
    Ts &newest = unreadThreads[key];
    newest     = std::max(newest, ts);
    publishUnreadThreads();
}

// msga's markThreadRead: the floor (and the poll's baseline) move up to
// upTo; the thread is read once its newest unread reply is.
void SlackBackend::Read::threadRead(ConvRef c, Ts root, Ts upTo) {
    const std::string key = threadKey(c, root);
    Ts               &f   = threadReadFloor[key];
    f                     = std::max(f, upTo);
    Ts &base              = threadBaseline[key];
    base                  = std::max(base, upTo);
    if (const auto it = unreadThreads.find(key); it != unreadThreads.end() && it->second <= upTo)
        unreadThreads.erase(it);
    publishUnreadThreads();
}

void SlackBackend::Read::publishUnreadThreads() {
    int n = 0;
    for (const auto &[k, ts] : unreadThreads)
        n += !s.threadMuted(keyConv(k), keyTs(k));
    s.setUnreadThreads(n);
}

// msga's loadThreadsView + JsonMappers::toThreadsViewPage: one page of the
// Threads page's feed (the endpoint the poll above reads).
void SlackBackend::loadThreadsView(std::string cursor, ThreadsViewDone done) {
    Read *r = _read;
    if (!r->session || r->threadsUnavailable) {
        r->later(0, [done = std::move(done)] {
            if (done)
                done(false, {});
        });
        return;
    }
    std::string form = "limit=10&priority_mode=all"; // every followed thread
    // Continuation is the previous answer's max_ts (no next_cursor here).
    if (!cursor.empty())
        form += "&" + net::formEncode({{"current_ts", cursor}});
    r->call(
        "subscriptions.thread.getView",
        std::move(form),
        [r, done = std::move(done)](const json::Document &doc, const std::string &err) {
            if (err == "cancelled")
                return;
            if (!err.empty()) {
                // Given up on only for a method-level refusal, never a transport failure.
                if (methodUnavailable(err))
                    r->threadsUnavailable = true;
                LOG_WARN("slack", "subscriptions.thread.getView: %s", err.c_str());
                if (done)
                    done(false, {});
                return;
            }
            model::Store     &s    = r->s;
            const json::Value root = doc.root();
            ThreadsView       page;
            page.totalUnreadReplies = int(root["total_unread_replies"].integer());
            page.hasMore            = root["has_more"].boolean();
            page.nextCursor         = std::string(root["max_ts"].str());
            for (const json::Value t : root["threads"]) {
                const json::Value rootObj = t["root_msg"];
                if (rootObj.has("subscribed") && !rootObj["subscribed"].boolean())
                    continue;
                FollowedThread f;
                // root_msg is a whole message and, unlike history ones, names its channel.
                f.conv     = s.findConversation(rootObj["channel"].str());
                f.root     = mapjson::toMessage(rootObj, s);
                f.lastRead = model::parseTs(rootObj["last_read"].str());
                if (f.conv == kNoConv || !f.root.ts)
                    continue;
                // latest_replies on a read thread, unread_replies (alone) on
                // one with news: both, deduplicated, oldest first.
                for (const char *k : {"latest_replies", "unread_replies"})
                    for (const json::Value rv : t[k]) {
                        model::Message m   = mapjson::toMessage(rv, s);
                        const auto     dup = [&](const model::Message &x) { return x.ts == m.ts; };
                        if (!m.ts ||
                            std::any_of(f.latestReplies.begin(), f.latestReplies.end(), dup))
                            continue;
                        m.threadTs = f.root.ts;
                        f.latestReplies.push_back(std::move(m));
                    }
                std::sort(
                    f.latestReplies.begin(),
                    f.latestReplies.end(),
                    [](const model::Message &a, const model::Message &b2) { return a.ts < b2.ts; }
                );
                page.threads.push_back(std::move(f));
            }
            if (done)
                done(true, std::move(page));
        }
    );
}

// msga's loadMessageAt: conversations.replies answers for any ts in the
// conversation — a plain message and a thread root come back as
// messages[0] (limit 1 cuts the rest of the thread), and a reply's own ts
// returns just that reply, which conversations.history never lists.
void SlackBackend::loadMessage(ConvRef conv, Ts ts, MessageDone done) {
    Read              *r  = _read;
    const std::string &id = convId(conv);
    if (id.empty() || !ts) {
        r->later(0, [done = std::move(done)] {
            if (done)
                done(false, {});
        });
        return;
    }
    r->call(
        "conversations.replies",
        net::formEncode({{"channel", id}, {"ts", model::formatTs(ts)}, {"limit", "1"}}),
        [r, ts, done = std::move(done)](const json::Document &doc, const std::string &err) {
            if (err == "cancelled")
                return;
            if (err.empty())
                for (const json::Value v : doc.root()["messages"]) {
                    model::Message m = mapjson::toMessage(v, r->s);
                    if (m.ts != ts)
                        continue; // a thread root came back instead of the reply
                    if (done)
                        done(true, std::move(m));
                    return;
                }
            if (done)
                done(false, {});
        },
        Read::Lane::Background
    );
}

// A message that arrived (msga's handleNewMessage): into the Store as a live
// message, then the badge by msga's rules — every DM message is a red badge,
// a muted conversation badges only mentions and followed-thread replies,
// plain channel thread replies don't badge the channel.
bool SlackBackend::Read::inject(ConvRef c, model::Message m, bool parentIsMe) {
    if (c >= s.conversationCount() || s.findMessage(c, m.ts))
        return false; // seen already (a history page, the send's own echo)
    const bool own  = s.me != kNoUser && m.user == s.me;
    const Ts   root = m.isReply() ? m.threadTs : 0;
    if (own) {
        // My send's echo, polled before chat.postMessage answered: its
        // pending copy is still there and the send's confirm replaces it.
        const std::vector<model::Message> *list =
            root ? s.replies(c, root) : &s.conversation(c).messages;
        if (list)
            for (size_t i = list->size(), n = 0; i-- > 0 && n < 10; ++n)
                if ((*list)[i].pending && (*list)[i].text == m.text)
                    return false;
    }
    const Ts          ts      = m.ts;
    const UserRef     author  = m.user;
    const bool        mention = s.mentionsMe(m.text);
    const std::string key     = root ? threadKey(c, root) : std::string();
    // Replying subscribes, as Slack does; a thread I started (parent_user_id)
    // is followed whether or not its root is loaded.
    if (root && (own || parentIsMe) && followed.insert(key).second)
        extrasChanged();
    // Someone held as away just posted: one probe instead of waiting a round.
    if (!own && author != kNoUser && !s.user(author).active)
        requestPresence(author, true);

    const model::Conversation &cv      = s.conversation(c);
    uint32_t                   unread0 = cv.unread, mentions0 = cv.mentions;
    const Ts                   lastRead = cv.lastRead;
    bool                       counted  = !own && cv.member && ts > lastRead;
    const bool                 isDm = cv.isDirect(), muted = cv.muted;

    if (root && !s.replies(c, root)) {
        // The thread isn't loaded: count the reply on its root only (the
        // panel reads the whole thread when it opens).
        s.updateMessage(c, root, [&](model::Message &r) {
            ++r.replyCount;
            r.latestReply = std::max(r.latestReply, ts);
            if (author != kNoUser && r.replyUsers.size() < 5 &&
                std::find(r.replyUsers.begin(), r.replyUsers.end(), author) == r.replyUsers.end())
                r.replyUsers.push_back(author);
        });
        // Still news (msga fired EvMessageNew for it): what notifies.
        s.announceReply(c, m);
    } else {
        s.addMessage(c, std::move(m));
        // msga's _readingConv: the open, focused chat marks it read as it
        // lands (the shell's observer, inside addMessage). The Store has
        // recounted; count on top of that, and not this message.
        if (const model::Conversation &x = s.conversation(c); x.lastRead != lastRead) {
            unread0   = x.unread;
            mentions0 = x.mentions;
            counted   = counted && ts > x.lastRead;
        }
    }

    // msga's handleNewMessage: a reply in a thread I follow, or one that
    // mentions me, makes the Threads entry unread.
    if (!own && root && !s.threadMuted(c, root) && (parentIsMe || followed.count(key) || mention))
        noteUnreadThreadReply(c, root, ts);

    uint32_t du = 0, dm = 0;
    if (counted) {
        const bool threadMuted = root && s.threadMuted(c, root);
        const bool isFollowed  = !threadMuted && root && (parentIsMe || followed.count(key));
        if (root && !mention && !isFollowed && (threadMuted || !isDm)) {
            // lives in the thread, not the channel
        } else if (!muted) {
            du = 1;
            dm = isDm || mention || isFollowed;
        } else if (!isDm && (mention || isFollowed)) {
            du = dm = 1;
        }
    }
    const model::Conversation &now = s.conversation(c);
    if (now.unread != unread0 + du || now.mentions != mentions0 + dm || now.latest < ts)
        s.updateConversation(c, [&](model::Conversation &x) {
            x.unread   = unread0 + du;
            x.mentions = mentions0 + dm;
            x.latest   = std::max(x.latest, ts); // replies too: list relevance
        });
    return true;
}

// ── For the realtime half ───────────────────────────────────────────────────

bool SlackBackend::deliver(ConvRef c, model::Message m, bool parentIsMe) {
    return _read->inject(c, std::move(m), parentIsMe);
}

void SlackBackend::followThread(ConvRef c, Ts root) {
    if (root && _read->followed.insert(threadKey(c, root)).second)
        _read->extrasChanged();
}

ConvRef SlackBackend::mergeConversation(model::Conversation fresh) {
    if (const ConvRef r = _store.findConversation(fresh.id); r != kNoConv)
        _read->carryLocal(fresh, _store.conversation(r));
    return _store.addConversation(std::move(fresh));
}

void SlackBackend::reloadConversations() {
    _read->loadConversations([](const std::string &) {});
}

void SlackBackend::backfillOpen() {
    if (_read->openConv != kNoConv) {
        _read->lastFg = _read->now();
        _read->pollConversation(_read->openConv, true);
    }
}

void SlackBackend::reloadUsergroups() {
    _read->loadUsergroups();
}

std::vector<model::Backend::Command> SlackBackend::serverCommands() const {
    return _read->serverCommands;
}

void SlackBackend::rearmReminders() {
    _read->armReminders();
}

void SlackBackend::noteRateLimited(const std::string &method, int64_t secs) {
    _read->noteRateLimited(method, secs);
}

bool SlackBackend::isDead(const std::string &id) const {
    return _read->dead.count(id) > 0;
}

void SlackBackend::markDead(const std::string &id) {
    _read->markDead(id);
}

void SlackBackend::markAlive(const std::string &id) {
    _read->markAlive(id);
}

void SlackBackend::threadRead(ConvRef c, Ts root, Ts upTo) {
    _read->threadRead(c, root, upTo);
}

void SlackBackend::readCall(std::string method, std::string form, ApiDone done, bool background) {
    _read->call(
        std::move(method),
        std::move(form),
        std::move(done),
        background ? Read::Lane::Background : Read::Lane::Normal
    );
}

// ── Contract odds and ends ──────────────────────────────────────────────────

SlackBackend::Read *SlackBackend::newRead(SlackBackend &b) {
    return new Read(b);
}

void SlackBackend::deleteRead(Read *r) {
    delete r;
}

// msga's PublicBackend::capabilities() for the fields the shell gates on.
model::Backend::Capabilities SlackBackend::capabilities() const {
    Capabilities c;
    c.huddles          = true;
    c.replyBroadcast   = true;
    c.scheduledSend    = true;                 // chat.scheduleMessage
    c.memberList       = true;                 // conversations.members
    c.threadsView      = _creds.sessionAuth(); // subscriptions.thread.getView: xoxc only
    c.messageReminders = _creds.sessionAuth(); // saved.*: xoxc only
    c.presence         = true;                 // polled users.getPresence
    c.selfStatus       = true;
    c.canvases         = true;
    c.fileUpload       = true;                 // files.getUploadURLExternal
    c.removePreview    = _creds.sessionAuth(); // chat.deleteAttachment: internal, xoxc only
    c.slashCommands    = true;                 // the built-ins, commands.list, chat.command
    c.sidebarTheme     = _creds.sessionAuth(); // users.prefs.get: xoxc only
    return c;
}

void SlackBackend::refreshSelfPresence(std::function<void()> then) {
    _read->refreshSelfPresence(std::move(then));
}

model::Backend::SelfPresence SlackBackend::selfPresence() const {
    return _read->self;
}

int64_t SlackBackend::nowSecs() const {
    return base::nowSecs();
}

} // namespace slack
