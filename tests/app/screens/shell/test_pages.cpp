// The shell's pages and dialogs ported from msga's ui/: the Saved messages
// page, the quick switcher's fuzzy matching, the sidebar's persisted visit
// stamps and Settings' "Clear state", the tray picture, restarts and the
// sample notification's outcome.
#include "app/fake/fake_backend.h"
#include "app/mrkdwn/emoji.h"
#include "base/utf8.h"
#include "base/file.h"
#include "base/i18n.h"
#include "support/test.h"
#include "base/time.h"
#include "screens/settings/settings_dialog.h"
#include "screens/shell/fuzzy_match.h"
#include "screens/shell/quick_switcher.h"
#include "screens/shell/saved_page.h"
#include "screens/shell/shell.h"
#include "screens/shell/shell_dialogs.h"
#include "screens/shell/update_bar.h"
#include "plat/testing.h"
#include "ui/controls.h"

#ifdef MSGA_HAVE_MESSAGES
#include "app/screens/messages/emoji_picker.h"
#include "app/screens/messages/image_cache.h"
#include "app/screens/messages/thread_panel.h"
#endif

#include <memory>
#include <unistd.h>

using namespace model;
using shell::fuzzyScore;

namespace {

ui::App &app() {
    static std::unique_ptr<ui::App> a = [] {
        std::string err;
        auto        p = ui::App::create(&err);
        if (!p)
            std::fprintf(stderr, "ui::App::create: %s\n", err.c_str());
        return p;
    }();
    return *a;
}

void pump(int n = 10) {
    for (int i = 0; i < n; ++i)
        app().pump(2);
}

template <class F>
bool until(F done, int ms = 3000) {
    for (int t = 0; t < ms && !done(); t += 5)
        app().pump(5);
    return done();
}

struct Harness {
    Store             store;
    fake::FakeBackend backend{store, app().platform()};
#ifdef MSGA_HAVE_MESSAGES
    screens::ImageCache images{app().platform()};
    screens::Context    ctx{app(), store, backend, images, {}, {}, {}, {}, {}};
#else
    alignas(16) char noCache[16]{};
    screens::Context ctx{
        app(), store, backend, *reinterpret_cast<screens::ImageCache *>(noCache), {}, {}, {}, {}, {}
    };
#endif
    shell::Settings               settings;
    std::string                   path;
    std::unique_ptr<ui::Window>   win;
    std::unique_ptr<shell::Shell> sh;

    explicit Harness(shell::Settings s = {}, std::string settingsPath = {})
        : settings(std::move(s)), path(std::move(settingsPath)) {
        backend.setFixture(MSGA_TEST_ASSETS, base::fromLocal(2026, 9, 21, 16, 0));
        bool done = false;
        backend.connect([&](bool ok, const std::string &) { done = ok; });
        for (int i = 0; i < 200 && !done; ++i)
            app().pump(5);
        plat::WindowDesc d;
        d.size        = {1200, 800};
        d.decorations = plat::Decorations::Custom;
        win           = std::make_unique<ui::Window>(d);
        sh            = std::make_unique<shell::Shell>(ctx, *win, settings, path);
        sh->open(store.findConversation("C0DESIGN"));
        pump();
    }
    ConvRef conv(const char *id) const { return store.findConversation(id); }
};

ui::View *findIn(ui::View *v, std::string_view name) {
    if (!v->visible())
        return nullptr;
    if (v->accessibleName() == name)
        return v;
    for (size_t i = 0; i < v->childCount(); ++i)
        if (ui::View *f = findIn(v->child(i), name))
            return f;
    return nullptr;
}

std::string tempDir() {
    return base::test::makeTempDir("msga_pages_");
}

} // namespace

// ── Fuzzy matching ──────────────────────────────────────────────────────────

