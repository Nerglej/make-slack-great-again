#include "app/model/store.h"

#include "app/mrkdwn/emoji.h"
#include "base/str.h"
#include "base/time.h"

#include <algorithm>
#include <charconv>
#include <cstdio>

namespace model {

// ── Ts ──────────────────────────────────────────────────────────────────────

Ts parseTs(std::string_view s) {
    // "seconds.micros"; the fraction may be shorter than 6 digits.
    const size_t dot  = s.find('.');
    int64_t      secs = 0;
    const auto   head = s.substr(0, dot);
    if (head.empty() || std::from_chars(head.data(), head.data() + head.size(), secs).ptr !=
                            head.data() + head.size())
        return 0;
    int64_t micros = 0;
    if (dot != std::string_view::npos) {
        const auto frac = s.substr(dot + 1);
        if (frac.size() > 6)
            return 0;
        for (char c : frac) {
            if (c < '0' || c > '9')
                return 0;
            micros = micros * 10 + (c - '0');
        }
        for (size_t k = frac.size(); k < 6; ++k)
            micros *= 10;
    }
    return secs * 1000000 + micros;
}

std::string formatTs(Ts ts) {
    char buf[32];
    std::snprintf(
        buf, sizeof buf, "%lld.%06lld", (long long)(ts / 1000000), (long long)(ts % 1000000)
    );
    return buf;
}

// ── Records ─────────────────────────────────────────────────────────────────

std::string_view User::label() const {
    if (!displayName.empty())
        return displayName;
    return name.empty() ? std::string_view(id) : std::string_view(name);
}

MessageExtras::MessageExtras()                      = default;
MessageExtras::MessageExtras(const MessageExtras &) = default;
MessageExtras::~MessageExtras()                     = default;
Message::Message()                                  = default;
Message::Message(Message &&) noexcept               = default;
Message &Message::operator=(Message &&) noexcept    = default;
Message::~Message()                                 = default;

bool File::isHtml() const {
    // msga's fileIsHtml: the type, else the extension.
    auto ieq = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
            if ((a[i] | 0x20) != (b[i] | 0x20))
                return false;
        return true;
    };
    if (ieq(mime, "text/html"))
        return true;
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos)
        return false;
    const std::string_view ext = std::string_view(name).substr(dot + 1);
    return ieq(ext, "html") || ieq(ext, "htm");
}

MessageExtras &Message::extras() {
    if (!extra)
        extra = std::make_unique<MessageExtras>();
    return *extra;
}

namespace {
const MessageExtras       kNoExtras;
const User                kNoUserRecord;
const std::vector<Typing> kNoTyping;
} // namespace

const std::string &Message::subtype() const {
    return (extra ? *extra : kNoExtras).subtype;
}
const std::vector<File> &Message::files() const {
    return (extra ? *extra : kNoExtras).files;
}
const std::vector<Attachment> &Message::attachments() const {
    return (extra ? *extra : kNoExtras).attachments;
}
bool Message::isHuddle() const {
    return extra && extra->subtype == "huddle_thread";
}

Message Message::clone() const {
    Message m;
    m.ts          = ts;
    m.threadTs    = threadTs;
    m.latestReply = latestReply;
    m.user        = user;
    m.replyCount  = replyCount;
    m.text        = text;
    m.edited      = edited;
    m.pinned      = pinned;
    m.pending     = pending;
    m.replyUsers  = replyUsers;
    m.reactions   = reactions;
    if (extra)
        m.extra = std::make_unique<MessageExtras>(*extra);
    return m;
}

// ── Store ───────────────────────────────────────────────────────────────────

Store::Store()  = default;
Store::~Store() = default;

void Store::clear() {
    _users.clear();
    _userIndex.clear();
    _convs.clear();
    _convIndex.clear();
    _typing.clear();
    _customEmoji.clear();
    myGroups.clear();
    workspaceId.clear();
    workspaceName.clear();
    workspaceIcon.clear();
    workspaceUrl.clear();
    _mutedThreads.clear();
    _aiTranscripts.clear();
    _reminders.clear();
    _saved.clear();
    _usergroups.clear();
    _channelNames.clear();
    _linkedAuthors.clear();
    _unreadThreads     = 0;
    workspaceMuted     = false;
    answersAreMentions = false;
    me                 = kNoUser;
    emit({ChangeKind::Roster});
    emit({ChangeKind::Users});
}

