// The workspace cache (app/cache/workspace_cache.h) under SlackBackend,
// against the local fake Slack: a cold start writes it, a warm start shows
// it before any answer and then merges the network's, cached history meets
// the head page without holes or ghosts, the wipes, and damaged files.
#include "app/cache/workspace_cache.h"
#include "app/slack/slack_backend.h"
#include "support/fake_slack_server.h"
#include "base/file.h"
#include "support/test.h"

#include <cstdlib>
#include <memory>
#include <string>

#ifndef _WIN32

namespace {

using fakeslack::app;
using fakeslack::ctl;
using fakeslack::pumpUntil;
using model::ConvRef;
using model::kNoConv;
using model::Ts;

void set(const std::string &json) {
    ctl("POST", "/_ctl/set", json);
}

const char *kWorkspace = R"({
 "auth.test": {"ok": true, "user_id": "UME", "team_id": "T1", "team": "Lumen",
               "url": "https://lumen.slack.com/"},
 "users.list": {"ok": true, "members": [
     {"id": "UME", "name": "me", "profile": {"real_name": "Me Myself"}},
     {"id": "UMIRA", "name": "mira", "profile": {"real_name": "Mira Okafor",
                                                 "image_72": "https://a/mira72.png"}}],
   "response_metadata": {"next_cursor": ""}},
 "conversations.list": {"ok": true, "channels": [
     {"id": "C1", "name": "general", "is_channel": true, "is_member": true,
      "topic": {"value": "Company news"}},
     {"id": "D1", "is_im": true, "user": "UMIRA"}],
   "response_metadata": {"next_cursor": ""}},
 "emoji.list": {"ok": true, "emoji": {"party": "https://e/party.png", "yay": "alias:party"}},
 "usergroups.list": {"ok": true, "usergroups": [{"id": "S1", "users": ["UME"]}]},
 "users.getPresence": {"ok": true, "presence": "active"},
 "conversations.history": {"ok": true, "has_more": false, "messages": [
     {"type": "message", "ts": "1700000003.000000", "user": "UMIRA", "text": "third",
      "reactions": [{"name": "tada", "count": 1, "users": ["UME"]}]},
     {"type": "message", "ts": "1700000002.000000", "user": "UME", "text": "second"},
     {"type": "message", "ts": "1700000001.000000", "user": "UMIRA", "text": "first",
      "files": [{"id": "F1", "name": "cat.png", "mimetype": "image/png",
                 "url_private": "https://files/cat.png", "original_w": 16, "original_h": 9}]}]}
})";

std::string cacheDir() {
    return cache::WorkspaceCache::dirFor(app(), "slack:T1");
}

// One backend on a Store, cache opened by hand (as the accounts controller
// does at activation).
struct Env {
    model::Store                         store;
    net::Client                          client{app()};
    std::unique_ptr<slack::SlackBackend> be;
    bool                                 warm = false;

    explicit Env(bool reset = true) {
        base::test::setEnv("MSGA_SLACK_TEST_SPEEDUP", "100");
        if (reset) {
            ctl("POST", "/_ctl/reset");
            set(kWorkspace);
        }
        slack::Credentials cr;
        cr.token  = "xoxc-test";
        cr.cookie = "xoxd-test";
        cr.teamId = "T1";
        be        = std::make_unique<slack::SlackBackend>(store, app(), client, cr);
        warm      = be->openCache(cacheDir());
    }
    ~Env() {
        be.reset(); // the last write happens here
        base::test::unsetEnv("MSGA_SLACK_TEST_SPEEDUP");
    }
    bool connect() {
        bool called = false, ok = false;
        be->connect([&](bool o, const std::string &) {
            called = true;
            ok     = o;
        });
        return pumpUntil([&] { return called && !be->connecting(); }) && ok;
    }
    ConvRef                    conv(const char *id) const { return store.findConversation(id); }
    const model::Conversation &c(const char *id) const { return store.conversation(conv(id)); }
};

bool haveServer() {
    if (!fakeslack::server().empty())
        return true;
    std::fprintf(stderr, "  skip: no fake Slack (python3 missing)\n");
    return false;
}

void wipe() {
    cache::WorkspaceCache::remove(app(), "slack:T1");
}

