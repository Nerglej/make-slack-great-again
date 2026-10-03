// Backend — what a messaging service implements (the demo fixture now, the
// Slack Web API + Socket Mode later). A backend writes everything it learns
// into the Store it was given; the UI never sees payloads, only Store
// changes. Calls return at once; `done` runs later on the UI thread, never
// re-entrantly from inside the call (a network round trip never completes
// synchronously, and callers rely on that ordering).
#pragma once

#include "app/model/store.h"
#include "base/time.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace model {

class Backend {
public:
    using Done = std::function<void(bool ok, const std::string &error)>;

    explicit Backend(Store &store) : _store(store) {}
    virtual ~Backend()                  = default;
    Backend(const Backend &)            = delete;
    Backend &operator=(const Backend &) = delete;

    // Signs in and loads the workspace, me, users and conversations.
    virtual void connect(Done done)                              = 0;
    // The page of top-level messages before `before` (0 = the newest page).
    // Messages arrive in the Store (Prepend / Append); the conversation's
    // hasMoreBefore says whether another page exists.
    virtual void loadHistory(ConvRef conv, Ts before, Done done) = 0;
    // A thread's replies (the root stays in the conversation's list).
    virtual void loadThread(ConvRef conv, Ts root, Done done)    = 0;
    // What the user is looking at (kNoConv: nothing; thread 0: the channel).
    // Poll-driven backends refresh it more often (the foreground poll).
    virtual void setActiveConversation(ConvRef conv, Ts thread) { (void)conv, (void)thread; }

    // Posts mrkdwn. A pending copy appears in the Store at once; it turns
    // into the confirmed message (pending = false) when the server answers.
    // threadTs != 0 posts a reply.
    virtual void send(ConvRef conv, std::string text, Ts threadTs, Done done) = 0;
    // The same with local files attached (absolute paths; text may be
    // empty). The pending copy shows the files at once. Default: a backend
    // that cannot upload yet sends the text alone.
    virtual void sendWithFiles(
        ConvRef conv, std::string text, Ts threadTs, std::vector<std::string> files, Done done
    ) {
        send(conv, std::move(text), threadTs, std::move(done));
    }
    virtual void edit(ConvRef conv, Ts ts, std::string text)                 = 0;
    virtual void remove(ConvRef conv, Ts ts)                                 = 0;
    virtual void react(ConvRef conv, Ts ts, std::string_view name, bool add) = 0;
    virtual void markRead(ConvRef conv, Ts ts)                               = 0;
    // "Mark unread": the read cursor moves back to just before ts.
    virtual void markUnread(ConvRef conv, Ts ts)                             = 0;
    virtual void setStarred(ConvRef conv, bool starred)                      = 0;
    // Muting silences a conversation (no notifications, no badge).
    virtual void setMuted(ConvRef conv, bool muted)                          = 0;
    virtual void setNotifyLevel(ConvRef conv, NotifyLevel level)             = 0;
    // Leaves a channel, closes a DM (it comes back with the next message or
    // openDm), removes an agent session from the list. member = false.
    virtual void leave(ConvRef conv)                                         = 0;
    // The DM with `user` (opened or created); done gets kNoConv on failure.
    virtual void openDm(UserRef user, std::function<void(ConvRef)> done)     = 0;
    virtual void setPinned(ConvRef conv, Ts ts, bool pinned)                 = 0;
    virtual void setSaved(ConvRef conv, Ts ts, bool saved)                   = 0;
    // Deletes a file from a message (own or as an admin).
    virtual void deleteFile(ConvRef conv, Ts ts, const std::string &fileId)  = 0;
    // "Remove preview" on an own message (Capabilities::removePreview):
    // strips attachment `attachmentId` (Slack's positional id, 1-based) for
    // everyone; on success the Store's message loses it. Default: fails.
    virtual void deleteAttachment(ConvRef conv, Ts ts, int attachmentId, Done done) {
        (void)conv, (void)ts, (void)attachmentId;
        if (done)
            done(false, "not supported");
    }
    // A file's bytes (a File's source(): an http(s) URL of this service) to
    // `toPath`, with the service's credentials; the write happens off the UI
    // thread. Local paths never come here: the
    // screens copy those themselves (screens/common/downloads.h). Default,
    // for backends without remote files: fails at once.
    virtual void downloadFile(const std::string &url, std::string toPath, Done done) {
        (void)url, (void)toPath;
        if (done)
            done(false, "not supported");
    }
    // "Remind me": a saved item with a due time (epoch secs); 0 removes it.
    virtual void setReminder(ConvRef conv, Ts ts, int64_t dueSecs) = 0;