UserRef Store::addUser(User u) {
    if (const auto it = _userIndex.find(u.id); it != _userIndex.end()) {
        _users[it->second]             = std::move(u); // same ref: messages keep pointing at it
        _users[it->second].placeholder = false;
        return it->second;
    }
    const UserRef ref = UserRef(_users.size());
    _userIndex.emplace(u.id, ref);
    _users.push_back(std::move(u));
    return ref;
}

void Store::usersChanged() {
    emit({ChangeKind::Users});
}

UserRef Store::internUser(std::string_view id) {
    if (id.empty())
        return kNoUser;
    if (const UserRef r = findUser(id); r != kNoUser)
        return r;
    User u;
    u.id          = std::string(id);
    u.placeholder = true; // kept: only the merge path of addUser clears it
    return addUser(std::move(u));
}

UserRef Store::findUser(std::string_view id) const {
    // unordered_map<std::string> has no heterogeneous lookup before C++20's
    // is_transparent support for unordered containers; one short copy is fine.
    const auto it = _userIndex.find(std::string(id));
    return it == _userIndex.end() ? kNoUser : it->second;
}

const User &Store::user(UserRef u) const {
    return u < _users.size() ? _users[u] : kNoUserRecord;
}

User &Store::user(UserRef u) {
    static User scratch; // writes to kNoUser land here and are harmless
    return u < _users.size() ? _users[u] : scratch;
}

ConvRef Store::addConversation(Conversation c) {
    if (const auto it = _convIndex.find(c.id); it != _convIndex.end()) {
        Conversation &old = _convs[it->second];
        // Keep the loaded messages: a roster refresh only brings metadata.
        if (c.messages.empty() && c.threads.empty()) {
            c.messages      = std::move(old.messages);
            c.threads       = std::move(old.threads);
            c.hasMoreBefore = old.hasMoreBefore;
        }
        old = std::move(c);
        emit({ChangeKind::Meta, it->second});
        return it->second;
    }
    const ConvRef ref = ConvRef(_convs.size());
    _convIndex.emplace(c.id, ref);
    _convs.push_back(std::move(c));
    _typing.emplace_back();
    emit({ChangeKind::Roster});
    return ref;
}

ConvRef Store::findConversation(std::string_view id) const {
    const auto it = _convIndex.find(std::string(id));
    return it == _convIndex.end() ? kNoConv : it->second;
}

// A ref this Store doesn't have (kNoConv, or one a view kept from the Store
// the slot just left) reads as an empty conversation, not past the end: the
// exit crash was a message list rebuilding on the blank Store.
const Conversation &Store::conversation(ConvRef c) const {
    static const Conversation none;
    return c < _convs.size() ? _convs[c] : none;
}

Conversation &Store::conversation(ConvRef c) {
    static Conversation scratch; // writes to a missing ref land here and are harmless
    if (c < _convs.size())
        return _convs[c];
    scratch = {};
    return scratch;
}

std::string Store::displayName(ConvRef ref) const {
    if (ref >= _convs.size())
        return {};
    const Conversation &c = _convs[ref];
    switch (c.kind) {
    case ConvKind::Dm:
        if (!c.localName.empty()) // a renamed agent session
            return c.localName;
        if (c.dmUser != kNoUser)
            return std::string(user(c.dmUser).label());
        return c.name;
    case ConvKind::Group: {
        if (!c.localName.empty())
            return c.localName;
        // "Mira Okafor, Jonas Weber": the participants other than me.
        std::string out;
        for (UserRef u : c.members) {
            if (u == me)
                continue;
            if (!out.empty())
                out += ", ";
            out += user(u).label();
        }
        return out.empty() ? c.name : out;
    }
    default:
        return c.name;
    }
}

void Store::updateConversation(ConvRef c, const std::function<void(Conversation &)> &fn) {
    if (c >= _convs.size())
        return;
    fn(_convs[c]);
    emit({ChangeKind::Meta, c});
}

