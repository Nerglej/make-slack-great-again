// SlackBackend's write side (see slack_backend.h): msga's PublicBackend
// write methods together with the optimistic half of its Session.
//
// Every change lands in the Store first and the Web API call follows; a
// definitive rejection puts the old state back. A transport failure says
// nothing about whether a write landed, so it never rolls back (msga's
// kAmbiguousWriteFailure) — except for a send, which reconciles: Slack has
// no idempotency key, so a post that may or may not have arrived is looked
// for in recent history before it is ever posted again (the old app's
// duplicate-message bug was a blind retransmit).
#include "app/model/image_size.h"
#include "app/model/jobs.h"
#include "app/slack/slack_backend.h"
#include "app/slack/slack_json.h"
#include "base/file.h"
#include "base/i18n.h"
#include "base/log.h"
#include "base/str.h"
#include "base/time.h"
#include "plat/plat.h"

#include <algorithm>
#include <utility>

namespace slack {

using model::ConvRef;
using model::Ts;

namespace {

// Base of the send/delete/upload retry backoff (msga's _sendRetryDelayMs).
constexpr int kRetryBaseMs     = 1000;
constexpr int kRetryMaxMs      = 60000;
constexpr int kDeleteRetries   = 6; // msga's kMaxDeleteRetries
constexpr int kUploadScans     = 6; // msga's kMaxUploadReconcileRetries
constexpr int kUploadTimeoutMs = 300000;

int backoff(int attempt) {
    return std::min(kRetryBaseMs << std::min(attempt, 6), kRetryMaxMs);
}

// The request may have reached Slack or not: a transport failure (net's
// reasons, "+ : detail"), a non-JSON answer (a proxy's 5xx page), or a rate
// limit. Anything else is Slack's own verdict.
bool transient(const std::string &e) {
    static const char *const kReasons[] = {
        "dns",
        "connect",
        "tls",
        "timeout",
        "protocol",
        "too_many_redirects",
        "bad_json",
        "ratelimited",
    };
    const std::string_view head = std::string_view(e).substr(0, e.find(':'));
    for (const char *r : kReasons)
        if (head == r)
            return true;
    return false;
}

// msga's isMethodUnavailable: the endpoint itself is refused for this token
// (an internal method on OAuth, a missing scope), not a passing failure.
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

void addParam(std::string &form, std::string_view k, std::string_view v) {
    if (!form.empty())
        form += '&';
    form += net::percentEncode(k);
    form += '=';
    form += net::percentEncode(v);
}

// Slack entity-escapes bare & < > in stored text; unescape both sides so a
// sent text compares equal to its stored form (msga's unescapedText).
std::string unescaped(std::string_view t) {
    static const struct {
        std::string_view from;
        char             to;
    } kEntities[] = {{"&lt;", '<'}, {"&gt;", '>'}, {"&amp;", '&'}};
    std::string out;
    out.reserve(t.size());
    for (size_t i = 0; i < t.size(); ++i) {
        bool hit = false;
        for (const auto &e : kEntities)
            if (t.compare(i, e.from.size(), e.from) == 0) {
                out += e.to;
                i += e.from.size() - 1;
                hit = true;
                break;
            }
        if (!hit)
            out += t[i];
    }
    return std::string(str::trim(out));
}

// The pending copy's file card, from the local file (msga's uploadFiles:
// images preview straight from disk).
model::File localFile(const std::string &path) {
    model::File f;
    f.id                  = "pending:" + path;
    f.name                = std::string(file::baseName(path));
    f.path                = path;
    f.size                = std::max<int64_t>(0, file::size(path));
    const std::string ext = str::asciiLower(file::extension(f.name));
    f.prettyType          = str::asciiUpper(ext);
    if (model::imageSize(path, &f.width, &f.height))
        f.mime = str::concat({"image/", ext == "jpg" ? "jpeg" : ext});
    return f;
}

// msga's friendlySendError: Slack's code in words where there are some.
std::string friendlySendError(const std::string &e) {
    static const struct {
        const char *code, *text;
    } kText[] = {
        {"cannot_reply_to_message", N_("You can't reply to this message.")},
        {"not_in_channel", N_("You're not a member of this channel.")},
        {"is_archived", N_("This conversation is archived.")},
        {"msg_too_long", N_("The message is too long.")},
        {"channel_not_found", N_("This conversation no longer exists.")},
        {"restricted_action", N_("You don't have permission to post here.")},
        {"no_permission", N_("You don't have permission to post here.")},
    };
    for (const auto &t : kText)
        if (e == t.code)
            return i18n::tr(t.text);
    return e;
}

// msga's withReauthHint: a missing scope is fixed by signing in again.
std::string withReauthHint(std::string message, const std::string &err) {
    if (err == "missing_scope")
        message +=
            i18n::tr(" \xE2\x80\x94 sign in to this workspace again to grant the new permission");
    return message;
}

// msga's parseDndMinutes: "30", "45m", "2h", "1h 30m", "1 hour" → minutes;
// "off" / "end" / "resume" → 0; anything else (an empty argument too) → -1.
int parseDndMinutes(std::string_view args) {
    const std::string a = str::asciiLower(str::trim(args));
    if (a == "off" || a == "end" || a == "resume")
        return 0;
    // ^(?:(\d+)\s*h[a-z]*)?\s*(?:(\d+)\s*(?:m[a-z]*)?)?$
    size_t i     = 0;
    auto   space = [&] {
        while (i < a.size() && (a[i] == ' ' || a[i] == '\t'))
            ++i;
    };
    auto digits = [&](int64_t *v) {
        const size_t from = i;
        while (i < a.size() && a[i] >= '0' && a[i] <= '9' && i - from < 9)
            *v = *v * 10 + (a[i++] - '0');
        return i > from && !(i < a.size() && a[i] >= '0' && a[i] <= '9');
    };
    auto word = [&] {
        while (i < a.size() && a[i] >= 'a' && a[i] <= 'z')
            ++i;
    };
    int64_t      h = 0, m = 0;
    bool         haveH = false, haveM = false;
    const size_t start = i;
    if (digits(&h)) {
        space();
        if (i < a.size() && a[i] == 'h') {
            haveH = true;
            word();
        } else { // no "h": these digits are the minutes
            i = start;
            h = 0;
        }
    }
    space();
    if (i < a.size()) {
        if (!digits(&m))
            return -1;
        haveM = true;
        space();
        if (i < a.size() && a[i] == 'm')
            word();
    }
    if (i != a.size() || (!haveH && !haveM))
        return -1;
    const int64_t minutes = h * 60 + m;
    return minutes > 0 && minutes < 1000000 ? int(minutes) : -1;
}

} // namespace

struct SlackBackend::Write {
    explicit Write(SlackBackend &b) : b(b) {}
    ~Write() {
        for (plat::TimerId id : timers)
            b._app.cancelTimer(id);
    }

    // One send from the pending copy to the confirmed message.
    struct Send {
        ConvRef                  conv  = model::kNoConv;
        Ts                       local = 0, thread = 0;
        std::string              text;    // exactly what chat.postMessage is given
        std::string              blocks;  // its Block Kit blocks (JSON), "" = none
        std::string              oldest;  // the reconcile scan's exclusive lower bound
        std::vector<std::string> fileIds; // uploads: what the shared message carries
        bool                     broadcast = false;
        bool                     withFiles = false; // an upload (msga: "Upload failed")
        bool                     undone    = false; // taken back while in flight (undo send)
        int                      attempts  = 0;
        Done                     done;
        void                     finish(bool ok, const std::string &err) {
            if (auto cb = std::exchange(done, nullptr))
                cb(ok, err);
        }
    };
    using SendPtr = std::shared_ptr<Send>;

    SlackBackend        &b;
    std::vector<SendPtr> sends; // in flight
    // Confirmed sends, local ts → server ts: an undo (or edit) that names the
    // pending copy after it turned into the real message. Recent ones only.
    struct Confirmed {
        ConvRef conv;
        Ts      local, server;
    };
    std::vector<Confirmed>     confirmed;
    std::vector<plat::TimerId> timers;
    Ts                         lastLocal        = 0;
    bool                       savedUnavailable = false, threadMarkUnavailable = false;
    MyProfile                  profile; // the last users.profile.get (updateProfile diffs it)
    bool                       profileLoaded = false;

    model::Store      &store() { return b._store; }
    const std::string &meId() { return store().user(store().me).id; }

    void later(int ms, std::function<void()> fn) {
        auto id = std::make_shared<plat::TimerId>(0);
        *id     = b._app.addTimer(ms, false, [this, id, fn = std::move(fn)] {
            std::erase(timers, *id);
            fn();
        });
        timers.push_back(*id);
    }
    // `done` later, never from inside the call (the Backend contract).
    void post(std::function<void()> fn) {
        b._app.post([alive = b._alive, fn = std::move(fn)] {
            if (*alive)
                fn();
        });
    }

    // A request outside the form-encoded Web API (an upload's bytes, a
    // multipart photo, a file's bytes), on the transfer pool: the backend's
    // destructor cancels it.
    void raw(net::Request req, std::function<void(net::Response)> done) {
        b.transfers().send(std::move(req), std::move(done));
    }

    // A best-effort write: `undo` reverts its optimistic Store change when
    // Slack rejects it outright; `fine` is the error that means "already so".
    // Only for writes that are safe to repeat (reactions, pins, stars, mark,
    // leave, files.delete — "already so" is fine): they ride the read path,
    // so a lost connection, a gateway page or a 429 is retried with backoff
    // like msga's idempotent queue did, instead of landing only locally.
    void write(
        std::string_view      method,
        std::string           form,
        std::function<void()> undo,
        const char           *fine = nullptr
    ) {
        b.readCall(
            std::string(method),
            std::move(form),
            [m    = std::string(method),
             undo = std::move(undo),
             fine](const json::Document &, const std::string &err) {
                if (err.empty() || err == "cancelled" || (fine && err == fine))
                    return;
                LOG_WARN("slack", "%s: %s", m.c_str(), err.c_str());
                if (!transient(err) && undo)
                    undo();
            }
        );
    }