    // "Find a channel" / "Create a channel": joins a listed channel, or
    // creates one (name already lower-cased and dashed). done(conv, "") once
    // it is a member conversation in the Store, done(kNoConv, why) else.
    using ConvDone = std::function<void(ConvRef, const std::string &error)>;
    virtual void joinChannel(ConvRef conv, ConvDone done) {
        (void)conv;
        if (done)
            done(kNoConv, "not supported");
    }
    virtual void createChannel(std::string name, bool isPrivate, ConvDone done) {
        (void)name, (void)isPrivate;
        if (done)
            done(kNoConv, "not supported");
    }

    // Agent sessions (Claude Code shows each as a DM). Other backends keep
    // the defaults: no DM is a session, nothing to stop.
    virtual bool isAgentSession(ConvRef) const { return false; }
    virtual bool canStopSession(ConvRef) const { return false; }
    virtual void stopSession(ConvRef) {}

    // ── Agent sessions, the rest (Claude Code; defaults: nothing of it) ────
    // The team a session can be started with: the
    // Generalist, the specialists, the teammates the user added.
    struct AgentRole {
        std::string id, name, description;
        std::string avatar; // a local image file
        std::string glyph;  // the avatar's glyph id (the teammate editor's pick)
        uint32_t    color = 0;
        std::string prompt;            // what it adds to Claude Code's prompt
        UserRef     user    = kNoUser; // who its messages come from
        bool        builtIn = false, edited = false;
    };
    virtual std::vector<AgentRole> agentRoles() const { return {}; }
    // Adds (id "") or updates a teammate; its id, "" on failure (*error).
    virtual std::string            saveAgentRole(const AgentRole &role, std::string *error) {
        (void)role;
        if (error)
            *error = "not supported";
        return {};
    }
    virtual void        removeAgentRole(const std::string &id) { (void)id; }
    virtual void        restoreAgentRole(const std::string &id) { (void)id; }
    // Why no session can be started in `dir` ("" = it can).
    virtual std::string agentSessionBlocker(const std::string &dir) const {
        (void)dir;
        return "not supported";
    }
    // A new session in `dir` (it starts with its first message): done(conv,
    // "") or done(kNoConv, why).
    virtual void startAgentSession(
        const std::string                                &dir,
        bool                                              skipPermissionChecks,
        const std::string                                &role,
        std::function<void(ConvRef, const std::string &)> done
    ) {
        (void)dir, (void)skipPermissionChecks, (void)role;
        if (done)
            done(kNoConv, "not supported");
    }
    virtual std::string agentSessionFolder(ConvRef) const { return {}; }
    // Which teammate (role id) a session works as; "" = none / not a session.
    virtual std::string agentSessionRole(ConvRef) const { return {}; }
    // "Find a session": every session the service has, listed or not.
    struct FoundSession {
        std::string id, title, folder, role, avatar, firstPrompt, lastPrompt;
        int64_t     lastActiveMs = 0;
        ConvRef     listed       = kNoConv; // where it is in the list already
    };
    virtual void findAgentSessions(std::function<void(std::vector<FoundSession>)> done) {
        if (done)
            done({});
    }
    // Lists a found session (kNoConv: it's gone).
    virtual ConvRef addFoundSession(const std::string &id) {
        (void)id;
        return kNoConv;
    }
    // Slash commands a conversation offers (the composer's "/" list).
    struct Command {
        std::string name, desc, usage;
        bool        local = false; // runLocalCommand answers it; nothing is sent
        // Where it comes from, the row's label: "msga", "Claude Code",
        // "Project skill", "Skill", a plugin's name…
        std::string source;
        // Its picture (a Slack app's icon); "" = the service's: the Slack
        // mark for Slack's own (source "Slack"), else the workspace's.
        std::string icon;
        bool        app = false; // a Slack app's: an initial chip when it has no icon
    };
    virtual std::vector<Command> commands(ConvRef) { return {}; }
    // A local command: rows for a dialog (/status), a conversation to open
    // (/clear), or why it can't run. thread: the root it was typed in (0:
    // the channel) — what it posts goes there.
    struct LocalResult {
        std::vector<std::pair<std::string, std::string>> status;
        ConvRef                                          open = kNoConv;
        std::string                                      error;
    };
    virtual LocalResult
    runLocalCommand(ConvRef, Ts thread, const std::string &name, const std::string &args) {
        (void)thread, (void)name, (void)args;
        return {};
    }
    // The prompts ↑ steps through (newest first): the conversation's, or a
    // folder's for a session not started yet.
    virtual std::vector<std::string> promptHistory(ConvRef) { return {}; }
    virtual std::vector<std::string> folderPromptHistory(const std::string &dir) {
        (void)dir;
        return {};
    }
    // Threads: whether a reply can go there, and whether it is a branched
    // conversation that can move to the list ("Open as session").
    virtual bool    threadAcceptsReplies(ConvRef, Ts root) const { return (void)root, true; }
    virtual bool    threadOpensAsSession(ConvRef, Ts root) const { return (void)root, false; }
    virtual ConvRef openThreadAsSession(ConvRef, Ts root) { return (void)root, kNoConv; }
    // "Delete message" where deleting depends on more than authorship.
    virtual bool    canDeleteMessage(ConvRef, Ts) const { return true; }
    // A message's button (Message extras' buttons) pressed. A button the
    // service won't let this client press (Slack's legacy attachment
    // buttons, a token without the internal API) fails with
    // kUnpressableButton.
    static constexpr const char *kUnpressableButton = "unpressable_button";
    virtual void pressButton(ConvRef conv, Ts ts, const std::string &buttonId, Done done) {
        (void)conv, (void)ts, (void)buttonId;
        if (done)
            done(false, "not supported");
    }
    // Zen mode: hides the tool-call cards.
    virtual void setZenMode(bool on) { (void)on; }
    // "Rename session…" / "Name conversation…": a name only this client
    // shows ("" = the service's own again).
    virtual void setLocalName(ConvRef conv, std::string name) {
        _store.updateConversation(conv, [&](Conversation &c) { c.localName = std::move(name); });
    }
    // The signed-in user is typing in conv (thread 0 = the channel). Call
    // repeatedly while typing; backends rate-limit the outgoing event.
    virtual void userTyping(ConvRef conv, Ts threadTs) = 0;