Thread *Store::findThread(Conversation &c, Ts root) {
    for (auto &t : c.threads)
        if (t.root == root)
            return &t;
    return nullptr;
}

const Thread *Store::findThread(const Conversation &c, Ts root) const {
    for (const auto &t : c.threads)
        if (t.root == root)
            return &t;
    return nullptr;
}

std::vector<Message> *Store::listFor(Conversation &c, Ts thread, bool create) {
    if (!thread)
        return &c.messages;
    if (Thread *t = findThread(c, thread))
        return &t->replies;
    if (!create)
        return nullptr;
    c.threads.push_back(Thread{thread, {}});
    return &c.threads.back().replies;
}

// Pages come oldest-first (fixture, conversations.replies) or newest-first
// (conversations.history): reverse the latter, insertion-sort anything else.
// Pages are ≤ 200 messages, and this avoids std::sort's ~1.5 KB introsort
// instantiation for a move-only type.
static void sortByTs(std::vector<Message> &v) {
    if (v.size() > 1 && v.front().ts > v.back().ts)
        std::reverse(v.begin(), v.end());
    for (size_t i = 1; i < v.size(); ++i)
        for (size_t j = i; j > 0 && v[j].ts < v[j - 1].ts; --j)
            std::swap(v[j], v[j - 1]);
}

static std::vector<Message>::iterator lowerByTs(std::vector<Message> &v, Ts ts) {
    return std::lower_bound(v.begin(), v.end(), ts, [](const Message &m, Ts t) {
        return m.ts < t;
    });
}

Message *Store::findMessage(ConvRef ref, Ts ts) {
    if (ref >= _convs.size())
        return nullptr;
    Conversation &c = _convs[ref];
    if (auto it = lowerByTs(c.messages, ts); it != c.messages.end() && it->ts == ts)
        return &*it;
    for (auto &t : c.threads)
        if (auto it = lowerByTs(t.replies, ts); it != t.replies.end() && it->ts == ts)
            return &*it;
    return nullptr;
}

const Message *Store::findMessage(ConvRef ref, Ts ts) const {
    return const_cast<Store *>(this)->findMessage(ref, ts);
}

const std::vector<Message> *Store::replies(ConvRef ref, Ts root) const {
    if (ref >= _convs.size())
        return nullptr;
    const Thread *t = findThread(_convs[ref], root);
    return t ? &t->replies : nullptr;
}

size_t Store::indexOf(ConvRef ref, Ts ts, Ts thread) const {
    if (ref >= _convs.size())
        return SIZE_MAX;
    Store *self = const_cast<Store *>(this); // listFor(create = false) never mutates
    auto  *list = self->listFor(self->_convs[ref], thread, false);
    if (!list)
        return SIZE_MAX;
    const auto it = lowerByTs(*list, ts);
    return (it != list->end() && it->ts == ts) ? size_t(it - list->begin()) : SIZE_MAX;
}

void Store::addPage(ConvRef ref, std::vector<Message> page) {
    if (ref >= _convs.size() || page.empty())
        return;
    Conversation &c      = _convs[ref];
    const Ts      thread = page.front().isReply() ? page.front().threadTs : 0;
    auto         &list   = *listFor(c, thread, true);
    sortByTs(page);

    std::vector<Ts>      updated, inserted;
    std::vector<Message> before, after;
    const bool           empty = list.empty();
    const Ts             front = empty ? 0 : list.front().ts;
    const Ts             back  = empty ? 0 : list.back().ts;
    for (auto &m : page) {
        if (!empty) {
            if (auto it = lowerByTs(list, m.ts); it != list.end() && it->ts == m.ts) {
                *it = std::move(m);
                updated.push_back(it->ts);
                continue;
            }
        }
        if (empty || m.ts > back)
            after.push_back(std::move(m));
        else if (m.ts < front)
            before.push_back(std::move(m));
        else {
            inserted.push_back(m.ts);
            list.insert(lowerByTs(list, m.ts), std::move(m));
        }
    }
    // Dedupe within the page itself (a sorted page may repeat a ts).
    auto dedupe = [](std::vector<Message> &v) {
        v.erase(
            std::unique(
                v.begin(), v.end(), [](const Message &a, const Message &b) { return a.ts == b.ts; }
            ),
            v.end()
        );
    };
    dedupe(before);
    dedupe(after);
    const uint32_t nBefore = uint32_t(before.size()), nAfter = uint32_t(after.size());
    if (nBefore)
        list.insert(
            list.begin(),
            std::make_move_iterator(before.begin()),
            std::make_move_iterator(before.end())
        );
    if (nAfter)
        list.insert(
            list.end(), std::make_move_iterator(after.begin()), std::make_move_iterator(after.end())
        );
    if (!thread && !list.empty())
        c.latest = std::max(c.latest, list.back().ts);

    for (Ts ts : updated)
        emit({ChangeKind::Update, ref, thread, ts});
    if (nBefore)
        emit({ChangeKind::Prepend, ref, thread, 0, nBefore});
    for (Ts ts : inserted)
        emit({ChangeKind::Insert, ref, thread, ts});
    if (nAfter)
        emit({ChangeKind::Append, ref, thread, 0, nAfter});
}