    // conversations.mark, one at a time per conversation: two in flight may
    // land in either order, and the cursor must end where the user last put
    // it (read, then "Mark unread"). While one is out, only the newest
    // waiting cursor is kept and sent once it answers.
    struct Mark {
        std::string channel, waiting; // waiting: the next ts to send ("" none)
        bool        inFlight = false;
    };
    std::vector<Mark> marks;

    void mark(const std::string &channel, std::string ts) {
        Mark *m = nullptr;
        for (auto &x : marks)
            if (x.channel == channel)
                m = &x;
        if (!m) {
            marks.push_back({channel, {}, false});
            m = &marks.back();
        }
        if (m->inFlight) {
            m->waiting = std::move(ts);
            return;
        }
        m->inFlight = true;
        // Retried like a read: a lost mark leaves the chat unread elsewhere.
        b.readCall(
            "conversations.mark",
            net::formEncode({{"channel", channel}, {"ts", ts}}),
            [this, channel](const json::Document &, const std::string &err) {
                if (!err.empty() && err != "cancelled")
                    LOG_WARN("slack", "conversations.mark: %s", err.c_str());
                for (size_t i = 0; i < marks.size(); ++i) {
                    if (marks[i].channel != channel)
                        continue;
                    marks[i].inFlight = false;
                    std::string next  = std::move(marks[i].waiting);
                    marks.erase(marks.begin() + i);
                    if (!next.empty())
                        mark(channel, std::move(next));
                    return;
                }
            }
        );
    }

    // My reaction on one message and emoji, one call at a time (msga's
    // single queue kept them in order): a retried add must never land after
    // the remove that followed it. While one is out, only the latest wanted
    // state is kept and sent once it answers, if it differs.
    struct Reaction {
        ConvRef     conv;
        Ts          ts;
        std::string name;
        int         want = -1; // the state wanted next (-1 none)
    };
    std::vector<Reaction> reactions; // in flight

    void react(ConvRef conv, Ts ts, const std::string &name, bool add) {
        for (Reaction &r : reactions)
            if (r.conv == conv && r.ts == ts && r.name == name) {
                r.want = add;
                return;
            }
        reactions.push_back({conv, ts, name, -1});
        b.readCall(
            add ? "reactions.add" : "reactions.remove",
            net::formEncode(
                {{"channel", b.convId(conv)}, {"timestamp", model::formatTs(ts)}, {"name", name}}
            ),
            [this, conv, ts, name, add](const json::Document &, const std::string &err) {
                int want = -1;
                for (size_t i = 0; i < reactions.size(); ++i)
                    if (reactions[i].conv == conv && reactions[i].ts == ts &&
                        reactions[i].name == name) {
                        want = reactions[i].want;
                        reactions.erase(reactions.begin() + long(i));
                        break;
                    }
                const bool fine = err.empty() || err == "cancelled" ||
                                  err == (add ? "already_reacted" : "no_reaction");
                if (!fine)
                    LOG_WARN("slack", "reactions.%s: %s", add ? "add" : "remove", err.c_str());
                if (want >= 0 && bool(want) != add) {
                    react(conv, ts, name, bool(want)); // the newer wish goes out now
                    return;
                }
                // Refused outright, nothing newer: the Store goes back.
                if (!fine && !transient(err))
                    b._store.setReaction(conv, ts, name, b._store.me, !add);
            }
        );
    }

    Ts nextLocalTs(ConvRef conv) {
        Ts ts = std::max(base::nowMicros(), lastLocal + 1);
        while (store().findMessage(conv, ts))
            ++ts;
        return lastLocal = ts;
    }
    Ts serverTs(ConvRef conv, Ts ts) const {
        for (const Confirmed &c : confirmed)
            if (c.conv == conv && c.local == ts)
                return c.server;
        return ts;
    }

    // ── Sending ─────────────────────────────────────────────────────────────
    SendPtr begin(ConvRef conv, std::string text, Ts thread, bool broadcast, Done done) {
        auto st       = std::make_shared<Send>();
        st->conv      = conv;
        st->thread    = thread;
        st->broadcast = broadcast && thread;
        st->done      = std::move(done);
        if (conv >= store().conversationCount()) {
            post([st] { st->finish(false, "channel_not_found"); });
            return nullptr;
        }
        if (thread)
            b.followThread(conv, thread); // msga's markThreadFollowed, on send
        // Only messages newer than the newest the server gave us can be this
        // one (msga's sinceTs); a fresh conversation falls back to the clock.
        const Ts latest = store().conversation(conv).latest;
        st->oldest      = latest ? model::formatTs(latest) : str::number(base::nowSecs() - 60);
        model::Message m;
        m.ts = st->local = nextLocalTs(conv);
        m.threadTs       = thread;
        m.user           = store().me;
        m.text           = text;
        m.pending        = true;
        if (st->broadcast) // the subtype the server gives it, so the channel shows it too
            m.extras().subtype = "thread_broadcast";
        st->text = std::move(text);
        store().addMessage(conv, std::move(m));
        sends.push_back(st);
        return st;
    }

    void postAttempt(SendPtr st) {
        std::string form;
        addParam(form, "channel", b.convId(st->conv));
        addParam(form, "text", st->text);
        if (!st->blocks.empty())
            addParam(form, "blocks", st->blocks);
        if (st->thread)
            addParam(form, "thread_ts", model::formatTs(st->thread));
        if (st->broadcast)
            addParam(form, "reply_broadcast", "true");
        b.api(
            "chat.postMessage",
            std::move(form),
            [this, st](const json::Document &doc, const std::string &err) {
                if (err.empty()) {
                    // Confirmed from the answer itself, not a later echo.
                    const json::Value r = doc.root();
                    model::Message    m = mapjson::toMessage(r["message"], store());
                    if (!m.ts)
                        m.ts = model::parseTs(r["ts"].str());
                    confirm(st, std::move(m));
                } else if (err != "cancelled") {
                    retryOrFail(st, err);
                }
            }
        );
    }

    void retryOrFail(SendPtr st, const std::string &err) {
        if (!transient(err))
            return fail(st, err);
        const int delay = backoff(st->attempts++);
        LOG_INFO("slack", "send: %s, reconciling in %d ms", err.c_str(), delay);
        later(delay, [this, st] { reconcile(st); });
    }

    // Was the lost post delivered after all? Own author + same text after
    // the anchor: confirm it; absent: safe to post again.
    void reconcile(SendPtr st) {
        std::string form;
        addParam(form, "channel", b.convId(st->conv));
        addParam(form, "oldest", st->oldest);
        addParam(form, "limit", "100");
        if (st->thread)
            addParam(form, "ts", model::formatTs(st->thread));
        b.api(
            st->thread ? "conversations.replies" : "conversations.history",
            std::move(form),
            [this, st](const json::Document &doc, const std::string &err) {
                if (err == "cancelled")
                    return;
                if (!err.empty())
                    return retryOrFail(st, err);
                const std::string want = unescaped(st->text);
                for (json::Value o : doc.root()["messages"]) {
                    const Ts ts = model::parseTs(o["ts"].str());
                    if (st->thread && ts == st->thread)
                        continue; // the root, not a reply
                    if (o["user"].str() != meId() || unescaped(o["text"].str()) != want)
                        continue;
                    LOG_INFO(
                        "slack", "send: %s was delivered after all", model::formatTs(ts).c_str()
                    );
                    return confirm(st, mapjson::toMessage(o, store()));
                }
                postAttempt(st);
            }
        );
    }

    void forget(const SendPtr &st) { std::erase(sends, st); }

    // The server's copy replaces the pending one.
    void confirm(SendPtr st, model::Message m) {
        forget(st);
        if (m.user == model::kNoUser)
            m.user = store().me;
        if (!m.threadTs && st->thread)
            m.threadTs = st->thread;
        m.pending = false;
        if (st->undone) { // undo send while in flight: the server copy goes too
            if (m.ts)
                deleteAttempt(st->conv, m.ts, 0, nullptr);
            return st->finish(false, "message_deleted");
        }
        const Ts   ts    = m.ts;
        const bool reply = m.isReply();
        store().removeMessage(st->conv, st->local);
        if (ts) {
            store().addMessage(st->conv, std::move(m));
            if (confirmed.size() >= 16)
                confirmed.erase(confirmed.begin());
            confirmed.push_back({st->conv, st->local, ts});
            if (!reply) // my own message reads the conversation up to it
                store().markRead(st->conv, ts);
        }
        st->finish(true, {});
    }

    void fail(SendPtr st, const std::string &err) {
        forget(st);
        LOG_WARN("slack", "send failed: %s", err.c_str());
        if (!st->undone) {
            store().removeMessage(st->conv, st->local);
            // msga's EvSendFailed: the ghost goes and the banner says why
            // (the text is not put back in the composer).
            if (b.onError)
                b.onError(
                    st->withFiles
                        ? i18n::arg(i18n::tr("Upload failed: %1"), err)
                        : i18n::arg(i18n::tr("Couldn't send message: %1"), friendlySendError(err))
                );
        }
        st->finish(false, err);
    }

    // ── Uploads (files.getUploadURLExternal → bytes → completeUploadExternal)
    struct Batch {
        SendPtr                  st;
        int                      pending = 0;
        std::vector<std::string> titles; // parallel to st->fileIds
    };