// A cold start that opened #general, read its history and quit.
void coldStart() {
    wipe();
    Env e;
    REQUIRE(!e.warm);
    REQUIRE(e.connect());
    const ConvRef general = e.conv("C1");
    REQUIRE(general != kNoConv);
    e.be->setActiveConversation(general, 0);
    bool done = false;
    e.be->loadHistory(general, 0, [&](bool, const std::string &) { done = true; });
    REQUIRE(pumpUntil([&] { return done; }));
    REQUIRE(pumpUntil([&] { return !e.store.emojiFor("yay").image.empty(); }, 3000));
    REQUIRE(pumpUntil([&] { return e.store.myGroups.size() == 1; }, 3000));
    // Local state the server never echoes back.
    e.store.updateConversation(e.conv("D1"), [](model::Conversation &x) {
        x.localName = "Mira (design)";
    });
    e.store.setAiTranscript("F1", "Hello there", "OpenAI"); // "Transcribe with AI"
    e.store.setThreadMuted(general, model::parseTs("1700000002.000000"), true);
}

} // namespace

TEST("slack cache: a cold start writes it, a warm start shows it before the network answers") {
    if (!haveServer())
        return;
    coldStart();
    const std::string dir = cacheDir();
    for (const char *f :
         {"roster.json", "users.json", "emoji.json", "meta.json", "messages/C1.json"})
        CHECK(file::exists(file::join(dir, f)));

    // While we were away: a new channel, a message deleted and one added.
    // And a slow Slack: auth.test takes its time.
    ctl("POST", "/_ctl/reset");
    set(kWorkspace);
    set(
        R"({"auth.test": {"ok": true, "user_id": "UME", "team_id": "T1", "team": "Lumen",
                          "__delay": 0.6},
            "conversations.list": {"ok": true, "channels": [
              {"id": "C1", "name": "general", "is_channel": true, "is_member": true},
              {"id": "C3", "name": "design", "is_channel": true, "is_member": true},
              {"id": "D1", "is_im": true, "user": "UMIRA"}],
            "response_metadata": {"next_cursor": ""}},
            "conversations.history": {"ok": true, "has_more": false, "messages": [
              {"type": "message", "ts": "1700000004.000000", "user": "UMIRA", "text": "fourth"},
              {"type": "message", "ts": "1700000003.000000", "user": "UMIRA", "text": "third"},
              {"type": "message", "ts": "1700000001.000000", "user": "UMIRA", "text": "first"}]}})"
    );
    Env e(false);
    REQUIRE(e.warm);
    // Everything below is in the Store before a single answer arrived (the
    // loop has not even run).
    REQUIRE(e.conv("C1") != kNoConv && e.conv("D1") != kNoConv);
    CHECK_STR(e.c("C1").topic, "Company news");
    CHECK_STR(e.store.displayName(e.conv("D1")), "Mira (design)");
    CHECK_STR(e.store.user(e.store.findUser("UMIRA")).avatar, "https://a/mira72.png");
    CHECK(!e.store.user(e.store.findUser("UMIRA")).placeholder);
    CHECK_STR(e.store.user(e.store.me).id, "UME");
    CHECK_STR(e.store.emojiFor("yay").image, "https://e/party.png");
    REQUIRE(e.store.myGroups.size() == 1);
    CHECK(e.store.threadMuted(e.conv("C1"), model::parseTs("1700000002.000000")));
    REQUIRE(e.store.aiTranscript("F1") != nullptr);
    CHECK_STR(e.store.aiTranscript("F1")->text, "Hello there");
    CHECK_STR(e.store.aiTranscript("F1")->by, "OpenAI");
    // The chat that was open, and its messages the moment it opens again.
    CHECK(e.be->lastConversation() == e.conv("C1"));
    e.be->setActiveConversation(e.conv("C1"), 0);
    const model::Conversation &general = e.c("C1");
    REQUIRE(general.messages.size() == 3);
    CHECK(!general.hasMoreBefore); // the cache held the whole history
    CHECK_STR(general.messages[0].text, "first");
    REQUIRE(general.messages[0].files().size() == 1);
    CHECK_STR(general.messages[0].files()[0].path, "https://files/cat.png");
    REQUIRE(general.messages[2].reactions.size() == 1);
    CHECK(general.messages[2].reactions[0].users[0] == e.store.me);
    CHECK(general.messages[1].user == e.store.me);
    CHECK_STR(general.messages[1].text, "second");

    // Then the network, merged in; local state survives it.
    REQUIRE(e.connect());
    CHECK(e.conv("C3") != kNoConv);
    CHECK_STR(e.store.displayName(e.conv("D1")), "Mira (design)");
    REQUIRE(pumpUntil(
        [&] {
            return e.c("C1").messages.size() == 3 && e.c("C1").messages.back().text == "fourth";
        },
        3000
    ));
    CHECK_STR(e.c("C1").messages[1].text, "third"); // "second" was deleted
}