void Store::addMessage(ConvRef ref, Message m) {
    if (ref >= _convs.size())
        return;
    Conversation &c      = _convs[ref];
    const Ts      thread = m.isReply() ? m.threadTs : 0;
    const Ts      ts     = m.ts;
    const UserRef author = m.user;

    // The author's typing indicator ends with their message, as in Slack —
    // except an agent's mid-task update (subtype "progress"): it is still
    // working.
    auto        &typers       = _typing[ref];
    const size_t typersBefore = typers.size();
    if (m.subtype() != "progress")
        std::erase_if(typers, [&](const Typing &t) {
            return t.user == author && t.thread == thread;
        });
    const bool typingChanged = typers.size() != typersBefore;

    if (Message *existing = findMessage(ref, ts)) {
        // A push for something we have (our own send echoed back): replace.
        *existing = std::move(m);
        emit({ChangeKind::Update, ref, thread, ts});
        if (typingChanged)
            emit({ChangeKind::Typing, ref, thread});
        return;
    }

    if (thread) {
        Message   *root  = findMessage(ref, thread);
        auto      &list  = *listFor(c, thread, true);
        const bool atEnd = list.empty() || ts > list.back().ts;
        list.insert(lowerByTs(list, ts), std::move(m));
        if (root) {
            ++root->replyCount;
            root->latestReply = std::max(root->latestReply, ts);
            if (author != kNoUser && root->replyUsers.size() < 5 &&
                std::find(root->replyUsers.begin(), root->replyUsers.end(), author) ==
                    root->replyUsers.end())
                root->replyUsers.push_back(author);
        }
        emit(
            {atEnd ? ChangeKind::Append : ChangeKind::Insert,
             ref,
             thread,
             atEnd ? 0 : ts,
             atEnd ? 1u : 0u}
        );
        if (root)
            emit({ChangeKind::Update, ref, 0, thread});
    } else {
        const bool atEnd   = c.messages.empty() || ts > c.messages.back().ts;
        const bool unread  = author != me && ts > c.lastRead;
        const bool mention = unread && mentions(m);
        c.messages.insert(lowerByTs(c.messages, ts), std::move(m));
        c.latest = std::max(c.latest, ts);
        if (c.isDirect())
            c.member = true; // a closed DM comes back with its next message
        if (unread) {
            ++c.unread;
            if (mention)
                ++c.mentions;
        }
        emit(
            {atEnd ? ChangeKind::Append : ChangeKind::Insert,
             ref,
             0,
             atEnd ? 0 : ts,
             atEnd ? 1u : 0u}
        );
        emit({ChangeKind::Meta, ref});
    }
    if (typingChanged)
        emit({ChangeKind::Typing, ref, thread});
}

bool Store::updateMessage(ConvRef ref, Ts ts, const std::function<void(Message &)> &fn) {
    Message *m = findMessage(ref, ts);
    if (!m)
        return false;
    fn(*m);
    emit({ChangeKind::Update, ref, m->isReply() ? m->threadTs : 0, ts});
    return true;
}