    // The files are read on a worker (they may be large), then each goes
    // up in the order given; the pending copy already shows them.
    void upload(SendPtr st, const std::vector<std::string> &paths) {
        struct Read {
            std::string                  path;
            std::shared_ptr<std::string> data = std::make_shared<std::string>();
            bool                         ok   = false;
        };
        auto reads = std::make_shared<std::vector<Read>>();
        for (const std::string &path : paths)
            reads->push_back({path});
        model::runInBackground(
            b._app,
            [reads] {
                for (Read &r : *reads)
                    r.ok = file::readAll(r.path, r.data.get());
            },
            [this, alive = b._alive, st, reads] {
                if (!*alive)
                    return;
                if (st->undone) { // undo send while the files were read: nothing went up
                    forget(st);
                    return st->finish(false, "message_deleted");
                }
                auto batch = std::make_shared<Batch>();
                batch->st  = st;
                for (Read &r : *reads) {
                    if (!r.ok) {
                        LOG_WARN("slack", "upload: cannot read %s", r.path.c_str());
                        continue;
                    }
                    ++batch->pending;
                    sendFile(batch, std::string(file::baseName(r.path)), std::move(r.data));
                }
                if (!batch->pending) // nothing readable: no callback will ever settle it
                    fail(st, "could not read the selected files");
            }
        );
    }

    void sendFile(
        const std::shared_ptr<Batch> &batch, std::string name, std::shared_ptr<std::string> data
    ) {
        b.api(
            "files.getUploadURLExternal",
            net::formEncode({{"filename", name}, {"length", str::number(int64_t(data->size()))}}),
            [this, batch, data, name](const json::Document &doc, const std::string &err) {
                if (err == "cancelled")
                    return;
                const std::string url(doc.root()["upload_url"].str());
                const std::string id(doc.root()["file_id"].str());
                if (!err.empty() || url.empty() || id.empty()) {
                    LOG_WARN("slack", "getUploadURLExternal: %s", err.c_str());
                    return uploaded(batch);
                }
                net::Request req; // the upload URL is pre-signed: no auth
                req.method    = "POST";
                req.url       = url;
                req.timeoutMs = kUploadTimeoutMs;
                req.body      = std::move(*data);
                req.headers.push_back({"Content-Type", "application/octet-stream"});
                raw(std::move(req), [this, batch, id, name](net::Response r) {
                    if (r.ok()) {
                        batch->st->fileIds.push_back(id);
                        batch->titles.push_back(name);
                    } else {
                        LOG_WARN("slack", "upload POST: %d %s", r.status, r.error.c_str());
                    }
                    uploaded(batch);
                });
            }
        );
    }

    void uploaded(const std::shared_ptr<Batch> &batch) {
        if (--batch->pending > 0)
            return;
        SendPtr st = batch->st;
        if (st->fileIds.empty())
            return fail(st, "file upload failed");
        json::Writer files;
        files.beginArray();
        for (size_t i = 0; i < st->fileIds.size(); ++i)
            files.beginObject()
                .key("id")
                .value(st->fileIds[i])
                .key("title")
                .value(batch->titles[i])
                .endObject();
        files.endArray();
        std::string form;
        addParam(form, "channel_id", b.convId(st->conv));
        addParam(form, "files", files.str());
        if (!st->text.empty())
            addParam(form, "initial_comment", st->text);
        if (st->thread)
            addParam(form, "thread_ts", model::formatTs(st->thread));
        // One share for all files: a retransmit would post them twice, so a
        // failure here is reported, not retried.
        b.api(
            "files.completeUploadExternal",
            std::move(form),
            [this, st](const json::Document &, const std::string &err) {
                if (err == "cancelled")
                    return;
                if (!err.empty())
                    return fail(st, err);
                // The answer has no message ts: find the share in history.
                st->finish(true, {});
                scanUpload(st);
            }
        );
    }

    // msga's reconcileUpload: my newest message carrying one of the file ids
    // replaces the pending copy; a share lags history, hence the retries.
    void scanUpload(SendPtr st) {
        std::string form;
        addParam(form, "channel", b.convId(st->conv));
        addParam(form, "oldest", str::number(base::nowSecs() - 120));
        addParam(form, "limit", "30");
        if (st->thread)
            addParam(form, "ts", model::formatTs(st->thread));
        b.api(
            st->thread ? "conversations.replies" : "conversations.history",
            std::move(form),
            [this, st](const json::Document &doc, const std::string &err) {
                if (err == "cancelled")
                    return;
                if (err.empty())
                    for (json::Value o : doc.root()["messages"]) {
                        if (o["user"].str() != meId())
                            continue;
                        for (json::Value f : o["files"])
                            if (std::find(st->fileIds.begin(), st->fileIds.end(), f["id"].str()) !=
                                st->fileIds.end())
                                return confirm(st, mapjson::toMessage(o, store()));
                    }
                if (st->attempts < kUploadScans) {
                    later(backoff(st->attempts++), [this, st] { scanUpload(st); });
                    return;
                }
                // Never seen: drop the ghost; history brings the real one.
                LOG_WARN("slack", "upload: shared message not found in history");
                forget(st);
                if (!st->undone)
                    store().removeMessage(st->conv, st->local);
            }
        );
    }

    // ── Deleting ────────────────────────────────────────────────────────────
    // chat.delete is idempotent (a repeat answers message_not_found), so an
    // ambiguous failure is simply sent again, a bounded number of times.
    // `restore` is the removed copy, put back if Slack refuses.
    void deleteAttempt(ConvRef conv, Ts ts, int attempt, std::shared_ptr<model::Message> restore) {
        b.api(
            "chat.delete",
            net::formEncode({{"channel", b.convId(conv)}, {"ts", model::formatTs(ts)}}),
            [this, conv, ts, attempt, restore](const json::Document &, const std::string &err) {
                if (err.empty() || err == "cancelled" || err == "message_not_found")
                    return;
                if (transient(err) && attempt < kDeleteRetries) {
                    later(backoff(attempt), [this, conv, ts, attempt, restore] {
                        deleteAttempt(conv, ts, attempt + 1, restore);
                    });
                    return;
                }
                LOG_WARN("slack", "chat.delete: %s", err.c_str());
                if (!transient(err) && restore)
                    putBack(conv, std::move(*restore));
            }
        );
    }

    void putBack(ConvRef conv, model::Message m) {
        const Ts                    root = m.isReply() ? m.threadTs : 0;
        std::vector<model::Message> page;
        page.push_back(std::move(m));
        store().addPage(conv, std::move(page)); // a page: no unread bump
        if (root)                               // addPage leaves the root's counter alone
            store().updateMessage(conv, root, [](model::Message &r) { ++r.replyCount; });
    }

    // ── Saved items (saved.*: "Save for later" and "Remind me") ─────────────
    // A reminder is a saved item with a due date; removing either removes
    // the whole item. Session tokens only (an internal API).
    void saved(ConvRef conv, Ts ts, bool on, int64_t due) {
        if (!b._creds.sessionAuth() || savedUnavailable)
            return;
        model::Message                *m         = store().findMessage(conv, ts);
        const bool                     wasSaved  = m && m->saved;
        const int64_t                  wasDue    = store().reminderAt(conv, ts);
        const model::Store::SavedItem *item      = store().findSaved(conv, ts);
        const bool                     wasListed = item != nullptr;
        const int64_t wasAt = item ? item->savedAt : 0, wasItemDue = item ? item->due : 0;
        store().updateMessage(conv, ts, [on](model::Message &x) { x.saved = on; });
        store().setReminderAt(conv, ts, on ? due : 0);
        store().setSavedItem(conv, ts, on, on ? due : 0);
        std::string form = net::formEncode(
            {{"item_type", "message"}, {"item_id", b.convId(conv)}, {"ts", model::formatTs(ts)}}
        );
        if (on && due > 0)
            addParam(form, "date_due", str::number(due));
        b.api(
            on ? "saved.add" : "saved.delete",
            std::move(form),
            [this, conv, ts, on, due, wasSaved, wasDue, wasListed, wasAt, wasItemDue](
                const json::Document &, const std::string &err
            ) {
                if (err.empty() || err == "cancelled" || transient(err))
                    return; // an ambiguous write may have landed: no rollback
                LOG_WARN("slack", "saved.%s: %s", on ? "add" : "delete", err.c_str());
                if (methodUnavailable(err))
                    savedUnavailable = true;
                // msga's banner: why the tint (or the Saved row) just changed back.
                if (b.onError) {
                    const bool  reminder = on ? due > 0 : (wasItemDue > 0 || wasDue > 0);
                    const char *what =
                        on ? (reminder ? N_("Couldn't set the reminder: %1")
                                       : N_("Couldn't save the message: %1"))
                           : (reminder ? N_("Couldn't remove the reminder: %1")
                                       : N_("Couldn't remove the saved message: %1"));
                    b.onError(i18n::arg(i18n::tr(what), err));
                }
                // Unless the user changed it again meanwhile.
                if ((store().findSaved(conv, ts) != nullptr) != on ||
                    store().reminderAt(conv, ts) != (on ? due : 0))
                    return;
                store().setSavedItem(conv, ts, wasListed, wasItemDue, wasAt);
                const model::Message *x = store().findMessage(conv, ts);
                if (!x || x->saved != on)
                    return;
                store().updateMessage(conv, ts, [wasSaved](model::Message &y) {
                    y.saved = wasSaved;
                });
                store().setReminderAt(conv, ts, wasDue);
            }
        );
    }