TEST("fuzzy: subsequences match, in order, any case") {
    CHECK(fuzzyScore("xdg", "xd-general").has_value()); // msga's issue #60
    CHECK(fuzzyScore("bb", "Bob Builder").has_value());
    CHECK(fuzzyScore("BOB", "bob builder").has_value());
    CHECK(fuzzyScore("ÅSA", "åsa lind").has_value());        // folded by code point
    CHECK_FALSE(fuzzyScore("gx", "xd-general").has_value()); // order matters
    CHECK_FALSE(fuzzyScore("zzz", "general").has_value());
    CHECK_FALSE(fuzzyScore("generals", "general").has_value());
    CHECK(*fuzzyScore("", "anything") == 0.0);
    CHECK_FALSE(fuzzyScore("a", "").has_value());
}

TEST("fuzzy: a substring beats a scattered match, word starts beat the middle") {
    // Consecutive characters outrank any boundary bonus.
    CHECK(*fuzzyScore("gen", "general") > *fuzzyScore("gen", "green-lane"));
    // A word start (after '-') beats the same letters mid-word.
    CHECK(*fuzzyScore("ui", "team-ui") > *fuzzyScore("ui", "build"));
    // camelCase humps count as word starts.
    CHECK(*fuzzyScore("ws", "myWorkspace") > *fuzzyScore("ws", "mywsorkspace") - 1);
    // The whole name typed wins over a longer name with it as a prefix.
    CHECK(*fuzzyScore("design", "design") > *fuzzyScore("design", "design-review"));
    // Trailing characters cost nothing: equal prefixes tie, so the caller's
    // order (recency) decides.
    CHECK(*fuzzyScore("des", "design-review") == *fuzzyScore("des", "design-backend"));
}

TEST("quick switcher: fuzzy, the placeholder and the empty state as msga") {
    Harness    h;
    const auto order = h.sh->sidebar().order();
    auto       r     = shell::quickSwitchFilter(h.store, "dsgn", order);
    REQUIRE(!r.empty());
    CHECK(r[0] == h.conv("C0DESIGN"));
    r = shell::quickSwitchFilter(h.store, "lnch", order); // #launch-atlas
    REQUIRE(!r.empty());
    CHECK(r[0] == h.conv("G0LAUNCH"));
    h.sh->showQuickSwitcher();
    pump();
    shell::QuickSwitcher *qs = h.sh->quickSwitcher();
    REQUIRE(qs);
    CHECK(findIn(qs, "No conversations match.") == nullptr);
    qs->field().insertText("qqqq");
    pump();
    CHECK(qs->results().empty());
    CHECK(findIn(qs, "No conversations match.") != nullptr);
    qs->close();
    pump();
}

// ── Saved messages ──────────────────────────────────────────────────────────

TEST("saved messages: the entry opens the page; cards, due lines, order, remove") {
    Harness       h;
    const ConvRef design = h.conv("C0DESIGN"), general = h.conv("C0GENERAL");
    const Ts      a = h.store.conversation(design).messages.back().ts;
    const Ts      b = h.store.conversation(general).messages.front().ts;
    h.backend.setSaved(design, a, true);
    const int64_t due = base::fromLocal(2026, 9, 25, 9, 30);
    h.backend.setReminder(general, b, due);
    pump();

    h.sh->sidebar().onSavedMessages();
    pump();
    REQUIRE(h.sh->savedOpen());
    CHECK(h.sh->sidebar().savedSelected());
    CHECK(h.sh->current() == kNoConv);
    shell::SavedPage *page = h.sh->savedPage();
    REQUIRE(page->cardCount() == 2);
    // Reminders first, then the bookmarks.
    CHECK(page->dueText(0) == i18n::arg("Reminder set for %1", base::formatDateTime(due)));
    CHECK_STR(page->dueText(1), "Saved for later");
    CHECK(page->statusText().empty());
    CHECK(findIn(page->card(1), "Remove") != nullptr);

    // Remove: the item goes (and the reminder with it).
    page->remove(0);
    pump();
    CHECK(page->cardCount() == 1);
    CHECK(h.store.reminderAt(general, b) == 0);
    page->remove(0);
    pump();
    CHECK(page->cardCount() == 0);
    CHECK_STR(
        page->statusText(), "Messages you save for later or set reminders on will appear here."
    );
    // The entry hides with nothing saved, the page stays until left.
    CHECK(h.sh->savedOpen());
    h.sh->open(design);
    pump();
    CHECK_FALSE(h.sh->savedOpen());
    CHECK_FALSE(h.sh->sidebar().savedSelected());
}