bool Store::removeMessage(ConvRef ref, Ts ts) {
    Message *m = findMessage(ref, ts);
    if (!m)
        return false;
    Conversation &c      = _convs[ref];
    const Ts      thread = m->isReply() ? m->threadTs : 0;
    auto         &list   = *listFor(c, thread, false);
    list.erase(lowerByTs(list, ts));
    if (thread) {
        if (Message *root = findMessage(ref, thread)) {
            if (root->replyCount)
                --root->replyCount;
            root->latestReply = list.empty() ? 0 : list.back().ts;
            emit({ChangeKind::Remove, ref, thread, ts});
            emit({ChangeKind::Update, ref, 0, thread});
            return true;
        }
    } else {
        // Its replies go with it, as with Slack's message_deleted.
        std::erase_if(c.threads, [&](const Thread &t) { return t.root == ts; });
        recountUnread(c);
    }
    emit({ChangeKind::Remove, ref, thread, ts});
    if (!thread)
        emit({ChangeKind::Meta, ref});
    return true;
}

bool Store::setReaction(ConvRef ref, Ts ts, std::string_view name, UserRef u, bool on) {
    return updateMessage(ref, ts, [&](Message &m) {
        auto it = std::find_if(m.reactions.begin(), m.reactions.end(), [&](const Reaction &r) {
            return r.name == name;
        });
        if (on) {
            if (it == m.reactions.end()) {
                m.reactions.push_back(Reaction{std::string(name), 1, {u}});
            } else if (std::find(it->users.begin(), it->users.end(), u) == it->users.end()) {
                it->users.push_back(u);
                ++it->count;
            }
        } else if (it != m.reactions.end()) {
            const auto before = it->users.size();
            std::erase(it->users, u);
            if (it->users.size() != before && it->count)
                --it->count;
            if (it->count == 0)
                m.reactions.erase(it);
        }
    });
}

bool Store::reactedByMe(const Reaction &r) const {
    return me != kNoUser && std::find(r.users.begin(), r.users.end(), me) != r.users.end();
}

bool Store::mentions(const Message &m) const {
    if (answersAreMentions)
        return m.subtype() != "progress";
    return mentionsMe(m.text);
}

void Store::recountUnread(Conversation &c) {
    c.unread   = 0;
    c.mentions = 0;
    auto it    = std::upper_bound(
        c.messages.begin(), c.messages.end(), c.lastRead, [](Ts t, const Message &m) {
            return t < m.ts;
        }
    );
    for (; it != c.messages.end(); ++it) {
        if (it->user == me)
            continue;
        ++c.unread;
        if (mentions(*it))
            ++c.mentions;
    }
}

void Store::markRead(ConvRef ref, Ts ts) {
    if (ref >= _convs.size())
        return;
    Conversation &c = _convs[ref];
    if (ts > c.lastRead)
        c.lastRead = ts;
    recountUnread(c);
    emit({ChangeKind::Meta, ref});
}

void Store::markUnread(ConvRef ref, Ts ts) {
    if (ref >= _convs.size() || ts <= 0)
        return;
    Conversation &c = _convs[ref];
    c.lastRead      = ts - 1;
    recountUnread(c);
    emit({ChangeKind::Meta, ref});
}

namespace {
std::vector<Store::Mark>::iterator findMark(std::vector<Store::Mark> &v, ConvRef c, Ts ts) {
    return std::find_if(v.begin(), v.end(), [&](const Store::Mark &m) {
        return m.conv == c && m.ts == ts;
    });
}
} // namespace

bool Store::threadMuted(ConvRef c, Ts root) const {
    auto &v = const_cast<std::vector<Mark> &>(_mutedThreads);
    return findMark(v, c, root) != v.end();
}

void Store::setThreadMuted(ConvRef c, Ts root, bool on) {
    auto it = findMark(_mutedThreads, c, root);
    if (on && it == _mutedThreads.end())
        _mutedThreads.push_back({c, root, 1});
    else if (!on && it != _mutedThreads.end())
        _mutedThreads.erase(it);
    else
        return;
    emit({ChangeKind::Meta, c}); // app-local state: the workspace cache keeps it
}