    // ── Members (conversations.members, paged) ──────────────────────────────
    struct Members {
        ConvRef                     conv;
        std::vector<model::UserRef> users;
        model::Backend::MembersDone done;
    };
    void membersPage(std::shared_ptr<Members> ms, std::string cursor) {
        std::string form;
        addParam(form, "channel", b.convId(ms->conv));
        addParam(form, "limit", "1000");
        if (!cursor.empty())
            addParam(form, "cursor", cursor);
        b.api(
            "conversations.members",
            std::move(form),
            [this, ms](const json::Document &doc, const std::string &err) {
                if (err == "cancelled")
                    return;
                if (!err.empty()) {
                    LOG_WARN("slack", "conversations.members: %s", err.c_str());
                    if (ms->done)
                        ms->done({}, err);
                    return;
                }
                const json::Value r = doc.root();
                for (json::Value u : r["members"])
                    ms->users.push_back(store().internUser(u.str()));
                const std::string next(r["response_metadata"]["next_cursor"].str());
                if (!next.empty())
                    return membersPage(ms, next);
                // The full list is a channel's count too (msga sized the next
                // popup by it): where the roster had no num_members (Grid's
                // client.userBoot, an old cache) the header's count was blank.
                const model::Conversation &c     = store().conversation(ms->conv);
                const bool                 group = c.kind == model::ConvKind::Group;
                const auto                 n     = uint32_t(ms->users.size());
                if (group ? c.members != ms->users : !c.isDirect() && c.memberCount != n)
                    store().updateConversation(ms->conv, [&](model::Conversation &x) {
                        if (group)
                            x.members = ms->users;
                        else
                            x.memberCount = n;
                    });
                if (ms->done)
                    ms->done(std::move(ms->users), {});
            }
        );
    }

    // users.setPhoto with a multipart body built off the UI thread.
    void photo(std::string body, std::string_view boundary, Done done) {
        net::Request req;
        req.method    = "POST";
        req.url       = apiBase(b._auth).append("users.setPhoto");
        req.timeoutMs = kUploadTimeoutMs;
        req.body      = std::move(body);
        req.headers.push_back(
            {"Content-Type", str::concat({"multipart/form-data; boundary=", boundary})}
        );
        addAuthHeaders(req.headers, b._auth);
        raw(std::move(req), [this, done = std::move(done)](net::Response r) {
            json::Document doc;
            std::string    err = r.error;
            if (err.empty() && !doc.parse(std::move(r.body), nullptr))
                err = "bad_json";
            if (err.empty() && !doc.root()["ok"].boolean())
                err = std::string(doc.root()["error"].str("unknown_error"));
            if (err == "cancelled")
                return;
            if (err.empty()) {
                const json::Value p = doc.root()["profile"];
                std::string       url(p["image_512"].str(p["image_192"].str()));
                if (!url.empty() && store().me != model::kNoUser) {
                    store().user(store().me).avatar = std::move(url);
                    store().usersChanged();
                }
            } else {
                LOG_WARN("slack", "users.setPhoto: %s", err.c_str());
            }
            if (done)
                done(err.empty(), err);
        });
    }

