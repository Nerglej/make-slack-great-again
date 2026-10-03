// The shell's desktop notifications, msga's MainWindow parts: new messages
// (maybeNotify with its gates, notifyWhenUsersResolve), huddles starting
// (maybeNotifyHuddle) and message reminders going off (notifyReminderDue).
// Shell members kept apart from shell.cpp; the clicks are handleAppEvent's.
#include "app/media/sounds.h"
#include "app/screens/common/message_text.h"
#include "base/i18n.h"
#include "base/str.h"
#include "base/utf8.h"
#include "screens/shell/header.h"
#include "screens/shell/shell.h"
#include "screens/shell/shell_dialogs.h"
#include "screens/shell/standin.h"

#include <algorithm>

using i18n::tr;
using model::ConvRef;
using model::kNoConv;
using model::kNoUser;
using model::Ts;

namespace shell {

namespace {

constexpr int64_t kMaxNotifyAgeSecs   = 30 * 86400; // msga's kMaxNotifyAgeDays
constexpr int     kNotifyResolveStep  = 150;        // ms, msga's kNotifyResolveStepMs
constexpr int     kNotifyResolveTries = 10;
constexpr size_t  kMaxBody            = 100;
constexpr int     kNotifyTimeoutMs    = 5000;

// msga's body cap: 97 characters and "…".
std::string capped(std::string s) {
    if (utf8::countCodePoints(s) <= kMaxBody)
        return s;
    size_t at = 0;
    for (size_t n = 0; n < kMaxBody - 3 && at < s.size(); ++n)
        at = utf8::nextBoundary(s, at);
    s.resize(at);
    return s + "\xE2\x80\xA6";
}

// msga's notificationPreview: the text, else what the attachments say.
std::string preview(const model::Store &st, const model::Message &m) {
    std::string text(str::trim(screens::plainText(st, m.text)));
    if (!text.empty())
        return text;
    std::vector<std::string> parts;
    auto                     add = [&](std::string_view mrkdwn) {
        std::string t(str::trim(screens::plainText(st, mrkdwn)));
        if (!t.empty())
            parts.push_back(std::move(t));
    };
    for (const model::Attachment &a : m.attachments()) {
        add(a.pretext);
        add(a.title);
        add(a.text);
        for (const model::AttachmentField &f : a.fields)
            add(str::concat({f.title, ": ", f.value}));
        for (const model::Block &b : a.blocks)
            add(b.text);
    }
    std::string out;
    for (const std::string &p : parts)
        out += (out.empty() ? "" : " \xC2\xB7 ") + p;
    return out;
}

// A name for a notification: the display name, else the handle; "Someone"
// for a user not resolved (yet).
std::string personName(const model::Store &st, model::UserRef u) {
    const model::User &x = st.user(u);
    if (u == kNoUser || x.placeholder || x.label().empty())
        return tr("Someone");
    return std::string(x.label());
}

// A user id's prefix only (U…, W… on Enterprise Grid), not its full shape.
bool hasUserIdPrefix(std::string_view id) {
    return id.size() > 1 && (id[0] == 'U' || id[0] == 'W');
}

} // namespace

// msga: the workspace's name in front ("Team · …") when it isn't the one on
// screen.
std::string Shell::teamTitle(const model::Store &st, const std::string &key, std::string title) {
    if (key.empty() || (key == _activeKey && _signedIn))
        return title;
    std::string team = st.workspaceName;
    for (const Workspace &w : _workspaces)
        if (w.key == key && !w.name.empty())
            team = w.name;
    return team.empty() ? title : str::concat({team, " \xC2\xB7 ", title});
}

// A picture from the avatar cache (none this time while it downloads; the
// next notification has it), masked into the rounded square on every
// platform. One on disk but not decoded yet is waited for (the worker
// decodes it; never this thread).
void Shell::notificationImage(std::vector<std::string> paths, std::function<void(plat::Image)> fn) {
    std::erase_if(paths, [](const std::string &p) { return p.empty(); });
    if (paths.empty()) {
        fn({});
        return;
    }
    const std::string first = std::move(paths.front());
    paths.erase(paths.begin());
    // Avatars never calls back once gone, and it goes with this Shell.
    _avatars.whenReady(
        first, 64, [this, rest = std::move(paths), fn = std::move(fn)](Avatars::Picture b) mutable {
            if (b && !b->empty())
                fn(roundedNotificationImage(*b));
            else
                notificationImage(std::move(rest), std::move(fn));
        }
    );
}

std::string Shell::workspaceIconFor(const model::Store &st) const {
    const std::string custom = customWorkspaceIconPath(_ctx.app.platform(), st.workspaceId);
    return custom.empty() ? st.workspaceIcon : custom;
}

// The OS notification, where there is a notifier (Windows falls back to a
// tray balloon by itself); 0 = none shown. Callers play the chime either way,
// as the old app did when it fell back to the tray.
uint64_t Shell::post(const plat::Notification &n) {
    plat::App &pa = _ctx.app.platform();
    return pa.notificationsAvailable() ? pa.notify(n) : 0;
}

model::NotifyLevel Shell::defaultLevel() const {
    return _settings.notifyLevel == 1 ? model::NotifyLevel::Mentions : model::NotifyLevel::All;
}

// msga's effectiveNotifLevel: muted is Nothing, Default follows Settings.
model::NotifyLevel Shell::effectiveLevel(const model::Conversation &c) const {
    return c.effectiveNotify(defaultLevel());
}

// One workspace's new messages (an Append to a list, or a reply to a thread
// not loaded: Arrived).
void Shell::messagesArrived(model::Store &st, const std::string &key, const model::Change &ch) {
    if (ch.conv >= st.conversationCount())
        return;
    if (ch.kind == model::ChangeKind::Arrived) {
        if (const model::Message *m = st.arrived())
            maybeNotify(st, key, ch.conv, *m, true);
        return;
    }
    const std::vector<model::Message> *list =
        ch.thread ? st.replies(ch.conv, ch.thread) : &st.conversation(ch.conv).messages;
    if (!list)
        return;
    for (size_t i = list->size() - std::min<size_t>(ch.count, list->size()); i < list->size(); ++i)
        maybeNotify(st, key, ch.conv, (*list)[i], true);
}

// msga's maybeNotify: its gates in its order, then the notification.
void Shell::maybeNotify(
    model::Store &st, const std::string &key, ConvRef conv, const model::Message &m, bool allowDefer
) {
    model::Backend &be = backendFor(st);
    if (conv >= st.conversationCount())
        return;
    if (model::tsSecs(m.ts) < be.nowSecs() - kMaxNotifyAgeSecs)
        return; // too old: a backfill, not news
    if (!_settings.notifications || st.workspaceMuted)
        return;
    if ((st.me != kNoUser && m.user == st.me) || m.pending)
        return;
    // Huddles notify on their own; an agent's mid-task updates (tool calls,
    // interim remarks) are shown but never announced.
    if (m.isHuddle() || m.subtype() == "progress")
        return;
    // On screen: this workspace, this conversation, the window focused.
    if (&st == &_ctx.store() && _signedIn && reading() && conv == _current)
        return;
    const model::Conversation &c = st.conversation(conv);
    if (!c.member || effectiveLevel(c) == model::NotifyLevel::Nothing)
        return;
    const Ts   root    = m.isReply() ? m.threadTs : 0;
    const bool mention = st.mentionsMe(m.text);
    if (root && st.threadMuted(conv, root) && !mention)
        return;
    const bool followed  = root && be.threadFollowed(conv, root);
    const bool important = c.isDirect() || mention || followed;
    if (effectiveLevel(c) != model::NotifyLevel::All && !important)
        return;

    // msga's notifyWhenUsersResolve: the author and raw mentions resolved
    // first (at most ~1.5 s), so the text names people.
    if (allowDefer) {
        std::vector<model::UserRef> pending;
        auto                        want = [&](model::UserRef u) {
            if (u != kNoUser && st.user(u).placeholder && hasUserIdPrefix(st.user(u).id) &&
                std::find(pending.begin(), pending.end(), u) == pending.end())
                pending.push_back(u);
        };
        want(m.user);
        for (size_t i = m.text.find("<@"); i != std::string::npos; i = m.text.find("<@", i + 2)) {
            const size_t end = m.text.find_first_of(">|", i + 2);
            if (end != std::string::npos && m.text[end] == '>') // a labelled one reads fine
                if (const model::UserRef u = st.findUser(m.text.substr(i + 2, end - i - 2));
                    u != kNoUser)
                    want(u);
        }
        if (!pending.empty()) {
            for (model::UserRef u : pending)
                be.resolveUser(u);
            auto copy = std::make_shared<model::Message>(m.clone());
            notifyWhenUsersResolve(&st, key, conv, std::move(copy), std::move(pending), 0);
            return;
        }
    }

    const std::string  sender = personName(st, m.user);
    plat::Notification n;
    std::string        body = preview(st, m);
    if (c.isDirect()) {
        n.title = sender;
        // Claude Code: every answer comes from the agent; the session names it.
        if (be.capabilities().agentSessions)
            n.title = st.displayName(conv);
    } else {
        n.title = "#" + c.name;
        body    = str::concat({sender, ": ", body});
    }
    n.title = teamTitle(st, key, std::move(n.title));
    n.body  = capped(std::move(body));
    // DMs: the sender's picture (a bot post's own); channels: the workspace's.
    std::string pic;
    if (c.isDirect()) {
        const std::string &av = st.user(m.user).avatar;
        pic                   = av.empty() && m.extra ? m.extra->botAvatar : av;
    } else {
        pic = workspaceIconFor(st);
    }
    n.timeoutMs = kNotifyTimeoutMs;
    // The chosen sound is ours to play (the old app's playNotificationSound);
    // the OS's own goes as the old app had it (sounds.h).
    n.silent    = sounds::kSilentNotifications;
    notificationImage(
        {std::move(pic)},
        [this, n = std::move(n), key = std::string(key), conv, root](plat::Image img) mutable {
            n.image = std::move(img);
            if (const uint64_t id = post(n))
                _notified[id] = {key, conv, root, 0, {}};
            if (_settings.notifySound)
                sounds::play(_ctx.app.platform(), _settings.soundId);
        }
    );
}

void Shell::notifyWhenUsersResolve(
    model::Store                   *st,
    const std::string              &key,
    ConvRef                         conv,
    std::shared_ptr<model::Message> m,
    std::vector<model::UserRef>     pending,
    int                             tries
) {
    auto id = std::make_shared<plat::TimerId>(0);
    *id     = _ctx.app.addTimer(
        kNotifyResolveStep,
        false,
        [this, id, st, key, conv, m = std::move(m), pending = std::move(pending), tries]() mutable {
            std::erase(_resolveTimers, *id);
            // The workspace went away meanwhile: nothing to say.
            if (!storeAlive(st) || conv >= st->conversationCount())
                return;
            std::erase_if(pending, [st](model::UserRef u) {
                return u >= st->userCount() || !st->user(u).placeholder;
            });
            if (pending.empty() || tries + 1 >= kNotifyResolveTries)
                maybeNotify(*st, key, conv, *m, false);
            else
                notifyWhenUsersResolve(st, key, conv, std::move(m), std::move(pending), tries + 1);
        }
    );
    _resolveTimers.push_back(*id);
}

// msga's maybeNotifyHuddle: once per huddle start (the conversation's Meta
// with huddleActive), never for one I'm in, a channel's only on "All new
// posts"; the banner covers the open conversation.
void Shell::huddleChanged(model::Store &st, const std::string &key, ConvRef conv) {
    if (conv >= st.conversationCount())
        return;
    const model::Conversation &c   = st.conversation(conv);
    const std::string          tag = str::concat({key, "\x1F", c.id});
    if (!c.huddleActive) {
        _notifiedHuddles.erase(tag);
        return;
    }
    if (_notifiedHuddles.count(tag))
        return;
    model::Backend &be = backendFor(st);
    if (!_settings.notifications || !_settings.notifyHuddles || st.workspaceMuted ||
        !be.capabilities().huddles)
        return;
    const model::NotifyLevel level = effectiveLevel(c);
    if (!c.member || c.muted || level == model::NotifyLevel::Nothing)
        return;
    const auto &ps = c.huddleParticipants;
    if (st.me != kNoUser && std::find(ps.begin(), ps.end(), st.me) != ps.end())
        return;
    if (!c.isDirect() && level != model::NotifyLevel::All)
        return;
    _notifiedHuddles.insert(tag);
    if (&st == &_ctx.store() && _signedIn && reading() && conv == _current)
        return; // the banner says it
    const model::UserRef starter = ps.empty() ? kNoUser : ps.front();
    const std::string    name    = personName(st, starter);
    plat::Notification   n;
    std::string          pic;
    if (c.isDirect()) {
        n.title = name;
        n.body  = tr("Started a huddle");
        pic     = st.user(starter).avatar;
    } else {
        n.title = "#" + c.name;
        n.body  = i18n::arg(tr("%1 started a huddle"), name);
        pic     = workspaceIconFor(st);
    }
    n.title          = teamTitle(st, key, std::move(n.title));
    n.actions        = {{"join", tr("Join")}};
    n.timeoutMs      = kNotifyTimeoutMs;
    n.silent         = sounds::kSilentNotifications;
    std::string join = c.huddleLink.empty() ? huddleJoinUrl(st, conv) : c.huddleLink;
    notificationImage(
        {std::move(pic)},
        [this, n = std::move(n), key = std::string(key), conv, join = std::move(join)](
            plat::Image img
        ) mutable {
            n.image = std::move(img);
            if (const uint64_t id = post(n))
                _notified[id] = {key, conv, 0, 0, std::move(join)};
            if (_settings.notifySound)
                sounds::play(_ctx.app.platform(), _settings.soundId);
        }
    );
}

// msga's notifyReminderDue: only the master switch gates it; the click opens
// the message itself (its thread for a reply) and flashes it.
void Shell::notifyReminderDue(const std::string &key, model::Store &st, ConvRef conv, Ts ts) {
    if (!_settings.notifications || conv >= st.conversationCount())
        return;
    const model::Store::SavedItem *it = st.findSaved(conv, ts);
    if (!it)
        return;
    const model::Conversation &c = st.conversation(conv);
    std::string                where;
    if (c.kind == model::ConvKind::Dm) {
        where = personName(st, c.dmUser);
    } else if (c.kind == model::ConvKind::Group) {
        // Its own name, else up to three of the others.
        where = c.localName;
        for (size_t i = 0, n = 0; c.localName.empty() && i < c.members.size() && n < 3; ++i)
            if (c.members[i] != st.me) {
                where += (n++ ? ", " : "") + personName(st, c.members[i]);
                backendFor(st).resolveUser(c.members[i]);
            }
    } else {
        where = "#" + c.name;
    }
    plat::Notification n;
    n.title            = where.empty() ? std::string(tr("Reminder"))
                                       : i18n::arg(tr("Reminder \xE2\x80\x94 %1"), where);
    n.title            = teamTitle(st, key, std::move(n.title));
    std::string author = it->author != kNoUser && !st.user(it->author).placeholder
                             ? std::string(st.user(it->author).label())
                             : it->botName;
    if (it->author != kNoUser && st.user(it->author).placeholder)
        backendFor(st).resolveUser(it->author);
    // msga's snippet: the text simplified, at most 120 characters.
    std::string snippet;
    for (char ch : screens::plainText(st, it->text))
        if (!(ch == ' ' || ch == '\n' || ch == '\t') || (!snippet.empty() && snippet.back() != ' '))
            snippet += ch == '\n' || ch == '\t' ? ' ' : ch;
    snippet = std::string(str::trim(snippet));
    if (utf8::countCodePoints(snippet) > 120) {
        size_t at = 0;
        for (size_t k = 0; k < 120; ++k)
            at = utf8::nextBoundary(snippet, at);
        snippet.resize(at);
    }
    std::string body =
        snippet.empty() ? std::string(tr("You asked to be reminded about a message.")) : snippet;
    if (!author.empty() && !snippet.empty())
        body = str::concat({author, ": ", body});
    n.body = capped(std::move(body));
    // The author's picture, a bot's, the DM peer's, the workspace's: the
    // first that has one.
    std::vector<std::string> pics;
    if (it->author != kNoUser)
        pics.push_back(st.user(it->author).avatar);
    pics.push_back(it->botAvatar);
    if (c.kind == model::ConvKind::Dm)
        pics.push_back(st.user(c.dmUser).avatar);
    pics.push_back(workspaceIconFor(st));
    n.timeoutMs = kNotifyTimeoutMs;
    n.silent    = sounds::kSilentNotifications;
    notificationImage(
        std::move(pics),
        [this, n = std::move(n), key = std::string(key), conv, thread = it->thread, ts](
            plat::Image img
        ) mutable {
            n.image = std::move(img);
            if (const uint64_t id = post(n))
                _notified[id] = {key, conv, thread, ts, {}};
            if (_settings.notifySound)
                sounds::play(_ctx.app.platform(), _settings.soundId);
        }
    );
}

} // namespace shell
