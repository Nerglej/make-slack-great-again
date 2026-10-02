// Backend-independent tests: the loop core (timers, posting) and the event
// contract, driven through the headless backend.
#include "core/loop_core.h"
#include "plat/plat.h"
#include "plat/testing.h"
#include "test_util.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

using namespace plat;
using plat_test::runCase;

namespace plat::testing_internal {
void    setHeadlessScale(Window &w, double s);
int     headlessFrames(Window &w);
Cursor  headlessCursor(Window &w);
HitArea headlessLastHit(Window &w);
} // namespace plat::testing_internal
namespace ti = plat::testing_internal;

namespace {

std::unique_ptr<App> headless() {
#if defined(_WIN32)
    _putenv_s("PLAT_BACKEND", "headless");
#else
    setenv("PLAT_BACKEND", "headless", 1);
#endif
    return App::create();
}

// Pump until pred() or the deadline; returns pred().
bool pumpUntil(App &app, const std::function<bool()> &pred, int ms = 1000) {
    const auto end = core::Clock::now() + std::chrono::milliseconds(ms);
    while (!pred() && core::Clock::now() < end)
        app.pump(10);
    return pred();
}

void testTimersOrderAndCancel() {
    core::LoopCore   core;
    std::vector<int> order;
    core.addTimer(20, false, [&] { order.push_back(2); });
    core.addTimer(5, false, [&] { order.push_back(1); });
    const TimerId cancelled = core.addTimer(10, false, [&] { order.push_back(99); });
    core.cancelTimer(cancelled);
    CHECK(core.msUntilNextTimer() >= 0);
    CHECK(core.msUntilNextTimer() <= 5);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(core.msUntilNextTimer() == 0);
    core.runDueTimers();
    CHECK((order == std::vector<int>{1, 2}));
    CHECK(core.msUntilNextTimer() == -1);
}

void testRepeatingTimerRearmsAndSelfCancels() {
    core::LoopCore core;
    int            fired = 0;
    TimerId        id    = 0;
    id                   = core.addTimer(1, true, [&] {
        if (++fired == 3)
            core.cancelTimer(id); // cancelling yourself mid-callback must be safe
    });
    for (int i = 0; i < 20 && fired < 3; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        core.runDueTimers();
    }
    CHECK(fired == 3);
    CHECK(core.msUntilNextTimer() == -1);
}

void testClampTimeout() {
    core::LoopCore core;
    CHECK(core.clampTimeout(-1) == -1);
    CHECK(core.clampTimeout(50) == 50);
    core.addTimer(1000, false, [] {});
    CHECK(core.clampTimeout(-1) > 900);
    CHECK(core.clampTimeout(10) == 10);
}

// A closure whose captures, when destroyed, post `depth` more like it — from
// a thread joined meanwhile when `viaThread`, like a retired net::WebSocket
// whose reader posts its close while the destructor joins it.
struct OnDestroy {
    std::function<void()> fn;
    ~OnDestroy() { fn(); }
};
std::function<void()>
reposting(core::LoopCore &c, int depth, bool viaThread, int &destroyed, bool &ran) {
    auto token = std::make_shared<OnDestroy>();
    token->fn  = [&c, depth, viaThread, &destroyed, &ran] {
        ++destroyed;
        if (depth == 0)
            return;
        auto next = reposting(c, depth - 1, viaThread, destroyed, ran);
        if (viaThread)
            std::thread([&] { c.post(std::move(next)); }).join();
        else
            c.post(std::move(next));
    };
    return [token, &ran] { ran = true; };
}

void testShutdownDrainsClosuresThatPost() {
    int  destroyed = 0;
    bool ran       = false;
    {
        core::LoopCore c;
        c.post(reposting(c, 3, false, destroyed, ran));
        c.post(reposting(c, 3, true, destroyed, ran));
        c.addTimer(1000, false, reposting(c, 3, true, destroyed, ran));
        c.shutdown();
        CHECK(destroyed == 12);
        CHECK(!c.hasPosted());
        CHECK(c.msUntilNextTimer() == -1);
        // Closed: a late post is dropped at once.
        c.post(reposting(c, 0, false, destroyed, ran));
        CHECK(destroyed == 13);
        CHECK(!c.hasPosted());
        // The destructor alone drains too.
        core::LoopCore d;
        d.post(reposting(d, 2, true, destroyed, ran));
    }
    CHECK(destroyed == 16);
    CHECK(!ran);
}

void testPostFromThreadsWakesLoop() {
    auto                     app = headless();
    std::atomic<int>         ran{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&] {
            for (int i = 0; i < 100; ++i)
                app->post([&] { ++ran; });
        });
    for (auto &t : threads)
        t.join();
    // A blocking pump(-1) must return once posted work exists.
    app->pump(-1);
    CHECK(ran == 400);
}