    // What the service can do: the UI shows a control only where it works
    // (the fields the shell gates on).
    struct Capabilities {
        bool huddles          = false; // header huddle button, sidebar huddle pills
        bool replyBroadcast   = false; // "Also send to channel" under the thread composer
        bool scheduledSend    = false; // the composer's schedule-send chevron
        bool memberList       = false; // the header's members button opens the list
        bool threadsView      = false; // the sidebar's "Threads" entry
        bool messageReminders = false; // "Saved messages" (while something is saved)
        bool presence         = true;  // presence dots, the footer's presence toggle
        bool selfStatus       = true;  // "Manage status" in the footer's avatar menu
        bool agentSessions    = false; // DMs are Claude Code sessions ("Sessions")
        bool profileContact   = true;  // "Manage profile" has email and phone
        bool zenMode          = false; // the footer's zen toggle (hides tool cards)
        bool slashCommands    = false; // the composer's "/" list (commands())
        bool canvases         = false; // the header's canvas tab (the canvas calls)
        bool fileUpload       = false; // Forward re-uploads a message's files (else: links)
        bool removePreview    = false; // deleteAttachment: a link preview's "Remove preview"
        bool sidebarTheme     = false; // loadSidebarTheme: Settings' "Use my Slack theme"
    };
    virtual Capabilities capabilities() const { return {}; }