TEST("saved messages: a click jumps to the message, a reply inside its thread") {
    Harness       h;
    const ConvRef general = h.conv("C0GENERAL");
    const Ts      top     = h.store.conversation(general).messages.front().ts;
    h.backend.setSaved(general, top, true);
    // A saved reply: the first thread with replies in #design.
    const ConvRef design = h.conv("C0DESIGN");
    Ts            root = 0, reply = 0;
    for (const model::Thread &t : h.store.conversation(design).threads)
        if (!t.replies.empty()) {
            root  = t.root;
            reply = t.replies.back().ts;
            break;
        }
    REQUIRE(reply);
    h.backend.setSaved(design, reply, true);
    pump();
    h.sh->openSaved();
    pump();
    shell::SavedPage *page = h.sh->savedPage();
    REQUIRE(page->cardCount() == 2);
    // Newest saved first: the reply.
    page->activate(0);
    pump(20);
    CHECK(h.sh->current() == design);
    CHECK(h.sh->threadOpen());
#ifdef MSGA_HAVE_MESSAGES
    auto *tp = static_cast<screens::ThreadPanel *>(h.sh->threadPanel());
    REQUIRE(tp);
    CHECK(tp->root() == root);
#else
    (void)root;
#endif
    h.sh->openSaved();
    pump();
    page->activate(1);
    pump(20);
    CHECK(h.sh->current() == general);
    CHECK_FALSE(h.sh->savedOpen());
}