void testRunQuitFromTimer() {
    auto app   = headless();
    int  ticks = 0;
    app->addTimer(1, true, [&] {
        if (++ticks == 5)
            app->quit();
    });
    app->run();
    CHECK(ticks == 5);
}

void testPostedWorkDoesNotStarve() {
    // A closure that re-posts itself runs once per iteration, not forever.
    auto                  app   = headless();
    int                   count = 0;
    std::function<void()> again = [&] {
        ++count;
        app->post(again);
    };
    app->post(again);
    app->pump(0);
    CHECK(count == 1);
    app->pump(0);
    CHECK(count == 2);
}

void testFrameContract() {
    auto app    = headless();
    int  frames = 0;
    app->setEventHandler([&](const Event &e) {
        if (e.type == EventType::Frame) {
            ++frames;
            Canvas c = e.window->beginPaint();
            for (int y = 0; y < c.height; ++y)
                for (int x = 0; x < c.width; ++x)
                    c.pixels[size_t(y) * c.stride + x] = x < c.width / 2 ? 0xffff0000 : 0xff0000ff;
            e.window->endPaint({});
        }
    });
    auto win = app->createWindow({.size = {100, 50}});
    CHECK(pumpUntil(*app, [&] { return frames == 1; }));
    // No request → no frame.
    app->pump(20);
    CHECK(frames == 1);
    // Several requests coalesce into one Frame per iteration.
    win->requestFrame();
    win->requestFrame();
    app->pump(0);
    CHECK(frames == 2);

    uint32_t px = 0;
    CHECK(app->testHooks()->readPixel(*win, 10, 10, &px));
    CHECK(px == 0xffff0000);
    CHECK(app->testHooks()->readPixel(*win, 90, 10, &px));
    CHECK(px == 0xff0000ff);
}

void testFractionalScaleCanvas() {
    auto   app = headless();
    Canvas last;
    app->setEventHandler([&](const Event &e) {
        if (e.type == EventType::Frame) {
            last = e.window->beginPaint();
            e.window->endPaint({});
        }
    });
    auto win = app->createWindow({.size = {101, 33}});
    ti::setHeadlessScale(*win, 1.25);
    CHECK(pumpUntil(*app, [&] { return last.width != 0; }));
    CHECK(last.scale == 1.25);
    CHECK(last.width == 126); // round(101 * 1.25)
    CHECK(last.height == 41); // round(33 * 1.25)
    CHECK(win->size().w == 101);
}

void testKeyTextAndShortcuts() {
    auto               app = headless();
    std::vector<Event> ev;
    app->setEventHandler([&](const Event &e) { ev.push_back(e); });
    auto win = app->createWindow({});
    win->setTextInput({.enabled = true});
    auto *h = app->testHooks();

    h->injectKey(*win, Key::A, true);
    h->injectKey(*win, Key::A, true); // auto-repeat
    h->injectKey(*win, Key::A, false);
    h->injectKey(*win, Key::ControlLeft, true);
    h->injectKey(*win, Key::C, true);
    h->injectKey(*win, Key::C, false);
    h->injectKey(*win, Key::ControlLeft, false);

    std::string text;
    int         downs = 0, repeats = 0;
    bool        ctrlC = false;
    for (auto &e : ev) {
        if (e.type == EventType::TextInput)
            text += e.text;
        if (e.type == EventType::KeyDown) {
            ++downs;
            repeats += e.repeat;
            if (e.key == Key::C && (e.mods & ModCtrl))
                ctrlC = true;
        }
    }
    CHECK(text == "aa"); // the chord types nothing
    CHECK(downs == 4);
    CHECK(repeats == 1);
    CHECK(ctrlC);
    CHECK(ev.front().type == EventType::FocusIn);
}