    // How I look to others: phantom away = away only
    // because no official client holds a connection.
    struct SelfPresence {
        bool loaded = false, active = false, online = false, manualAway = false;
        bool phantomAway() const { return loaded && !active && !online && !manualAway; }
    };
    virtual SelfPresence selfPresence() const { return {}; }
    // Someone's presence asked for now (the DM just
    // opened), not on the poll's next round; the Store follows.
    virtual void         requestPresence(UserRef u) { (void)u; }
    // away = true forces "away" (the footer's toggle); false = automatic.
    virtual void         setPresence(bool away, Done done) { (void)away, (void)done; }
    // The presence link: a connection the app holds so
    // the service counts it as a running client — Slack shows you active
    // only while one is up. Settings → System → Presence, in its order.
    enum class PresenceMode : uint8_t {
        WhileRunning, // held as long as the app runs
        WhileUsing,   // dropped after 30 minutes without input
        Native,       // none: left to the official apps
    };
    // Where the link stands (the footer's presence tooltip says why).
    enum class PresenceLink : uint8_t {
        Off,         // Native, or a service without one
        Connecting,  // being (re)established
        Active,      // up: the service counts us as connected
        Idle,        // WhileUsing dropped it; any input brings it back
        Unavailable, // the service refused it for these credentials
    };
    virtual void         setPresenceMode(PresenceMode mode) { (void)mode; }
    virtual PresenceLink presenceLink() const { return PresenceLink::Off; }
    // Real input in the app (a click, a key, a scroll; the shell throttles
    // it to one call per 20 s): the WhileUsing idle clock and the service's
    // own activity signal.
    virtual void         noteUserActivity() {}
    // My status: emoji shortcode (no colons, "" = none), text, expiry (epoch
    // secs, 0 = never). Success updates my User in the Store.
    virtual void         setStatus(std::string emoji, std::string text, int64_t expiry, Done done) {
        (void)emoji, (void)text, (void)expiry, (void)done;
    }
    // My profile (the footer's "Manage profile"): what users.profile.get
    // returns, a change of it, and a new photo (a local image path).
    struct MyProfile {
        std::string displayName, realName, email, phone, avatar;
    };
    virtual void loadMyProfile(std::function<void(MyProfile)> done) {
        const User &me = _store.user(_store.me);
        MyProfile   p{me.displayName, std::string(me.label()), me.email, {}, me.avatar};
        if (done)
            done(std::move(p));
    }
    virtual void updateProfile(std::string name, std::string email, std::string phone, Done done) {
        (void)name, (void)email, (void)phone;
        if (done)
            done(false, "not supported");
    }
    virtual void setPhoto(std::string path, Done done) {
        (void)path;
        if (done)
            done(false, "not supported");
    }
    // A thread reply that also appears in the channel ("Also send to channel").
    virtual void sendBroadcast(ConvRef conv, std::string text, Ts threadTs, Done done) {
        send(conv, std::move(text), threadTs, std::move(done));
    }
    // Posts at `postAt` (epoch secs) instead of now; a thread reply when
    // `threadTs` is set.
    virtual void
    scheduleMessage(ConvRef conv, std::string text, Ts threadTs, int64_t postAt, Done done) {
        (void)conv, (void)text, (void)threadTs, (void)postAt, (void)done;
    }
    // The composer's message with Block Kit `blocks` (a JSON array: the
    // rich_text block mrkdwn::compose builds for lists) next to its mrkdwn
    // `text`, which the pending copy shows. A service without blocks sends
    // the text alone (the defaults).
    virtual void sendBlocks(
        ConvRef conv, std::string text, std::string blocks, Ts threadTs, bool broadcast, Done done
    ) {
        (void)blocks;
        if (broadcast)
            sendBroadcast(conv, std::move(text), threadTs, std::move(done));
        else
            send(conv, std::move(text), threadTs, std::move(done));
    }
    virtual void editBlocks(ConvRef conv, Ts ts, std::string text, std::string blocks) {
        (void)blocks;
        edit(conv, ts, std::move(text));
    }
    virtual void scheduleBlocks(
        ConvRef conv, std::string text, std::string blocks, Ts threadTs, int64_t postAt, Done done
    ) {
        (void)blocks;
        scheduleMessage(conv, std::move(text), threadTs, postAt, std::move(done));
    }
    // The messages still waiting to be posted, listed into the Store again
    // (Store::setScheduled): the Scheduled messages page asks when it opens.
    virtual void refreshScheduled() {}
    // Cancels one (Store::ScheduledItem::id); the Store drops it once the
    // service agrees.
    virtual void cancelScheduled(const std::string &id, Done done) {
        (void)id;
        if (done)
            done(false, "not_supported");
    }
    // Everyone in a channel or group DM (the header's member list); `error`
    // is the service's reason when the list couldn't be loaded.
    using MembersDone = std::function<void(std::vector<UserRef> members, std::string error)>;
    virtual void loadMembers(ConvRef conv, MembersDone done) {
        (void)conv;
        if (done)
            done({}, {});
    }
    // A GIF search service of the backend's own (the demo's stand-in GIPHY);
    // without one the composer's picker asks for a GIPHY key. An empty query
    // is "trending".
    struct Gif {
        std::string url, preview, title; // local paths or URLs
        int         width = 0, height = 0;
    };
    virtual bool gifSearchAvailable() const { return false; }
    virtual void searchGifs(std::string query, std::function<void(std::vector<Gif>)> done) {
        (void)query;
        if (done)
            done({});
    }
    // What the service predicts I'll type next (Claude Code's suggested
    // reply); "" = none.
    virtual std::string promptSuggestion(ConvRef) const { return {}; }