    // users.profile.set with a JSON profile; `apply` patches my User on success.
    void
    setProfile(const json::Writer &profile, std::function<void(model::User &)> apply, Done done) {
        b.api(
            "users.profile.set",
            net::formEncode({{"profile", profile.str()}}),
            [this,
             apply = std::move(apply),
             done  = std::move(done)](const json::Document &, const std::string &err) {
                if (err == "cancelled")
                    return;
                if (err.empty() && store().me != model::kNoUser) {
                    apply(store().user(store().me));
                    store().usersChanged();
                }
                if (!err.empty())
                    LOG_WARN("slack", "users.profile.set: %s", err.c_str());
                if (done)
                    done(err.empty(), err);
            }
        );
    }
};

SlackBackend::Write *SlackBackend::newWrite(SlackBackend &b) {
    return new Write(b);
}

void SlackBackend::deleteWrite(Write *w) {
    delete w;
}

// ── Messages ────────────────────────────────────────────────────────────────

void SlackBackend::send(ConvRef conv, std::string text, Ts threadTs, Done done) {
    if (auto st = _write->begin(conv, std::move(text), threadTs, false, std::move(done)))
        _write->postAttempt(std::move(st));
}

void SlackBackend::sendBroadcast(ConvRef conv, std::string text, Ts threadTs, Done done) {
    if (auto st = _write->begin(conv, std::move(text), threadTs, true, std::move(done)))
        _write->postAttempt(std::move(st));
}

void SlackBackend::sendBlocks(
    ConvRef conv, std::string text, std::string blocks, Ts threadTs, bool broadcast, Done done
) {
    if (auto st = _write->begin(conv, std::move(text), threadTs, broadcast, std::move(done))) {
        st->blocks = std::move(blocks);
        _write->postAttempt(std::move(st));
    }
}

void SlackBackend::sendWithFiles(
    ConvRef conv, std::string text, Ts threadTs, std::vector<std::string> files, Done done
) {
    if (files.empty())
        return send(conv, std::move(text), threadTs, std::move(done));
    auto st = _write->begin(conv, std::move(text), threadTs, false, std::move(done));
    if (!st)
        return;
    st->withFiles = true;
    // The pending copy shows the files at once (local previews).
    _store.updateMessage(conv, st->local, [&files](model::Message &m) {
        for (const std::string &p : files)
            m.extras().files.push_back(localFile(p));
    });
    _write->upload(std::move(st), files);
}

void SlackBackend::edit(ConvRef conv, Ts ts, std::string text) {
    editBlocks(conv, ts, std::move(text), {});
}

void SlackBackend::editBlocks(ConvRef conv, Ts ts, std::string text, std::string blocks) {
    ts                      = _write->serverTs(conv, ts);
    const model::Message *m = _store.findMessage(conv, ts);
    if (!m || m->pending)
        return;
    auto before = std::make_shared<std::pair<std::string, bool>>(m->text, m->edited);
    _store.updateMessage(conv, ts, [&text](model::Message &x) {
        x.text   = text;
        x.edited = true;
    });
    std::string form;
    addParam(form, "channel", convId(conv));
    addParam(form, "ts", model::formatTs(ts));
    addParam(form, "text", text);
    // Text without blocks makes chat.update drop the message's blocks, which
    // is right for a plain edit and why a list travels again here.
    if (!blocks.empty())
        addParam(form, "blocks", blocks);
    api("chat.update",
        std::move(form),
        [this, conv, ts, text, before](const json::Document &doc, const std::string &err) {
            if (err == "cancelled")
                return;
            if (err.empty()) {
                // Slack may normalise the text (links, mentions): keep its copy.
                const std::string_view t = doc.root()["message"]["text"].str();
                if (!t.empty() && t != text)
                    _store.updateMessage(conv, ts, [t](model::Message &x) {
                        x.text = std::string(t);
                    });
                return;
            }
            LOG_WARN("slack", "chat.update: %s", err.c_str());
            if (transient(err))
                return;
            _store.updateMessage(conv, ts, [&](model::Message &x) {
                if (x.text == text) { // unless edited again meanwhile
                    x.text   = before->first;
                    x.edited = before->second;
                }
            });
        });
}

void SlackBackend::remove(ConvRef conv, Ts ts) {
    // Still in flight (undo send): the copy goes now, the server's copy when
    // the confirmation names its ts.
    for (const auto &st : _write->sends)
        if (st->conv == conv && st->local == ts) {
            if (!st->undone) {
                st->undone = true;
                _store.removeMessage(conv, ts);
            }
            return;
        }
    ts = _write->serverTs(conv, ts);
    std::erase_if(_write->confirmed, [&](const Write::Confirmed &c) {
        return c.conv == conv && c.server == ts;
    });
    std::shared_ptr<model::Message> restore;
    if (const model::Message *m = _store.findMessage(conv, ts))
        restore = std::make_shared<model::Message>(m->clone());
    _store.removeMessage(conv, ts);
    _write->deleteAttempt(conv, ts, 0, std::move(restore));
}

void SlackBackend::react(ConvRef conv, Ts ts, std::string_view name, bool add) {
    if (!_store.setReaction(conv, ts, name, _store.me, add))
        return;
    _write->react(conv, ts, std::string(name), add);
}

// msga's Session::downloadFile: the bytes with the workspace's token (and,
// for session auth, its d cookie), then written to disk on a worker. Slack
// answers a request it doesn't accept with its sign-in page (HTML, 200):
// that is a failure, not the file.
void SlackBackend::downloadFile(const std::string &url, std::string toPath, Done done) {
    net::Request req;
    req.url       = url;
    req.timeoutMs = kUploadTimeoutMs;
    addAuthHeaders(req.headers, _auth);
    // Held here too: a workspace switch (our destructor) cancels the
    // transfer and still answers it, so the caller's background job ends.
    auto held = std::make_shared<Done>(std::move(done));
    _downloads.push_back(held);
    transfers().send(std::move(req), [this, held, toPath = std::move(toPath)](net::Response r) {
        std::erase(_downloads, held);
        Done done = std::exchange(*held, nullptr);
        if (r.error == "cancelled") {
            if (done)
                done(false, "cancelled");
            return;
        }
        std::string err = r.error;
        if (err.empty() && !r.ok())
            err = str::concat({"http ", std::to_string(r.status)});
        if (err.empty() && str::startsWith(r.header("Content-Type"), "text/html") &&
            r.body.find("<html") != std::string::npos &&
            r.url.find("files.slack.com") != std::string::npos)
            err = "not_authed";
        if (!err.empty()) {
            LOG_WARN("slack", "download: %s", err.c_str());
            if (done)
                done(false, err);
            return;
        }
        auto body = std::make_shared<std::string>(std::move(r.body));
        auto ok   = std::make_shared<bool>(false);
        model::runInBackground(
            _app,
            [body, ok, toPath] { *ok = file::writeAtomic(toPath, *body); },
            [ok, done] {
                if (done) // the file is written either way: no alive check
                    done(*ok, *ok ? std::string() : std::string("write_failed"));
            }
        );
    });
}

void SlackBackend::deleteFile(ConvRef conv, Ts ts, const std::string &fileId) {
    auto   removed = std::make_shared<model::File>();
    size_t at      = SIZE_MAX;
    _store.updateMessage(conv, ts, [&](model::Message &m) {
        if (!m.extra)
            return;
        auto &fs = m.extra->files;
        for (size_t i = 0; i < fs.size(); ++i)
            if (fs[i].id == fileId) {
                *removed = std::move(fs[i]);
                fs.erase(fs.begin() + long(i));
                at = i;
                break;
            }
    });
    if (at == SIZE_MAX)
        return;
    _write->write(
        "files.delete", net::formEncode({{"file", fileId}}), [this, conv, ts, removed, at] {
            _store.updateMessage(conv, ts, [&](model::Message &m) {
                auto &fs = m.extras().files;
                fs.insert(fs.begin() + long(std::min(at, fs.size())), *removed);
            });
        }
    );
}

void SlackBackend::deleteAttachment(ConvRef conv, Ts ts, int attachmentId, Done done) {
    // The official client's "Remove preview": the internal chat.deleteAttachment
    // (channel, ts, attachment = the 1-based positional id; Slack renumbers
    // the rest). Own messages only, a session token only. No retry: a lost
    // answer is ambiguous and a re-send could hit a renumbered id.
    if (conv >= _store.conversationCount() || attachmentId <= 0) {
        if (done)
            _app.post([done] { done(false, "invalid_attachment"); });
        return;
    }
    api("chat.deleteAttachment",
        net::formEncode(
            {{"channel", convId(conv)},
             {"ts", model::formatTs(ts)},
             {"attachment", std::to_string(attachmentId)}}
        ),
        [this, conv, ts, attachmentId, done = std::move(done)](
            const json::Document &, const std::string &err
        ) {
            if (err == "cancelled")
                return;
            if (!err.empty()) {
                LOG_WARN("slack", "chat.deleteAttachment: %s", err.c_str());
                if (done)
                    done(false, err);
                return;
            }
            // Gone for good: the rest keep their order and are renumbered.
            _store.updateMessage(conv, ts, [attachmentId](model::Message &m) {
                if (!m.extra)
                    return;
                auto &as = m.extra->attachments;
                for (size_t i = 0; i < as.size(); ++i)
                    if ((as[i].id ? as[i].id : int(i) + 1) == attachmentId) {
                        as.erase(as.begin() + long(i));
                        break;
                    }
                for (size_t i = 0; i < as.size(); ++i)
                    if (as[i].id)
                        as[i].id = int32_t(i) + 1;
            });
            if (done)
                done(true, {});
        });
}

void SlackBackend::setPinned(ConvRef conv, Ts ts, bool pinned) {
    const model::UserRef me  = _store.me;
    auto                 set = [this, conv, ts, me](bool on) {
        _store.updateMessage(conv, ts, [on, me](model::Message &m) {
            m.pinned   = on;
            m.pinnedBy = on ? me : model::kNoUser;
        });
    };
    if (!_store.findMessage(conv, ts))
        return;
    set(pinned);
    _write->write(
        pinned ? "pins.add" : "pins.remove",
        net::formEncode({{"channel", convId(conv)}, {"timestamp", model::formatTs(ts)}}),
        [set, pinned] { set(!pinned); },
        pinned ? "already_pinned" : "no_pin"
    );
}

void SlackBackend::setSaved(ConvRef conv, Ts ts, bool saved) {
    _write->saved(conv, ts, saved, 0);
}

void SlackBackend::setReminder(ConvRef conv, Ts ts, int64_t dueSecs) {
    _write->saved(conv, ts, dueSecs > 0, dueSecs);
    rearmReminders();
}

void SlackBackend::scheduleMessage(
    ConvRef conv, std::string text, Ts thread, int64_t postAt, Done done
) {
    scheduleBlocks(conv, std::move(text), {}, thread, postAt, std::move(done));
}

void SlackBackend::scheduleBlocks(
    ConvRef conv, std::string text, std::string blocks, Ts thread, int64_t postAt, Done done
) {
    std::string form;
    addParam(form, "channel", convId(conv));
    addParam(form, "text", text);
    if (!blocks.empty())
        addParam(form, "blocks", blocks);
    if (thread)
        addParam(form, "thread_ts", model::formatTs(thread));
    addParam(form, "post_at", str::number(postAt));
    api("chat.scheduleMessage",
        std::move(form),
        [done = std::move(done)](const json::Document &, const std::string &err) {
            if (err == "cancelled")
                return;
            if (!err.empty())
                LOG_WARN("slack", "chat.scheduleMessage: %s", err.c_str());
            if (done)
                done(err.empty(), err);
        });
}

void SlackBackend::userTyping(ConvRef, Ts) {
    // users.typing is gone from the Web API (msga's sendTyping is a no-op
    // too); official clients send typing over their RTM socket.
}

// ── Read cursors ────────────────────────────────────────────────────────────

void SlackBackend::markRead(ConvRef conv, Ts ts) {
    if (conv >= _store.conversationCount() || ts <= 0)
        return;
    const model::Message *m = _store.findMessage(conv, ts);
    if (m && m->isReply()) {
        markThreadRead(conv, m->threadTs, ts);
        return;
    }
    if (ts <= _store.conversation(conv).lastRead)
        return; // read already: nothing to tell Slack
    _store.markRead(conv, ts);
    _write->mark(convId(conv), model::formatTs(ts));
}

void SlackBackend::markThreadRead(ConvRef conv, Ts root, Ts ts) {
    // A thread's own read cursor (msga's markThreadRead): an internal
    // method, session tokens only; best effort.
    if (conv >= _store.conversationCount() || !root || ts <= 0)
        return;
    threadRead(conv, root, ts); // the Threads entry
    if (!_creds.sessionAuth() || _write->threadMarkUnavailable)
        return;
    api("subscriptions.thread.mark",
        net::formEncode(
            {{"channel", convId(conv)},
             {"thread_ts", model::formatTs(root)},
             {"ts", model::formatTs(ts)}}
        ),
        [this](const json::Document &, const std::string &err) {
            if (!err.empty() && err != "cancelled") {
                LOG_WARN("slack", "subscriptions.thread.mark: %s", err.c_str());
                if (methodUnavailable(err))
                    _write->threadMarkUnavailable = true;
            }
        });
}

void SlackBackend::markUnread(ConvRef conv, Ts ts) {
    // The official client's "Mark unread": the cursor moves to just before
    // the message (conversations.mark takes any ts).
    if (conv >= _store.conversationCount() || ts <= 0)
        return;
    _store.markUnread(conv, ts);
    _write->mark(convId(conv), model::formatTs(ts - 1));
}

// ── Conversations ───────────────────────────────────────────────────────────

void SlackBackend::setStarred(ConvRef conv, bool starred) {
    if (conv >= _store.conversationCount())
        return;
    auto set = [this, conv](bool on) {
        _store.updateConversation(conv, [on](model::Conversation &c) { c.starred = on; });
    };
    set(starred);
    _write->write(
        starred ? "stars.add" : "stars.remove",
        net::formEncode({{"channel", convId(conv)}}),
        [set, starred] { set(!starred); },
        starred ? "already_starred" : "not_starred"
    );
}

// Mute and the notification level are msga's own: Slack's per-channel prefs
// aren't reachable over the public API, so the old app kept them locally
// (Session::setNotificationLevel / setConvMuted) and so does the Store.
void SlackBackend::setMuted(ConvRef conv, bool muted) {
    if (conv < _store.conversationCount())
        _store.updateConversation(conv, [muted](model::Conversation &c) { c.muted = muted; });
}

void SlackBackend::setNotifyLevel(ConvRef conv, model::NotifyLevel level) {
    if (conv < _store.conversationCount())
        _store.updateConversation(conv, [level](model::Conversation &c) { c.notify = level; });
}

void SlackBackend::leave(ConvRef conv) {
    if (conv >= _store.conversationCount())
        return;
    const bool wasStarred = _store.conversation(conv).starred;
    _store.updateConversation(conv, [](model::Conversation &c) {
        c.member  = false;
        c.starred = false; // Slack drops a left channel from Starred too
    });
    // A DM or group DM is closed, not left (conversations.leave refuses IMs).
    _write->write(
        _store.conversation(conv).isDirect() ? "conversations.close" : "conversations.leave",
        net::formEncode({{"channel", convId(conv)}}),
        [this, conv, wasStarred] {
            _store.updateConversation(conv, [wasStarred](model::Conversation &c) {
                c.member  = true;
                c.starred = wasStarred;
            });
        }
    );
}

void SlackBackend::openDm(model::UserRef user, std::function<void(ConvRef)> done) {
    if (user >= _store.userCount()) {
        _write->post([done] {
            if (done)
                done(model::kNoConv);
        });
        return;
    }
    for (ConvRef c = 0; c < _store.conversationCount(); ++c) {
        const model::Conversation &cv = _store.conversation(c);
        if (cv.kind == model::ConvKind::Dm && cv.dmUser == user && cv.member) {
            _write->post([done, c] {
                if (done)
                    done(c);
            });
            return;
        }
    }
    // conversations.open creates the DM or reopens a closed one.
    api("conversations.open",
        net::formEncode({{"users", _store.user(user).id}}),
        [this, user, done](const json::Document &doc, const std::string &err) {
            if (err == "cancelled")
                return;
            const std::string id(doc.root()["channel"]["id"].str());
            ConvRef           found = model::kNoConv;
            if (err.empty() && !id.empty()) {
                markAlive(id);
                found = _store.findConversation(id);
                if (found == model::kNoConv) {
                    model::Conversation c; // the roster refresh fills in the rest
                    c.id          = id;
                    c.name        = _store.user(user).name;
                    c.kind        = model::ConvKind::Dm;
                    c.dmUser      = user;
                    c.memberCount = 2;
                    found         = _store.addConversation(std::move(c));
                } else {
                    _store.updateConversation(found, [](model::Conversation &c) {
                        c.member = true;
                    });
                }
            } else {
                LOG_WARN("slack", "conversations.open: %s", err.c_str());
                // msga's Session::openDm: People, "Message", /dm — the
                // banner says why (showNetworkError, the raw error).
                if (onError && !err.empty())
                    onError(err);
            }
            if (done)
                done(found);
        });
}

// msga's joinChannel: conversations.join, then the channel is a member one
// (the old app reloaded the roster; only the membership changes).
void SlackBackend::joinChannel(ConvRef conv, ConvDone done) {
    if (conv >= _store.conversationCount()) {
        _write->post([done] {
            if (done)
                done(model::kNoConv, "channel_not_found");
        });
        return;
    }
    api("conversations.join",
        net::formEncode({{"channel", convId(conv)}}),
        [this, conv, done](const json::Document &doc, const std::string &err) {
            if (err == "cancelled")
                return;
            if (!err.empty()) {
                LOG_WARN("slack", "conversations.join: %s", err.c_str());
                if (done)
                    done(model::kNoConv, err);
                return;
            }
            const auto n = uint32_t(doc.root()["channel"]["num_members"].integer());
            markAlive(convId(conv));
            _store.updateConversation(conv, [n](model::Conversation &c) {
                c.member      = true;
                c.memberCount = std::max(n, c.memberCount);
            });
            if (done)
                done(conv, {});
        });
}

// msga's createChannel: conversations.create; the answer is the new channel.
void SlackBackend::createChannel(std::string name, bool isPrivate, ConvDone done) {
    api("conversations.create",
        net::formEncode({{"name", name}, {"is_private", isPrivate ? "true" : "false"}}),
        [this, done](const json::Document &doc, const std::string &err) {
            if (err == "cancelled")
                return;
            model::Conversation c;
            if (err.empty())
                c = mapjson::toConversation(doc.root()["channel"], _store);
            if (c.id.empty()) {
                LOG_WARN("slack", "conversations.create: %s", err.c_str());
                if (done)
                    done(model::kNoConv, err.empty() ? std::string("invalid_response") : err);
                return;
            }
            c.member        = true;
            c.hasMoreBefore = false;                                // nothing posted yet
            c.memberCount   = std::max<uint32_t>(c.memberCount, 1); // just me
            const ConvRef r = _store.addConversation(std::move(c));
            if (done)
                done(r, {});
        });
}

void SlackBackend::loadMembers(ConvRef conv, MembersDone done) {
    if (conv >= _store.conversationCount()) {
        _write->post([done] {
            if (done)
                done({}, {});
        });
        return;
    }
    auto ms  = std::make_shared<Write::Members>();
    ms->conv = conv;
    ms->done = std::move(done);
    _write->membersPage(std::move(ms), {});
}

// ── Me: presence, status, profile ──────────────────────────────────────────

void SlackBackend::setPresence(bool away, Done done) {
    // "auto" lets Slack decide — which, for a session workspace with no
    // official client connected, is away. msga held an rtm.connect socket
    // (RtmPresence) to look active; that is not ported yet.
    api("users.setPresence",
        net::formEncode({{"presence", away ? "away" : "auto"}}),
        [this, away, done = std::move(done)](const json::Document &, const std::string &err) {
            if (err == "cancelled")
                return;
            if (!err.empty()) {
                LOG_WARN("slack", "users.setPresence: %s", err.c_str());
                if (done)
                    done(false, err);
                return;
            }
            if (_store.me != model::kNoUser) {
                _store.user(_store.me).active = !away;
                _store.usersChanged();
            }
            // msga's Session::setPresence: the rich snapshot (manual_away…)
            // only comes from the server — re-poll instead of guessing, and
            // answer once it is in, so the footer settles on the new state.
            refreshSelfPresence([done] {
                if (done)
                    done(true, {});
            });
        });
}

void SlackBackend::setStatus(std::string emoji, std::string text, int64_t expiry, Done done) {
    // The model keeps the bare name; Slack wants ":name:".
    json::Writer p;
    p.beginObject()
        .key("status_text")
        .value(text)
        .key("status_emoji")
        .value(emoji.empty() ? std::string() : str::concat({":", emoji, ":"}))
        .key("status_expiration")
        .value(expiry)
        .endObject();
    _write->setProfile(
        p,
        [emoji = std::move(emoji), text = std::move(text)](model::User &u) {
            u.statusEmoji = emoji;
            u.statusText  = text;
        },
        std::move(done)
    );
}

void SlackBackend::loadMyProfile(std::function<void(MyProfile)> done) {
    api("users.profile.get",
        {},
        [this, done = std::move(done)](const json::Document &doc, const std::string &err) {
            if (err == "cancelled")
                return;
            if (!err.empty()) { // what the Store knows, rather than an empty form
                LOG_WARN("slack", "users.profile.get: %s", err.c_str());
                return Backend::loadMyProfile(done);
            }
            const json::Value p = doc.root()["profile"];
            MyProfile         mp;
            mp.displayName        = std::string(p["display_name"].str());
            mp.realName           = std::string(p["real_name"].str());
            mp.email              = std::string(p["email"].str());
            mp.phone              = std::string(p["phone"].str());
            mp.avatar             = std::string(p["image_512"].str(p["image_192"].str()));
            _write->profile       = mp;
            _write->profileLoaded = true;
            if (done)
                done(std::move(mp));
        });
}

void SlackBackend::updateProfile(
    std::string name, std::string email, std::string phone, Done done
) {
    // Only the fields that changed (msga's profile dialog): an unchanged
    // email would still need the admin rights to set it.
    const MyProfile &was = _write->profile;
    const bool       all = !_write->profileLoaded;
    const struct {
        const char        *key;
        const std::string &now, &before;
    } fields[] = {
        {"display_name", name, was.displayName},
        {"email", email, was.email},
        {"phone", phone, was.phone},
    };
    json::Writer p;
    p.beginObject();
    bool any = false;
    for (const auto &f : fields)
        if (all || f.now != f.before) {
            p.key(f.key).value(f.now);
            any = true;
        }
    p.endObject();
    if (!any) {
        _write->post([done] {
            if (done)
                done(true, {});
        });
        return;
    }
    _write->setProfile(
        p,
        [this, name, email, phone](model::User &u) {
            u.displayName               = name;
            u.email                     = email;
            _write->profile.displayName = name;
            _write->profile.email       = email;
            _write->profile.phone       = phone;
        },
        bannerOnFailure(N_("Could not update profile: %1"), std::move(done))
    );
}

// msga's Session: a failed profile or photo change also reaches the error
// banner, with the re-auth hint for a missing scope; `done` hears it as well.
SlackBackend::Done SlackBackend::bannerOnFailure(const char *what, Done done) {
    return [this, alive = _alive, what, done = std::move(done)](bool ok, const std::string &err) {
        if (!ok && *alive && onError)
            onError(withReauthHint(i18n::arg(i18n::tr(what), err), err));
        if (done)
            done(ok, err);
    };
}

void SlackBackend::setPhoto(std::string path, Done done) {
    done = bannerOnFailure(N_("Could not update avatar: %1"), std::move(done));
    // users.setPhoto wants multipart/form-data with the image as "image";
    // the file is read and the body built on a worker.
    static constexpr std::string_view kBoundary = "msga-photo-7d4a19c2e8b3";
    auto                              body      = std::make_shared<std::string>();
    auto                              ok        = std::make_shared<bool>(false);
    model::runInBackground(
        _app,
        [path = std::move(path), body, ok] {
            std::string data;
            if (!file::readAll(path, &data))
                return;
            *body = str::concat(
                {"--",
                 kBoundary,
                 "\r\nContent-Disposition: form-data; name=\"image\"; filename=\"",
                 file::baseName(path),
                 "\"\r\nContent-Type: application/octet-stream\r\n\r\n"}
            );
            *body += data;
            *body += str::concat({"\r\n--", kBoundary, "--\r\n"});
            *ok = true;
        },
        [this, alive = _alive, body, ok, done = std::move(done)]() mutable {
            if (!*alive)
                return;
            if (!*ok) {
                if (done)
                    done(false, "cannot_open_file");
                return;
            }
            _write->photo(std::move(*body), kBoundary, std::move(done));
        }
    );
}

// ── Search ──────────────────────────────────────────────────────────────────

void SlackBackend::search(std::string query, std::function<void(std::vector<SearchHit>)> done) {
    // The contract is newest first, hence the timestamp sort (msga took
    // Slack's default "score" order).
    api("search.messages",
        net::formEncode(
            {{"query", query}, {"count", "20"}, {"sort", "timestamp"}, {"sort_dir", "desc"}}
        ),
        [this, done = std::move(done)](const json::Document &doc, const std::string &err) {
            if (err == "cancelled")
                return;
            std::vector<SearchHit> hits;
            if (!err.empty())
                LOG_WARN("slack", "search.messages: %s", err.c_str());
            for (json::Value m : doc.root()["messages"]["matches"]) {
                SearchHit h;
                h.conv = _store.findConversation(m["channel"]["id"].str());
                h.ts   = model::parseTs(m["ts"].str());
                if (h.conv == model::kNoConv || !h.ts)
                    continue; // a conversation the roster doesn't have
                // A reply names its thread in thread_ts, or only in the
                // permalink's query ("…?thread_ts=…&cid=…").
                h.thread = model::parseTs(m["thread_ts"].str());
                if (!h.thread) {
                    const std::string_view link = m["permalink"].str();
                    const size_t           q    = link.find('?');
                    if (q != std::string_view::npos)
                        h.thread = model::parseTs(net::queryValue(link.substr(q + 1), "thread_ts"));
                }
                if (h.thread == h.ts)
                    h.thread = 0; // a root is a top-level hit
                h.text = m["text"].str();
                hits.push_back(h);
            }
            if (done)
                done(std::move(hits));
        });
}

// ── Channel canvases ────────────────────────────────────────────────────────
// msga's PublicBackend canvas calls. The Store follows each answer: the
// conversation's canvasId / canvasTitle, which the header's tab shows.

namespace {

ConvRef canvasConv(const model::Store &s, std::string_view fileId) {
    for (ConvRef r = 0; r < s.conversationCount(); ++r)
        if (s.conversation(r).canvasId == fileId)
            return r;
    return model::kNoConv;
}

// fileId "" drops the canvas; title "" keeps the one known.
void setCanvas(model::Store &s, ConvRef conv, std::string fileId, std::string title) {
    if (conv == model::kNoConv)
        return;
    const model::Conversation &c = s.conversation(conv);
    if (fileId.empty())
        title.clear();
    else if (title.empty())
        title = c.canvasTitle.empty() ? std::string("Canvas") : c.canvasTitle;
    if (c.canvasId == fileId && c.canvasTitle == title)
        return;
    s.updateConversation(conv, [&](model::Conversation &x) {
        x.canvasId    = std::move(fileId);
        x.canvasTitle = std::move(title);
    });
}

std::string markdownContent(std::string_view markdown) {
    json::Writer w;
    w.beginObject().key("type").value("markdown").key("markdown").value(markdown).endObject();
    return w.take();
}

} // namespace

void SlackBackend::loadChannelCanvas(ConvRef conv, std::function<void(std::string)> done) {
    const std::string &id = convId(conv);
    if (id.empty()) {
        _write->post([done] {
            if (done)
                done({});
        });
        return;
    }
    api("conversations.info",
        net::formEncode({{"channel", id}}),
        [this, conv, done = std::move(done)](const json::Document &doc, const std::string &err) {
            if (err == "cancelled")
                return;
            std::string fileId;
            if (err.empty()) {
                fileId = mapjson::toConversation(doc.root()["channel"], _store).canvasId;
                setCanvas(_store, conv, fileId, {});
            } else {
                // Unknown: keep what the Store has.
                LOG_WARN("slack", "conversations.info (canvas): %s", err.c_str());
                fileId = _store.conversation(conv).canvasId;
            }
            if (done)
                done(std::move(fileId));
        });
}

void SlackBackend::loadCanvasMeta(const std::string &fileId, CanvasMetaDone done) {
    api("files.info",
        net::formEncode({{"file", fileId}}),
        [this, fileId, done = std::move(done)](const json::Document &doc, const std::string &err) {
            if (err == "cancelled")
                return;
            if (err.empty()) {
                const json::Value f = doc.root()["file"];
                std::string       title(f["title"].str());
                setCanvas(_store, canvasConv(_store, fileId), fileId, title);
                if (done)
                    done(std::move(title), std::string(f["permalink"].str()), CanvasState::Ok);
                return;
            }
            // Gone / NoAccess are routine (deleted elsewhere while
            // conversations.info still names it; a channel not joined).
            CanvasState state = CanvasState::Ok;
            if (err == "file_deleted" || err == "file_not_found")
                state = CanvasState::Gone;
            else if (err == "not_visible")
                state = CanvasState::NoAccess;
            else
                LOG_WARN("slack", "files.info (canvas): %s", err.c_str());
            if (state == CanvasState::Gone)
                setCanvas(_store, canvasConv(_store, fileId), {}, {});
            if (done)
                done({}, {}, state);
        });
}

void SlackBackend::loadCanvasContent(const std::string &fileId, CanvasHtmlDone done) {
    // files.info → url_private → an authed GET: canvases come back as HTML
    // (<div class="quip-canvas-content">…, blocks carrying section ids).
    api("files.info",
        net::formEncode({{"file", fileId}}),
        [this, done = std::move(done)](const json::Document &doc, const std::string &err) {
            if (err == "cancelled")
                return;
            const std::string url(doc.root()["file"]["url_private"].str());
            if (!err.empty() || url.empty()) {
                if (done)
                    done({}, err.empty() ? std::string("no_url") : err);
                return;
            }
            net::Request req;
            req.url = url;
            addAuthHeaders(req.headers, _auth);
            _write->raw(std::move(req), [done](net::Response r) {
                if (r.error == "cancelled")
                    return;
                std::string e = r.error;
                if (e.empty() && !r.ok())
                    e = str::concat({"http ", std::to_string(r.status)});
                if (!e.empty())
                    LOG_WARN("slack", "canvas download: %s", e.c_str());
                if (done)
                    done(e.empty() ? std::move(r.body) : std::string(), e);
            });
        });
}

void SlackBackend::createChannelCanvas(ConvRef conv, std::string markdown, CanvasCreated done) {
    std::string form;
    addParam(form, "channel_id", convId(conv));
    if (!markdown.empty())
        addParam(form, "document_content", markdownContent(markdown));
    api("conversations.canvases.create",
        std::move(form),
        [this, conv, done = std::move(done)](const json::Document &doc, const std::string &err) {
            if (err == "cancelled")
                return;
            std::string fileId(doc.root()["canvas_id"].str());
            if (!err.empty())
                LOG_WARN("slack", "conversations.canvases.create: %s", err.c_str());
            else
                setCanvas(_store, conv, fileId, "Untitled");
            if (done)
                done(err.empty() ? std::move(fileId) : std::string(), err);
        });
}

void SlackBackend::editCanvas(
    const std::string &fileId, std::vector<CanvasChange> changes, Done done
) {
    // canvases.edit takes one change per call ("no more than 1 items
    // allowed", verified by msga): sent one by one, in order.
    if (changes.empty()) {
        _write->post([done] {
            if (done)
                done(true, {});
        });
        return;
    }
    const CanvasChange c = std::move(changes.front());
    changes.erase(changes.begin());
    using Op                        = CanvasChange::Op;
    const bool               rename = c.op == Op::Rename;
    static const char *const kOps[] = {
        "rename", "replace", "replace", "delete", "insert_before", "insert_after"
    };
    json::Writer w;
    w.beginArray().beginObject().key("operation").value(kOps[int(c.op)]);
    if (!c.sectionId.empty())
        w.key("section_id").value(c.sectionId);
    if (c.op != Op::DeleteSection) {
        w.key(rename ? "title_content" : "document_content").beginObject();
        w.key("type").value("markdown").key("markdown").value(c.markdown);
        w.endObject();
    }
    w.endObject().endArray();
    std::string form;
    addParam(form, "canvas_id", fileId);
    addParam(form, "changes", w.str());
    api("canvases.edit",
        std::move(form),
        [this,
         fileId,
         title   = rename ? c.markdown : std::string(),
         changes = std::move(changes),
         done    = std::move(done)](const json::Document &, const std::string &err) mutable {
            if (err == "cancelled")
                return;
            if (!err.empty()) {
                LOG_WARN("slack", "canvases.edit: %s", err.c_str());
                if (done)
                    done(false, err);
                return;
            }
            if (!title.empty())
                setCanvas(_store, canvasConv(_store, fileId), fileId, title);
            editCanvas(fileId, std::move(changes), std::move(done));
        });
}

void SlackBackend::deleteCanvas(const std::string &fileId, Done done) {
    api("canvases.delete",
        net::formEncode({{"canvas_id", fileId}}),
        [this, fileId, done = std::move(done)](const json::Document &, const std::string &err) {
            if (err == "cancelled")
                return;
            if (err.empty())
                setCanvas(_store, canvasConv(_store, fileId), {}, {});
            else
                LOG_WARN("slack", "canvases.delete: %s", err.c_str());
            if (done)
                done(err.empty(), err);
        });
}

// ── Slash commands (msga's Session::runCommand) ─────────────────────────────

namespace {

// msga's CommonCommands for Slack (nativeCommands), in its order.
const struct {
    const char *name, *desc, *usage;
} kNativeCommands[] = {
    {"shrug", N_("Appends \xC2\xAF\\_(\xE3\x83\x84)_/\xC2\xAF to your message"), N_("[message]")},
    {"mute", N_("Mute or unmute a channel"), ""},
    {"active", N_("Set yourself to active"), ""},
    {"away", N_("Toggle your away status"), ""},
    {"dnd", N_("Pause or resume notifications"), N_("[duration, e.g. 30m or 2h] or off")},
    {"status", N_("Set or clear your status"), N_("[:emoji:] [text] or clear")},
    {"msg", N_("Send a direct message"), N_("@user [message]")},
    {"dm", N_("Send a direct message"), N_("@user [message]")},
    {"leave", N_("Leave a channel or conversation"), ""},
};

} // namespace

// The workspace's own commands (commands.list) first, then the built-ins it
// doesn't already have (msga's command merge). Every one runs here: none is
// posted as a message.
std::vector<model::Backend::Command> SlackBackend::commands(ConvRef) {
    std::vector<Command> out = serverCommands();
    for (const auto &n : kNativeCommands) {
        const bool dup = std::any_of(out.begin(), out.end(), [&](const Command &c) {
            return str::asciiLower(c.name) == n.name;
        });
        if (dup)
            continue;
        Command c;
        c.name   = n.name;
        c.desc   = i18n::tr(n.desc);
        c.usage  = *n.usage ? i18n::tr(n.usage) : "";
        c.local  = true;
        c.source = "Slack";
        out.push_back(std::move(c));
    }
    return out;
}

model::Backend::LocalResult SlackBackend::runLocalCommand(
    ConvRef conv, Ts thread, const std::string &name, const std::string &args
) {
    LocalResult       r;
    const std::string cmd    = str::asciiLower(name);
    // Every answer that isn't immediate reaches the error banner (msga's
    // errorHub); success says nothing.
    auto              failed = [this](std::string message) {
        if (onError)
            onError(message);
    };
    if (cmd == "shrug") {
        static constexpr std::string_view kShrug = "\xC2\xAF\\_(\xE3\x83\x84)_/\xC2\xAF";
        // Typed in a thread: the reply goes there (msga's sendMessage(…, root)).
        send(
            conv,
            args.empty() ? std::string(kShrug) : str::concat({args, " ", kShrug}),
            thread,
            nullptr
        );
    } else if (cmd == "msg" || cmd == "dm") {
        std::string_view  a   = str::trim(args);
        const size_t      sp  = a.find(' ');
        std::string_view  who = a.substr(0, sp);
        const std::string rest(
            sp == std::string_view::npos ? std::string_view() : str::trim(a.substr(sp + 1))
        );
        model::UserRef target = model::kNoUser;
        if (str::startsWith(who, "<@") && who.size() > 3 && who.back() == '>') {
            std::string_view id = who.substr(2, who.size() - 3);
            id                  = id.substr(0, id.find('|'));
            target              = _store.findUser(id);
        } else {
            const std::string want =
                str::asciiLower(str::startsWith(who, "@") ? who.substr(1) : who);
            for (size_t i = 0; i < _store.userCount() && target == model::kNoUser && !want.empty();
                 ++i) {
                const model::User &u = _store.user(model::UserRef(i));
                if (str::asciiLower(u.name) == want || str::asciiLower(u.displayName) == want)
                    target = model::UserRef(i);
            }
        }
        if (target == model::kNoUser) {
            r.error = i18n::arg(i18n::tr("No such user: %1"), who);
            return r;
        }
        openDm(target, [this, rest](ConvRef dm) {
            if (dm != model::kNoConv && !rest.empty())
                send(dm, rest, 0, nullptr);
        });
    } else if (cmd == "leave") {
        leave(conv);
    } else if (cmd == "mute") {
        if (conv < _store.conversationCount())
            setMuted(conv, !_store.conversation(conv).muted);
    } else if (cmd == "away" || cmd == "active") {
        // /away toggles like the official client; /active is never away.
        const bool away = cmd == "away" && !selfPresence().manualAway;
        setPresence(away, [failed](bool ok, const std::string &err) {
            if (!ok)
                failed(
                    withReauthHint(i18n::arg(i18n::tr("Could not change presence: %1"), err), err)
                );
        });
    } else if (cmd == "status") {
        const std::string a(str::trim(args));
        std::string       emoji, text = a;
        if (str::asciiLower(a) == "clear") {
            text.clear();
        } else if (!a.empty() && a[0] == ':') {
            if (const size_t end = a.find(':', 1); end != std::string::npos && end > 1) {
                emoji = a.substr(1, end - 1);
                text  = std::string(str::trim(std::string_view(a).substr(end + 1)));
            }
        }
        setStatus(emoji, text, 0, [failed](bool ok, const std::string &err) {
            if (!ok)
                failed(withReauthHint(i18n::arg(i18n::tr("Could not set status: %1"), err), err));
        });
    } else if (cmd == "dnd") {
        const int minutes = parseDndMinutes(args);
        if (minutes < 0) {
            r.error = i18n::tr(
                "Usage: /dnd [duration, e.g. 30m or 2h] \xE2\x80\x94 or /dnd off to resume"
            );
            return r;
        }
        setDndSnooze(minutes, [failed](bool ok, const std::string &err) {
            if (!ok)
                failed(withReauthHint(
                    i18n::arg(i18n::tr("Could not update notifications: %1"), err), err
                ));
        });
    } else {
        // A workspace or app command: chat.command, never retried (a repeat
        // would run it twice).
        std::string form;
        addParam(form, "channel", convId(conv));
        addParam(form, "command", "/" + cmd);
        if (!args.empty())
            addParam(form, "text", args);
        // In a thread, as the official client sends it: the app answers there.
        if (thread)
            addParam(form, "thread_ts", model::formatTs(thread));
        api("chat.command",
            std::move(form),
            [failed, cmd](const json::Document &, const std::string &err) {
                if (!err.empty() && err != "cancelled")
                    failed(i18n::arg(i18n::tr("Command /%1 failed: %2"), cmd, err));
            });
    }
    return r;
}

// msga's setDndSnooze: dnd.setSnooze for a number of minutes, dnd.endSnooze
// for 0; my User shows it at once (the dnd_updated_user echo agrees).
void SlackBackend::setDndSnooze(int minutes, Done done) {
    api(minutes > 0 ? "dnd.setSnooze" : "dnd.endSnooze",
        minutes > 0 ? net::formEncode({{"num_minutes", str::number(int64_t(minutes))}})
                    : std::string(),
        [this, minutes, done = std::move(done)](const json::Document &, const std::string &err) {
            if (err == "cancelled")
                return;
            if (err.empty() && _store.me != model::kNoUser) {
                _store.user(_store.me).dnd = minutes > 0;
                _store.usersChanged();
            }
            if (!err.empty())
                LOG_WARN("slack", "dnd: %s", err.c_str());
            if (done)
                done(err.empty(), err);
        });
}

// ── Bot buttons (msga's pressBotButton; slack-bot-button-press.md) ─────────
// blocks.actions — internal, session tokens only — as the web client sends
// it: the app's bot id as service_id, the button, the message as container.
// Never retried (a press is not idempotent).
void SlackBackend::pressButton(ConvRef conv, Ts ts, const std::string &buttonId, Done done) {
    const model::Message *m  = _store.findMessage(conv, ts);
    const model::Button  *bt = nullptr;
    if (m && m->extra)
        for (const model::Button &x : m->extra->buttons)
            if (!buttonId.empty() && x.id == buttonId && !bt)
                bt = &x;
    if (!bt || !_creds.sessionAuth() || m->extra->botId.empty()) {
        _write->post([done] {
            if (done)
                done(false, kUnpressableButton);
        });
        return;
    }
    const int64_t ms = base::nowMicros() / 1000;
    json::Writer  a;
    a.beginArray().beginObject();
    a.key("action_id").value(bt->id).key("block_id").value(bt->blockId).key("type").value("button");
    a.key("text").beginObject().key("type").value("plain_text").key("text").value(bt->label);
    a.endObject();
    char actionTs[32];
    std::snprintf(
        actionTs, sizeof actionTs, "%lld.%03lld000", (long long)(ms / 1000), (long long)(ms % 1000)
    );
    a.key("action_ts").value(actionTs);
    if (!bt->value.empty())
        a.key("value").value(bt->value);
    if (bt->style != model::Button::Style::Default)
        a.key("style").value(bt->style == model::Button::Style::Primary ? "primary" : "danger");
    a.endObject().endArray();
    json::Writer c;
    c.beginObject().key("type").value("message").key("message_ts").value(model::formatTs(ts));
    c.key("channel_id").value(convId(conv)).key("is_ephemeral").value(false);
    if (m->isReply())
        c.key("thread_ts").value(model::formatTs(m->threadTs));
    c.endObject();
    std::string form;
    addParam(form, "service_id", m->extra->botId);
    addParam(form, "client_token", "msga-" + str::number(ms));
    addParam(form, "actions", a.str());
    addParam(form, "container", c.str());
    api("blocks.actions",
        std::move(form),
        [done = std::move(done)](const json::Document &, const std::string &err) {
            if (err == "cancelled")
                return;
            if (!err.empty())
                LOG_WARN("slack", "blocks.actions: %s", err.c_str());
            if (done)
                done(err.empty(), err);
        });
}

// ── The account's Slack theme (msga's loadSidebarTheme) ─────────────────────

void SlackBackend::loadSidebarTheme(std::function<void(SidebarTheme, std::string)> done) {
    if (!_creds.sessionAuth()) {
        _write->post([done] {
            if (done)
                done({}, "not_supported");
        });
        return;
    }
    api("users.prefs.get",
        {},
        [done = std::move(done)](const json::Document &doc, const std::string &err) {
            if (err == "cancelled")
                return;
            if (!err.empty()) {
                LOG_WARN("slack", "users.prefs.get: %s", err.c_str());
                if (done)
                    done({}, err);
                return;
            }
            const json::Value p = doc.root()["prefs"];
            SidebarTheme      t;
            t.iaTheme = std::string(p["ia_theme"].str());
            // The legacy slots, a JSON object (often sent as a string).
            json::Document inner;
            json::Value    legacy = p["sidebar_theme_custom_values"];
            if (legacy.isString() && inner.parse(std::string(legacy.str()), nullptr))
                legacy = inner.root();
            static const char *const kSlots[] = {
                "column_bg",
                "menu_bg",
                "active_item",
                "active_item_text",
                "hover_item",
                "text_color",
                "active_presence",
                "badge",
            };
            std::string values;
            bool        complete = legacy.isObject();
            for (const char *k : kSlots) {
                const std::string_view v = legacy[k].str();
                complete                 = complete && !v.empty();
                values += (values.empty() ? "" : ",") + std::string(v);
            }
            if (complete && legacy.has("top_nav_bg") && legacy.has("top_nav_text"))
                values += str::concat(
                    {",", legacy["top_nav_bg"].str(), ",", legacy["top_nav_text"].str()}
                );
            if (complete)
                t.legacyValues = std::move(values);
            if (done)
                done(std::move(t), {});
        });
}

} // namespace slack