const Store::AiTranscript *Store::aiTranscript(const std::string &fileId) const {
    const auto it = _aiTranscripts.find(fileId);
    return it == _aiTranscripts.end() ? nullptr : &it->second;
}

void Store::setAiTranscript(const std::string &fileId, std::string text, std::string by) {
    if (fileId.empty())
        return;
    _aiTranscripts[fileId] = {std::move(text), std::move(by)};
    const auto has         = [&](const Message &m) {
        for (const File &f : m.files())
            if (f.id == fileId)
                return true;
        return false;
    };
    for (ConvRef c = 0; c < ConvRef(_convs.size()); ++c) {
        for (const Message &m : _convs[c].messages)
            if (has(m))
                emit({ChangeKind::Update, c, 0, m.ts});
        for (const Thread &t : _convs[c].threads)
            for (const Message &m : t.replies)
                if (has(m))
                    emit({ChangeKind::Update, c, t.root, m.ts});
    }
}

int64_t Store::reminderAt(ConvRef c, Ts ts) const {
    auto &v  = const_cast<std::vector<Mark> &>(_reminders);
    auto  it = findMark(v, c, ts);
    return it == v.end() ? 0 : it->value;
}

void Store::setReminderAt(ConvRef c, Ts ts, int64_t due) {
    auto it = findMark(_reminders, c, ts);
    if (it != _reminders.end())
        _reminders.erase(it);
    if (due > 0)
        _reminders.push_back({c, ts, due});
    emit({ChangeKind::Update, c, 0, ts});
}

namespace {
// What a Saved messages card shows of the message.
void takePreview(Store::SavedItem &s, const Message &m) {
    s.thread    = m.isReply() ? m.threadTs : 0;
    s.text      = m.text;
    s.author    = m.user;
    s.botName   = m.extra ? m.extra->botName : std::string();
    s.botAvatar = m.extra ? m.extra->botAvatar : std::string();
    s.previewed = true;
}
} // namespace

std::vector<Store::SavedItem> Store::savedItems() const {
    std::vector<SavedItem> out = _saved;
    std::sort(out.begin(), out.end(), [](const SavedItem &a, const SavedItem &b) {
        // msga's messageReminders: reminders (soonest first) ahead of plain
        // bookmarks (newest first).
        if ((a.due > 0) != (b.due > 0))
            return a.due > 0;
        if (a.due > 0 && a.due != b.due)
            return a.due < b.due;
        if (a.savedAt != b.savedAt)
            return a.savedAt > b.savedAt;
        return a.ts > b.ts;
    });
    return out;
}

const Store::SavedItem *Store::findSaved(ConvRef c, Ts ts) const {
    for (const SavedItem &s : _saved)
        if (s.conv == c && s.ts == ts)
            return &s;
    return nullptr;
}

void Store::setSavedItem(ConvRef c, Ts ts, bool on, int64_t due, int64_t savedAt) {
    auto it = std::find_if(_saved.begin(), _saved.end(), [&](const SavedItem &s) {
        return s.conv == c && s.ts == ts;
    });
    if (!on) {
        if (it == _saved.end())
            return;
        _saved.erase(it);
    } else {
        if (it == _saved.end()) {
            SavedItem s;
            s.conv = c;
            s.ts   = ts;
            it     = _saved.insert(_saved.end(), std::move(s));
        }
        if (it->due != due)
            it->fired = false; // rescheduled: it goes off again
        it->due = due;
        if (savedAt > 0)
            it->savedAt = savedAt;
        else if (it->savedAt <= 0)
            it->savedAt = base::nowSecs();
        if (!it->previewed)
            if (const Message *m = findMessage(c, ts))
                takePreview(*it, *m);
    }
    emit({ChangeKind::Update, c, 0, ts});
}

void Store::setSavedPreview(ConvRef c, Ts ts, const Message *m) {
    for (SavedItem &s : _saved)
        if (s.conv == c && s.ts == ts) {
            if (m)
                takePreview(s, *m);
            else
                s.previewed = true; // gone: "No preview available"
            emit({ChangeKind::Update, c, 0, ts});
            return;
        }
}