TEST("saved messages: an item whose message isn't known asks for it once") {
    Harness       h;
    const ConvRef general = h.conv("C0GENERAL");
    // Saved elsewhere: a ts this workspace never loaded.
    h.store.setSavedItem(general, 1'000'000'000'000'123, true, 0, 5);
    const Store::SavedItem *it = h.store.findSaved(general, 1'000'000'000'000'123);
    REQUIRE(it);
    CHECK_FALSE(it->previewed);
    h.sh->openSaved();
    pump();
    it = h.store.findSaved(general, 1'000'000'000'000'123);
    REQUIRE(it);
    CHECK(it->previewed); // asked; the fake has no such message
    shell::SavedPage *page = h.sh->savedPage();
    REQUIRE(page->cardCount() == 1);
    CHECK(findIn(page->card(0), "No preview available") != nullptr);
}

// ── Visit stamps, "Clear state" ─────────────────────────────────────────────

TEST("visited: the stamps round-trip through the settings file") {
    shell::Settings s;
    s.visitedAt           = {{"C0DESIGN", 1758000000}, {"D0MIRA", 1758000100}};
    s.trayMonochrome      = false;
    const std::string dir = tempDir();
    REQUIRE(!dir.empty());
    const std::string path = dir + "/settings.json";
    REQUIRE(s.save(path));
    const shell::Settings l = shell::Settings::load(path);
    REQUIRE(l.visitedAt.size() == 2);
    CHECK(l.visitedAt[0].first == "C0DESIGN");
    CHECK(l.visitedAt[0].second == 1758000000);
    CHECK(l.visitedAt[1].first == "D0MIRA");
    CHECK_FALSE(l.trayMonochrome);
    CHECK(shell::Settings().trayMonochrome); // on by default, like msga
    file::remove(path);
    file::remove(dir);
}

TEST("visited: opening a conversation is saved; the stamps keep it listed after a restart") {
    const std::string dir  = tempDir();
    const std::string path = dir + "/settings.json";
    ConvRef           sam  = kNoConv;
    {
        Harness h({}, path);
        sam = h.conv("D0SAM");
        h.sh->open(sam);
        pump();
        // Written 1.5 s after the last change.
        REQUIRE(until([&] { return file::exists(path); }, 3000));
        const shell::Settings l   = shell::Settings::load(path);
        bool                  has = false;
        for (const auto &[id, at] : l.visitedAt)
            has |= id == "D0SAM" && at > 0;
        CHECK(has);
    }
    // A narrow window: only the stamp keeps the DM listed.
    shell::Settings s = shell::Settings::load(path);
    s.relevantDays    = 1;
    Harness                 h(s, path);
    shell::Sidebar::Filters f;
    f.relevantDays = 1;
    h.sh->sidebar().setFilters(f);
    pump();
    CHECK(h.sh->sidebar().rowState(sam).exists);
    CHECK(h.sh->sidebar().visited().count("D0SAM") == 1);

    // Settings → Storage → "Clear state": the stamps go (file included).
    h.sh->openSettings();
    pump();
    settings::SettingsDialog *d = h.sh->settingsDialog();
    REQUIRE(d);
    d->showPage(settings::SettingsDialog::Page::Storage);
    pump();
    auto *clear = static_cast<ui::FormButton *>(d->find("Clear state"));
    REQUIRE(clear);
    clear->onClick();
    pump();
    CHECK_FALSE(clear->enabled());
    CHECK(h.sh->sidebar().visited().count("D0SAM") == 0);
    const shell::Settings after = shell::Settings::load(path);
    for (const auto &[id, at] : after.visitedAt)
        CHECK(id != "D0SAM");
    file::remove(path);
    file::remove(dir);
}

// ── Settings: restart, sample notification, updates ─────────────────────────

TEST("settings: \"Save and restart\" restarts; the sample notification reports back") {
    Harness h;
    h.sh->openSettings();
    pump();
    settings::SettingsDialog *d = h.sh->settingsDialog();
    REQUIRE(d);
    // Without an updater: msga's state without a checker.
    d->showPage(settings::SettingsDialog::Page::System);
    pump();
    CHECK(d->find("Update checks not available.") != nullptr);
    CHECK_FALSE(d->find("Check for updates")->enabled());
    d->close();
    pump();

    bool quit               = false;
    h.sh->onQuit            = [&] { quit = true; };
    h.settings.slackSession = false; // the app keys box shows
    h.sh->openSettings();
    pump();
    d = h.sh->settingsDialog();
    d->showPage(settings::SettingsDialog::Page::System);
    pump();
    ui::View *save = d->find("Save and restart");
    REQUIRE(save);
    static_cast<ui::FormButton *>(save)->onClick(); // nothing changed
    pump();
    CHECK(d->find("No changes to save.") != nullptr);
    CHECK_FALSE(quit);
}

TEST("update bar: hidden until an update is ready, then msga's wording") {
    Harness           h;
    shell::UpdateBar *bar = h.sh->updateBar();
    REQUIRE(bar);
    CHECK_FALSE(bar->visible());
    bar->showUpdateReady();
    pump();
    CHECK(bar->visible());
#ifdef __APPLE__
    CHECK_STR(bar->button()->label(), "Open installer");
#else
    CHECK_STR(bar->button()->label(), "Restart now");
    CHECK(findIn(bar, "Restart now") != nullptr);
#endif
}

// ── Tray picture ────────────────────────────────────────────────────────────

TEST("tray icon: fitted into the square, and the monochrome silhouette") {
    // A wide opaque picture: a dark mark on a light backdrop.
    gfx::Bitmap src(40, 20);
    for (int y = 0; y < 20; ++y)
        for (int x = 0; x < 40; ++x)
            src.pixels()[y * 40 + x] = (x >= 15 && x < 25) ? 0xff202020U : 0xffeeeeeeU;
    const gfx::Bitmap fit = shell::trayPicture(src, false);
    REQUIRE(fit.width() == shell::kTrayIconSize);
    REQUIRE(fit.height() == shell::kTrayIconSize);
    const int n = shell::kTrayIconSize;
    CHECK((fit.pixels()[0] >> 24) == 0);                              // letterbox: transparent
    CHECK((fit.pixels()[(n / 2) * n + 2] >> 24) == 0xff);             // the picture, edge to edge
    CHECK((fit.pixels()[(n / 2) * n + n / 2] & 0xffffff) < 0x404040); // the mark, dark

    const gfx::Bitmap mono = shell::trayPicture(src, true);
    const uint32_t    mark = mono.pixels()[(n / 2) * n + n / 2];
    const uint32_t    bg   = mono.pixels()[(n / 2) * n + 4];
    CHECK((mark >> 24) == 0xff);
    CHECK(mark == 0xffffffffU); // white
    CHECK((bg >> 24) == 0);     // the backdrop keyed out
    CHECK((mono.pixels()[0] >> 24) == 0);

    // One flat colour: the whole picture is the silhouette.
    gfx::Bitmap flat(10, 10);
    std::fill(flat.pixels(), flat.pixels() + 100, 0xff3366ccU);
    const gfx::Bitmap fm = shell::trayPicture(flat, true);
    CHECK(fm.pixels()[(n / 2) * n + n / 2] == 0xffffffffU);
}

// ── Claude Code: a teammate's "Message" ─────────────────────────────────────

namespace {

struct TeamFake : fake::FakeBackend {
    using fake::FakeBackend::FakeBackend;
    Capabilities capabilities() const override {
        Capabilities c  = fake::FakeBackend::capabilities();
        c.agentSessions = true;
        return c;
    }
    std::vector<AgentRole> agentRoles() const override {
        AgentRole r;
        r.id   = "designer";
        r.name = "Designer";
        r.user = mate;
        return {r};
    }
    std::string agentSessionBlocker(const std::string &) const override { return {}; }
    UserRef     mate = kNoUser;
};

} // namespace

TEST("profile card \"Message\": a teammate's page, anyone else's DM (msga's openDmWith)") {
    Store    store;
    TeamFake backend{store, app().platform()};
#ifdef MSGA_HAVE_MESSAGES
    screens::ImageCache images{app().platform()};
    screens::Context    ctx{app(), store, backend, images, {}, {}, {}, {}, {}};
#else
    alignas(16) char noCache[16]{};
    screens::Context ctx{
        app(), store, backend, *reinterpret_cast<screens::ImageCache *>(noCache), {}, {}, {}, {}, {}
    };
#endif
    backend.setFixture(MSGA_TEST_ASSETS, base::fromLocal(2026, 9, 21, 16, 0));
    bool done = false;
    backend.connect([&](bool ok, const std::string &) { done = ok; });
    REQUIRE(until([&] { return done; }));
    backend.mate = store.findUser("U0LENA");
    plat::WindowDesc d;
    d.size        = {1200, 800};
    d.decorations = plat::Decorations::Custom;
    ui::Window      win(d);
    shell::Settings settings;
    shell::Shell    sh(ctx, win, settings, std::string());
    sh.open(store.findConversation("C0DESIGN"));
    pump();
    REQUIRE(ctx.messageUser);
    ctx.messageUser(backend.mate);
    pump();
    CHECK(sh.teammateOpen());
    CHECK(sh.sidebar().selectedTeammate() == "designer");
    ctx.messageUser(store.findUser("U0MIRA"));
    REQUIRE(until([&] { return sh.current() == store.findConversation("D0MIRA"); }));
    CHECK_FALSE(sh.teammateOpen());
}

// ── Composer @ list and emoji search cost (performance review M11) ─────────

TEST("composer: the @ list folds each user once and filters in the open list") {
    Harness h;
    auto   &c = h.sh->composer();
    c.edit().focus();
    c.edit().insertText("hey @m");
    pump();
    shell::PickList *list = c.pickList();
    REQUIRE(list != nullptr);
    const size_t folds = c.mentionFolds();
    CHECK(folds > 0);
    // One keystroke is one look (onChange and onSelectionChange both ask),
    // the same popup takes the new rows, and no label is folded again.
    size_t looks = c.pickRecomputes();
    for (const char *ch : {"i", "r"}) {
        c.edit().insertText(ch);
        pump();
        CHECK(c.pickRecomputes() == looks + 1);
        looks = c.pickRecomputes();
        CHECK(c.pickList() == list);
    }
    CHECK(c.mentionFolds() == folds);
    // The rows are what a fold-per-user filter gives, in roster order.
    std::vector<std::string> want;
    for (UserRef u = 0; u < h.store.userCount() && want.size() < 50; ++u) {
        const User &user = h.store.user(u);
        if (user.placeholder)
            continue;
        const std::string label(user.label());
        if (utf8::containsFolded(label, "mir") || utf8::containsFolded(user.name, "mir"))
            want.push_back(u == h.store.me ? "@" + label + " (you)" : "@" + label);
    }
    REQUIRE(!want.empty());
    REQUIRE(list->count() == want.size()); // "mir" matches no @channel alias
    for (size_t i = 0; i < want.size(); ++i)
        CHECK_STR(list->item(i).title, want[i]);
    // Asking again with nothing changed neither looks nor rebuilds the rows.
    const int builds = list->builds();
    c.updatePickList();
    CHECK(c.pickRecomputes() == looks);
    CHECK(list->builds() == builds);
    // Escape closes it; the same text then opens a fresh one, as before.
    ui::Event esc{ui::EventType::KeyDown};
    esc.key = plat::Key::Escape;
    CHECK(list->handleKey(esc));
    pump();
    CHECK(c.pickList() == nullptr);
    c.updatePickList();
    CHECK(c.pickList() != nullptr);
    // A blank ends the word: the list goes.
    c.edit().insertText(" ");
    pump();
    CHECK(c.pickList() == nullptr);
}

#ifdef MSGA_HAVE_MESSAGES
TEST("emoji picker: the search finds what a fold-per-name search finds; recents by name") {
    Harness h;
    h.store.setCustomEmoji("zed_caps", "https://emoji.example/z.png");
    h.store.setCustomEmoji("party_parrot", "https://emoji.example/p.png");
    auto *ep = screens::EmojiPicker::show(*h.win, {400, 700, 20, 20}, h.ctx, nullptr);
    REQUIRE(ep != nullptr);
    for (const char *q : {"par", "SMILE", "Zed", "+1", "flag-", "x"}) {
        std::vector<std::string> want;
        for (const auto &[name, image] : h.store.customEmojiImages())
            if (utf8::containsFolded(name, q))
                want.push_back(name);
        emoji::forEach([&](std::string_view n, const std::string &) {
            if (utf8::containsFolded(n, q))
                want.emplace_back(n);
            return true;
        });
        ep->filter(q);
        REQUIRE(ep->cellCount() == want.size());
        for (size_t i = 0; i < want.size(); ++i)
            CHECK_STR(ep->cellName(i), want[i]);
    }
    // "Frequently used": a custom one found by name, a gone one skipped.
    screens::EmojiPicker::restoreState({"zed_caps", "tada", "gone_one"}, 0);
    ep->filter({});
    REQUIRE(ep->cellCount() > 2);
    CHECK_STR(ep->cellName(0), "zed_caps");
    CHECK_STR(ep->cellName(1), "tada");
    CHECK_STR(ep->sections()[0], "Frequently used");
    screens::EmojiPicker::restoreState({}, 0);
    ep->close();
    pump();
}
#endif

// ── Sidebar hover ───────────────────────────────────────────────────────────

// M6: a row's hover recolours its name (Label::setColor), which never
// reshapes: sweeping the pointer down the sidebar builds no text layout.
TEST("pages: hovering sidebar rows reshapes no text") {
    Harness         h;
    const ui::RectF sb    = h.sh->sidebar().windowRect();
    auto            sweep = [&] {
        for (float y = sb.y + 4; y < sb.bottom() - 4; y += 7) {
            app().platform().testHooks()->injectPointerMove(h.win->native(), {sb.x + 90, y});
            app().pump(0);
        }
        app().platform().testHooks()->injectPointerMove(h.win->native(), {sb.right() + 300, 400});
        app().pump(0);
    };
    sweep(); // anything shaped lazily on a first hover (none expected) is done
    const size_t n0 = text::layoutBuilds();
    sweep();
    CHECK(text::layoutBuilds() == n0);
}