TEST("slack cache: a presence flip doesn't re-serialise users.json; a profile change does") {
    if (!haveServer())
        return;
    wipe();
    Env e;
    // Mira's presence is ours to flip: the sweep stops asking after a failure.
    set(R"({"users.getPresence?user=UMIRA": {"ok": false, "error": "internal_error"}})");
    REQUIRE(e.connect());
    const std::string path = file::join(cacheDir(), "users.json");
    REQUIRE(pumpUntil([&] { return file::exists(path); }, 5000));
    fakeslack::pumpFor(1500); // the first presence round and its write settle
    std::string before;
    REQUIRE(file::readAll(path, &before));
    const model::UserRef mira = e.store.findUser("UMIRA");
    REQUIRE(mira != model::kNoUser);

    // Presence only, waited past the 1 s write throttle: no write.
    e.store.user(mira).active = !e.store.user(mira).active;
    e.store.usersChanged();
    fakeslack::pumpFor(1500);
    std::string after;
    REQUIRE(file::readAll(path, &after));
    CHECK(after == before);

    // A profile change is written (with the presence it carries).
    e.store.user(mira).displayName = "Mira O.";
    e.store.usersChanged();
    REQUIRE(pumpUntil(
        [&] {
            std::string now;
            return file::readAll(path, &now) && now != before;
        },
        3000
    ));
    std::string profile;
    REQUIRE(file::readAll(path, &profile));
    CHECK(profile.find("Mira O.") != std::string::npos);

    // Quitting saves the last presence: the next start's dots.
    e.store.user(mira).active = !e.store.user(mira).active;
    e.store.usersChanged();
    e.be.reset();
    std::string closed;
    REQUIRE(file::readAll(path, &closed));
    CHECK(closed != profile);
}

TEST("slack cache: a cached run the head page doesn't reach is replaced, not left with a hole") {
    if (!haveServer())
        return;
    coldStart();
    ctl("POST", "/_ctl/reset");
    set(kWorkspace);
    // Many messages since: the head page starts after the cached ones.
    set(
        R"({"conversations.history": {"ok": true, "has_more": true, "messages": [
              {"type": "message", "ts": "1700000200.000000", "user": "UMIRA", "text": "new 2"},
              {"type": "message", "ts": "1700000100.000000", "user": "UMIRA", "text": "new 1"}]}})"
    );
    Env e(false);
    REQUIRE(e.warm);
    e.be->setActiveConversation(e.conv("C1"), 0);
    const model::Conversation &general = e.c("C1");
    REQUIRE(general.messages.size() == 3);
    REQUIRE(pumpUntil([&] { return general.messages.size() == 2; }, 3000));
    CHECK_STR(general.messages[0].text, "new 1");
    CHECK(general.hasMoreBefore); // paging continues from the head page down
}

TEST("slack cache: reminder previews persist, so a restart doesn't fetch them again") {
    if (!haveServer())
        return;
    wipe();
    const Ts ts = model::parseTs("1800000000.000100");
    {
        Env e;
        set(R"({"saved.list": {"ok": true, "saved_items": [
                  {"item_type": "message", "item_id": "C1", "ts": "1800000000.000100",
                   "date_due": 0, "date_created": 1}]},
                "conversations.replies?ts=1800000000.000100": {"ok": true, "messages": [
                  {"type": "message", "ts": "1800000000.000100", "user": "UMIRA",
                   "text": "the report <@UME>"}]}})");
        REQUIRE(e.connect());
        const ConvRef c = e.conv("C1");
        REQUIRE(pumpUntil([&] { return e.store.findSaved(c, ts) != nullptr; }, 5000));
        // What the Saved page does for a card it has no preview for.
        bool loaded = false;
        e.be->loadMessage(c, ts, [&](bool ok, model::Message m) {
            e.store.setSavedPreview(c, ts, ok ? &m : nullptr);
            loaded = true;
        });
        REQUIRE(pumpUntil([&] { return loaded; }));
        REQUIRE(e.store.findSaved(c, ts)->previewed);
    }
    Env e(false);
    REQUIRE(e.warm);
    // Before any answer: the item, with what its message said.
    const model::Store::SavedItem *it = e.store.findSaved(e.conv("C1"), ts);
    REQUIRE(it);
    CHECK(it->previewed);
    CHECK_STR(it->text, "the report <@UME>");
    CHECK(it->author == e.store.findUser("UMIRA"));
    CHECK(it->thread == 0);
}