void Store::setReminderFired(ConvRef c, Ts ts, bool fired) {
    for (SavedItem &s : _saved)
        if (s.conv == c && s.ts == ts && s.fired != fired) {
            s.fired = fired;
            emit({ChangeKind::Update, c, 0, ts});
            return;
        }
}

const Store::Usergroup *Store::findUsergroup(std::string_view id) const {
    for (const Usergroup &g : _usergroups)
        if (g.id == id)
            return &g;
    return nullptr;
}

void Store::setUsergroups(std::vector<Usergroup> groups) {
    std::vector<std::string> mine;
    const std::string       &meId = user(me).id;
    for (const Usergroup &g : groups)
        if (!meId.empty() && std::find(g.users.begin(), g.users.end(), meId) != g.users.end())
            mine.push_back(g.id);
    myGroups    = std::move(mine);
    _usergroups = std::move(groups);
    emit({ChangeKind::Users});
}

const std::string *Store::channelName(std::string_view id) const {
    const auto it = _channelNames.find(std::string(id));
    return it == _channelNames.end() ? nullptr : &it->second;
}

UserRef Store::linkedAuthor(std::string_view conv, std::string_view ts) const {
    const auto it = _linkedAuthors.find(str::concat({conv, "/", ts}));
    return it == _linkedAuthors.end() ? kNoUser : it->second;
}

void Store::setLinkedAuthor(std::string_view conv, std::string_view ts, UserRef u) {
    if (!conv.empty() && !ts.empty() && u != kNoUser)
        _linkedAuthors[str::concat({conv, "/", ts})] = u;
}

void Store::setChannelName(std::string id, std::string name) {
    _channelNames[std::move(id)] = std::move(name);
    emit({ChangeKind::Users});
}

void Store::announceReply(ConvRef c, const Message &m) {
    const Message *was = _arrived;
    _arrived           = &m;
    emit({ChangeKind::Arrived, c, m.threadTs, m.ts});
    _arrived = was;
}

void Store::setUnreadThreads(int n) {
    n = std::max(n, 0);
    if (n == _unreadThreads)
        return;
    _unreadThreads = n;
    emit({ChangeKind::Meta, kNoConv});
}

std::string Store::workspaceLink() const {
    if (!workspaceUrl.empty())
        return workspaceUrl.back() == '/' ? workspaceUrl : workspaceUrl + '/';
    return "https://app.slack.com/client/" + workspaceId + '/';
}

std::string Store::conversationLink(ConvRef c) const {
    const std::string base = workspaceUrl.empty() ? "https://slack.com/" : workspaceLink();
    return base + "archives/" + conversation(c).id;
}

std::string Store::permalink(ConvRef c, Ts ts, Ts thread) const {
    std::string t = formatTs(ts);
    t.erase(std::remove(t.begin(), t.end(), '.'), t.end());
    std::string out = conversationLink(c) + "/p" + t;
    if (thread && thread != ts)
        out += "?thread_ts=" + formatTs(thread) + "&cid=" + conversation(c).id;
    return out;
}

bool Store::mentionsMe(std::string_view text) const {
    // Port of msga's mrkdwnMentions(): a direct mention, a broadcast keyword,
    // or a user group I belong to — the one predicate every badge uses.
    if (me != kNoUser) {
        const std::string tag = "<@" + user(me).id;
        for (size_t i = text.find(tag); i != std::string_view::npos; i = text.find(tag, i + 1)) {
            const size_t after = i + tag.size();
            if (after < text.size() && (text[after] == '>' || text[after] == '|'))
                return true;
        }
    }
    if (!myGroups.empty()) {
        constexpr std::string_view tag = "<!subteam^";
        for (size_t i = text.find(tag); i != std::string_view::npos; i = text.find(tag, i + 1)) {
            const size_t start = i + tag.size();
            size_t       end   = start;
            while (end < text.size() && text[end] != '>' && text[end] != '|')
                ++end;
            const auto id = text.substr(start, end - start);
            if (std::find(myGroups.begin(), myGroups.end(), id) != myGroups.end())
                return true;
        }
    }
    return text.find("<!here") != std::string_view::npos ||
           text.find("<!channel") != std::string_view::npos ||
           text.find("<!everyone") != std::string_view::npos;
}