    // ── Channel canvases ────────────────────────────────────────────────────
    // A conversation's canvas is Conversation::canvasId / canvasTitle; these
    // calls keep both up to date in the Store. Slack has no read API for a
    // canvas: content arrives as the HTML its file serves (blocks carrying
    // section ids), and is written back as canvas markdown (real markdown,
    // not mrkdwn).
    // canvases.edit's operations: the title, the whole document, or one
    // section (a section diff; ids from the HTML's blocks).
    struct CanvasChange {
        enum class Op : uint8_t {
            Rename,
            ReplaceAll,
            ReplaceSection,
            DeleteSection,
            InsertBefore,
            InsertAfter,
        };
        Op          op = Op::ReplaceAll;
        std::string markdown;  // Rename: the new title; DeleteSection: unused
        std::string sectionId; // the *Section / Insert* ops
    };
    enum class CanvasState : uint8_t {
        Ok,       // there (title and link may still be "" after a passing failure)
        Gone,     // deleted, though the conversation still names it
        NoAccess, // there, but this account may not view it
    };
    // Looks the conversation's canvas up (conversations.info); done(fileId),
    // "" = none.
    virtual void loadChannelCanvas(ConvRef conv, std::function<void(std::string fileId)> done) {
        if (done)
            done(
                conv < _store.conversationCount() ? _store.conversation(conv).canvasId
                                                  : std::string()
            );
    }
    // The canvas's title (the tab) and permalink ("Copy link").
    using CanvasMetaDone =
        std::function<void(std::string title, std::string permalink, CanvasState state)>;
    virtual void loadCanvasMeta(const std::string &fileId, CanvasMetaDone done) {
        (void)fileId;
        if (done)
            done({}, {}, CanvasState::Ok);
    }
    // The canvas as HTML; error "" on success.
    using CanvasHtmlDone = std::function<void(std::string html, std::string error)>;
    virtual void loadCanvasContent(const std::string &fileId, CanvasHtmlDone done) {
        (void)fileId;
        if (done)
            done({}, "not_supported");
    }
    // Creates the conversation's canvas with `markdown` in it; done(fileId,
    // "") or done("", error) ("…already_exists": it had one we didn't know).
    using CanvasCreated = std::function<void(std::string fileId, std::string error)>;
    virtual void createChannelCanvas(ConvRef conv, std::string markdown, CanvasCreated done) {
        (void)conv, (void)markdown;
        if (done)
            done({}, "not_supported");
    }
    // Applies the changes in order; stops at the first failure.
    virtual void
    editCanvas(const std::string &fileId, std::vector<CanvasChange> changes, Done done) {
        (void)fileId, (void)changes;
        if (done)
            done(false, "not_supported");
    }
    // For everyone, for good (Slack has no undo).
    virtual void deleteCanvas(const std::string &fileId, Done done) {
        (void)fileId;
        if (done)
            done(false, "not_supported");
    }