void testPointerClicksAndHitTest() {
    auto               app = headless();
    std::vector<Event> ev;
    app->setEventHandler([&](const Event &e) { ev.push_back(e); });
    auto win = app->createWindow({.decorations = Decorations::Custom});
    win->setHitTest([](Point p) { return p.y < 30 ? HitArea::Caption : HitArea::Client; });
    auto *h = app->testHooks();

    h->injectPointerMove(*win, {50, 100});
    h->injectButton(*win, Button::Left, true);
    h->injectButton(*win, Button::Left, false);
    h->injectButton(*win, Button::Left, true);
    h->injectButton(*win, Button::Left, false);

    std::vector<int> clicks;
    for (auto &e : ev)
        if (e.type == EventType::PointerDown)
            clicks.push_back(e.clicks);
    CHECK((clicks == std::vector<int>{1, 2}));

    // A press on the caption turns into an OS move and never reaches the app.
    ev.clear();
    h->injectPointerMove(*win, {50, 10});
    h->injectButton(*win, Button::Left, true);
    bool sawDown = false;
    for (auto &e : ev)
        sawDown |= e.type == EventType::PointerDown;
    CHECK(!sawDown);
    CHECK(ti::headlessLastHit(*win) == HitArea::Caption);
}

void testClipboardIsAsyncAndTyped() {
    auto app = headless();
    app->setClipboardText("héllo ✓");
    std::optional<std::string> got;
    bool                       called = false;
    app->requestClipboard("text/plain;charset=utf-8", [&](auto v) {
        called = true;
        got    = v;
    });
    CHECK(!called); // never re-entrant
    CHECK(pumpUntil(*app, [&] { return called; }));
    CHECK(got && *got == "héllo ✓");

    called = false;
    app->requestClipboard("image/png", [&](auto v) {
        called = true;
        got    = v;
    });
    CHECK(pumpUntil(*app, [&] { return called; }));
    CHECK(!got);
}

void testKeyNames() {
    CHECK(std::string(keyName(Key::A)) == "A");
    CHECK(std::string(keyName(Key::KpEqual)) == "KpEqual");
    CHECK(std::string(keyName(Key::Count)) == "Unknown");
}

} // namespace

int main() {
    runCase("timers fire in due order, cancel works", testTimersOrderAndCancel);
    runCase(
        "repeating timer re-arms and can cancel itself", testRepeatingTimerRearmsAndSelfCancels
    );
    runCase("wait timeout clamps to next timer", testClampTimeout);
    runCase("shutdown drains closures that post as they go", testShutdownDrainsClosuresThatPost);
    runCase("post from 4 threads wakes a blocking pump", testPostFromThreadsWakesLoop);
    runCase("run() exits on quit() from a timer", testRunQuitFromTimer);
    runCase("self-reposting closure cannot starve the loop", testPostedWorkDoesNotStarve);
    runCase("frame requests coalesce; painted pixels read back", testFrameContract);
    runCase("fractional scale sizes the canvas in physical px", testFractionalScaleCanvas);
    runCase("keys: text, repeat, Ctrl chords type nothing", testKeyTextAndShortcuts);
    runCase("pointer: double-click count, caption press swallowed", testPointerClicksAndHitTest);
    runCase("clipboard: async, typed, never re-entrant", testClipboardIsAsyncAndTyped);
    runCase("key names", testKeyNames);
    return plat_test::summary();
}