TEST("slack cache: sign-out and Clear cache wipe it; an open cache then writes nothing") {
    if (!haveServer())
        return;
    coldStart();
    const std::string dir = cacheDir();
    REQUIRE(file::exists(file::join(dir, "roster.json")));
    CHECK(cache::WorkspaceCache::diskBytes(app()) > 0);
    wipe(); // the accounts controller's sign-out
    CHECK(!file::exists(dir));
    CHECK(cache::WorkspaceCache::diskBytes(app()) == 0);

    coldStart();
    {
        Env e(false);
        REQUIRE(e.warm);
        cache::WorkspaceCache::clearAll(app()); // Settings → Clear cache
        CHECK(!file::exists(dir));
        e.store.updateConversation(e.conv("C1"), [](model::Conversation &x) { x.localName = "x"; });
        e.be->setActiveConversation(e.conv("D1"), 0);
    } // destroyed: would write now
    CHECK(!file::exists(dir));
    Env cold(false);
    CHECK(!cold.warm); // the next start is a cold one
}

TEST("slack cache: damaged or foreign files are ignored safely") {
    if (!haveServer())
        return;
    const std::string dir = cacheDir();
    const auto        put = [&](const char *name, const char *text) {
        REQUIRE(file::writeAtomic(file::join(dir, name), text));
    };
    wipe();
    put("roster.json", "{\"v\":1,\"c\":[[\"C1\",\"gen");
    {
        Env e(false);
        CHECK(!e.warm);
        CHECK(e.store.conversationCount() == 0);
    }
    wipe();
    put("roster.json", "{\"v\":99,\"c\":[[\"C1\",\"general\"]]}");
    {
        Env e(false);
        CHECK(!e.warm);
    }
    // A usable roster with odd values, everything else broken.
    wipe();
    put("roster.json",
        "{\"v\":1,\"c\":[[\"C1\",\"general\",\"\",77,\"\",{},\"x\",1,1,1,1,1,"
        "200],[],[\"\"],7,\"nope\"]}");
    put("users.json", "garbage");
    put("meta.json", "{\"v\":1,\"me\":5,\"muted\":[[\"CX\",1],7],\"rem\":{\"a\":1}}");
    put("emoji.json", "{\"v\":1,\"e\":[1,2]}");
    put("messages/C1.json", "{\"v\":1,\"m\":[[0],[\"bad\"],[1700000001000000,1,2,3,4,5,6,7,8,9]]}");
    {
        Env e(false);
        REQUIRE(e.warm);
        REQUIRE(e.store.conversationCount() == 1);
        const model::Conversation &c = e.c("C1");
        CHECK(c.kind == model::ConvKind::Channel);      // 77 is no kind
        CHECK(c.notify == model::NotifyLevel::Default); // nor is 200 a level
        CHECK(e.store.me == model::kNoUser);
        e.be->setActiveConversation(e.conv("C1"), 0);
        // Only the record with a real ts survives; the junk fields default.
        REQUIRE(c.messages.size() == 1);
        CHECK(!c.messages[0].isReply());
    }
    wipe();
}

TEST("slack read: connect is done once the conversations are in, users may follow") {
    if (!haveServer())
        return;
    wipe();
    ctl("POST", "/_ctl/reset");
    set(kWorkspace);
    set(R"({"users.list": {"ok": true, "members": [
              {"id": "UMIRA", "name": "mira", "profile": {"real_name": "Mira Okafor"}}],
            "response_metadata": {"next_cursor": ""}, "__delay": 0.8}})");
    Env  e(false);
    bool called = false, ok = false;
    e.be->connect([&](bool o, const std::string &) {
        called = true;
        ok     = o;
    });
    REQUIRE(pumpUntil([&] { return called; }));
    CHECK(ok);
    CHECK(e.be->connecting()); // users.list is still on its way
    REQUIRE(e.conv("D1") != kNoConv);
    CHECK(e.store.user(e.c("D1").dmUser).placeholder);
    REQUIRE(pumpUntil([&] { return !e.be->connecting(); }, 5000));
    CHECK_STR(e.store.displayName(e.conv("D1")), "Mira Okafor");
    wipe();
}

#endif