    // One message by ts — top-level, a thread root or a reply — as a copy, for
    // what the loaded history may not hold (the Saved messages page's
    // previews). ok = false: gone, or not readable.
    // Default, for backends whose Store holds everything: the Store's copy,
    // answered at once.
    using MessageDone = std::function<void(bool ok, Message m)>;
    virtual void loadMessage(ConvRef conv, Ts ts, MessageDone done) {
        const Message *m = _store.findMessage(conv, ts);
        if (done)
            done(m != nullptr, m ? m->clone() : Message());
    }

    struct SearchHit {
        ConvRef     conv   = kNoConv;
        Ts          ts     = 0;
        Ts          thread = 0; // non-zero: a reply in that thread
        std::string text;       // the message's mrkdwn (it may not be loaded)
    };
    // Messages containing `query` (case-insensitive), newest first.
    virtual void search(std::string query, std::function<void(std::vector<SearchHit>)> done) = 0;

    // The sidebar's "Threads" page (Capabilities::threadsView):
    // the threads I follow, newest activity first. Copies,
    // not Store data: a root may be far older than the loaded history.
    struct FollowedThread {
        ConvRef              conv = kNoConv;
        Message              root;          // with its reply count and participants
        std::vector<Message> latestReplies; // the last few, oldest first
        Ts                   lastRead = 0;  // my read cursor inside the thread
    };
    struct ThreadsView {
        std::vector<FollowedThread> threads;
        int                         totalUnreadReplies = 0;
        bool                        hasMore            = false;
        std::string                 nextCursor; // the next call's `cursor`
    };
    // One page ("" = the first). ok = false: unavailable, or it failed.
    using ThreadsViewDone = std::function<void(bool ok, ThreadsView page)>;
    virtual void loadThreadsView(std::string cursor, ThreadsViewDone done) {
        (void)cursor;
        if (done)
            done(false, {});
    }
    // My read cursor in a followed thread moves to ts (best effort).
    virtual void markThreadRead(ConvRef conv, Ts root, Ts ts) { (void)conv, (void)root, (void)ts; }

    // A thread I follow (started, replied in, subscribed)
    // — its replies are important (notify, badge).
    virtual bool threadFollowed(ConvRef conv, Ts root) const {
        (void)conv, (void)root;
        return false;
    }
    // A user known only by id (a placeholder): looked up;
    // the Store fills the record in when it answers.
    virtual void resolveUser(UserRef u) { (void)u; }

    // A channel a message mentions that the roster doesn't list:
    // its name lands in Store::setChannelName, once.
    virtual void resolveChannel(const std::string &id) { (void)id; }

    // The account's own theme in the service (Capabilities::sidebarTheme):
    // Slack's redesign theme JSON and/or the
    // legacy comma-separated sidebar colours ("" each when absent).
    struct SidebarTheme {
        std::string iaTheme, legacyValues;
    };
    virtual void loadSidebarTheme(std::function<void(SidebarTheme, std::string error)> done) {
        if (done)
            done({}, "not_supported");
    }

    // The service's clock (epoch secs): the sidebar's relevance window ends
    // here. The fake workspace answers the moment its fixture is anchored to.
    virtual int64_t nowSecs() const { return base::nowSecs(); }

    Store &store() { return _store; }

    // Something the user should hear about that no call is waiting for (a
    // queued message Claude Code never took): the shell's error banner.
    // Set by whoever puts the backend on screen.
    std::function<void(const std::string &message)> onError;
    // A message reminder went off: its saved item
    // (Store::findSaved) carries the preview. Set like onError.
    std::function<void(ConvRef conv, Ts ts)>        onReminderDue;

protected:
    Store &_store;
};

} // namespace model