void Store::setTyping(ConvRef ref, UserRef u, Ts thread, bool on, int64_t sinceMs) {
    if (ref >= _typing.size() || u == me)
        return;
    auto      &list = _typing[ref];
    const auto it   = std::find_if(list.begin(), list.end(), [&](const Typing &t) {
        return t.user == u && t.thread == thread;
    });
    if (on && it != list.end() && it->sinceMs != sinceMs)
        it->sinceMs = sinceMs; // the same typer, a turn that began at another time
    else if (on == (it != list.end()))
        return; // no change: a refreshed indicator fires nothing
    else if (on)
        list.push_back(Typing{u, thread, sinceMs});
    else
        list.erase(it);
    emit({ChangeKind::Typing, ref, thread});
}

const std::vector<Typing> &Store::typing(ConvRef ref) const {
    return ref < _typing.size() ? _typing[ref] : kNoTyping;
}

std::vector<std::pair<std::string, std::string>> Store::customEmojiImages() const {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto &[name, value] : _customEmoji)
        if (value.compare(0, 6, "alias:") != 0)
            out.emplace_back(name, value);
    std::sort(out.begin(), out.end());
    return out;
}

void Store::setCustomEmoji(std::string name, std::string value) {
    _customEmoji[std::move(name)] = std::move(value);
}

Store::EmojiGlyph Store::emojiFor(std::string_view name) const {
    EmojiGlyph g;
    // "+1::skin-tone-3": modifiers only apply to a Unicode base; a custom
    // image base ignores them.
    if (const size_t sep = name.find("::"); sep != std::string_view::npos && sep > 0) {
        g.unicode = emoji::toUnicode(name);
        if (!g.unicode.empty())
            return g;
        return emojiFor(name.substr(0, sep));
    }
    g.unicode = emoji::toUnicode(name);
    if (!g.unicode.empty())
        return g;
    // A raw glyph given as a name (Teams sends reactions as the emoji itself):
    // shortcodes are ASCII, so any non-ASCII byte means it is the glyph.
    for (char c : name)
        if (uint8_t(c) > 0x7F) {
            g.unicode = std::string(name);
            return g;
        }
    std::string cur(name);
    for (int hops = 0; hops < 8; ++hops) { // bounded alias-chain walk
        const auto it = _customEmoji.find(cur);
        if (it == _customEmoji.end())
            break;
        if (it->second.compare(0, 6, "alias:") == 0) {
            cur       = it->second.substr(6);
            g.unicode = emoji::toUnicode(cur);
            if (!g.unicode.empty())
                return g;
            continue;
        }
        g.image = it->second;
        return g;
    }
    return g;
}

Store::ObserverId Store::observe(ConvRef conv, Observer fn) {
    const ObserverId id = _nextObserver++;
    // During dispatch the slot vector must not reallocate under the running
    // callback; new observers join once the outermost emit() is done.
    (_dispatching ? _joining : _observers).push_back(Slot{id, conv, std::move(fn)});
    return id;
}

void Store::unobserve(ObserverId id) {
    for (auto &s : _joining)
        if (s.id == id)
            s.id = 0;
    for (auto &s : _observers) {
        if (s.id == id) {
            if (_dispatching) {
                // Can't erase under a running loop (or destroy the running
                // std::function): mark it dead and compact afterwards.
                s.id          = 0;
                _needsCompact = true;
            } else {
                std::erase_if(_observers, [id](const Slot &x) { return x.id == id; });
            }
            return;
        }
    }
}

void Store::emit(const Change &ch) {
    const bool global = ch.kind == ChangeKind::Roster || ch.kind == ChangeKind::Users;
    ++_dispatching;
    for (auto &s : _observers) {
        if (s.id == 0)
            continue; // unobserved during this dispatch
        if (s.conv != kAnyConv && !global && s.conv != ch.conv)
            continue;
        s.fn(ch);
    }
    if (--_dispatching == 0) {
        if (_needsCompact) {
            std::erase_if(_observers, [](const Slot &x) { return x.id == 0; });
            _needsCompact = false;
        }
        for (auto &s : _joining)
            if (s.id)
                _observers.push_back(std::move(s));
        _joining.clear();
    }
}

} // namespace model
