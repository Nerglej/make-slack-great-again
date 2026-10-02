// msga: the app. Opens the signed-in workspace (screens/shell/accounts.h),
// or the old app's "Log in to workspace" page when there is none.
//
//   msga                  the saved workspace, or the logged-out page
//   msga msga://…         also hands an OAuth callback URL to the sign-in
//   --demo <dir>          the fake workspace in <dir>/fixture.json (only in
//                         builds configured with -DMSGA_DEMO=ON)
//   --demo-tour <file>    with --demo: msga's scripted walkthrough (demo/tour.json)
//
// Diagnostics (screenshots, measurements):
//   --theme light|dark   override the saved theme for this run
//   --open <conv id>     open this conversation instead of the fixture's start
//   --thread <text>      then open the thread whose root contains <text>
//   --browse-all         visit every conversation once, then print RSS
//   --exit-after <ms>    quit after that long
#include "app/auth/workspaces.h"
#include "app/cache/workspace_cache.h"
#include "app/crash/crash_handler.h"
#include "app/i18n/languages.h"
#include "app/identity.h"
#include "app/llm/service.h"
#include "app/media/audio_player.h"
#include "app/model/backend_proxy.h"
#include "app/model/null_backend.h"
#include "app/update/updater.h"
#include "base/file.h"
#include "base/i18n.h"
#include "base/time.h"
#include "base/log.h"
#include "base/str.h"
#include "gfx/icons_generated.h"
#include "net/net.h"
#include "screens/common/context.h"
#include "screens/common/remote_images.h"
#include "screens/shell/accounts.h"
#include "screens/shell/avatars.h"
#include "screens/shell/context_menus.h"
#include "screens/shell/desktop_entry.h"
#include "screens/shell/settings.h"
#include "screens/shell/shell.h"
#ifdef MSGA_LEGACY_IMPORT
#include "app/legacy/legacy.h"
#endif

#ifdef MSGA_DEMO
#include "app/fake/fake_backend.h"
#include "screens/shell/demo/tour.h"
#endif
#ifdef MSGA_HAVE_MESSAGES
#include "screens/messages/debug_scroll.h"
#include "screens/messages/image_cache.h"
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#if defined(MSGA_DEMO) && defined(__linux__)
#include <ftw.h>
#include <unistd.h>
#endif

namespace {

[[maybe_unused]] long rssKb() {
    long kb = -1;
    if (FILE *f = std::fopen("/proc/self/status", "r")) {
        char line[256];
        while (std::fgets(line, sizeof line, f))
            if (std::strncmp(line, "VmRSS:", 6) == 0)
                kb = std::atol(line + 6);
        std::fclose(f);
    }
    return kb;
}

#ifdef MSGA_DEMO
// msga's demo::isolateState: a demo run keeps nothing of the user's — its
// HOME and XDG dirs point at a wiped <tmp>/msga-demo-state, so the settings,
// workspaces, caches and the old app's credential store it reads and writes
// are throwaway ones. Before the platform starts: it reads them on first use.
bool isolateDemoState(std::string *error) {
#if !defined(__linux__)
    *error = "demo mode redirects HOME/XDG_* and is Linux-only for now";
    return false;
#else
    const char       *tmp   = std::getenv("TMPDIR");
    const std::string state = file::join(tmp && *tmp ? tmp : "/tmp", "msga-demo-state");
    if (file::exists(state) &&
        nftw(
            state.c_str(),
            [](const char *path, const struct stat *, int, struct FTW *) { return ::remove(path); },
            16,
            FTW_DEPTH | FTW_PHYS
        ) != 0) {
        *error = "cannot wipe " + state;
        return false;
    }
    for (const char *sub : {"config", "data", "cache", "state"})
        if (!file::makeDirs(file::join(state, sub))) {
            *error = "cannot create " + file::join(state, sub);
            return false;
        }
    setenv("HOME", state.c_str(), 1);
    setenv("XDG_CONFIG_HOME", file::join(state, "config").c_str(), 1);
    setenv("XDG_DATA_HOME", file::join(state, "data").c_str(), 1);
    setenv("XDG_CACHE_HOME", file::join(state, "cache").c_str(), 1);
    setenv("XDG_STATE_HOME", file::join(state, "state").c_str(), 1);
    return true;
#endif
}
#endif

} // namespace

int main(int argc, char **argv) {
#ifdef MSGA_DEMO
    std::string demo;
#else
    constexpr std::string_view demo; // no demo mode in this build: never set
#endif
    std::string                  theme;
    std::vector<std::string>     urls; // msga:// from the OS (an OAuth callback)
    int                          exitAfter = 0;
    int                          exitCode  = 0;
    // These diagnostics act on a workspace, which only the demo provides so
    // far. debugScroll (hidden): scroll torture test with frame verification.
    [[maybe_unused]] std::string openId, threadText;
    [[maybe_unused]] int         debugScroll = 0;
    [[maybe_unused]] bool        browseAll   = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a    = argv[i];
        auto              next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--theme")
            theme = next();
#ifdef MSGA_DEMO
        else if (a == "--demo")
            demo = next();
        else if (str::startsWith(a, "--demo="))
            demo = a.substr(7);
#endif
        else if (a == "--open")
            openId = next();
        else if (a == "--thread")
            threadText = next();
        else if (a == "--exit-after")
            exitAfter = std::atoi(next().c_str());
        else if (a == "--browse-all")
            browseAll = true;
        else if (a == "--debug-scroll")
            debugScroll = std::atoi(next().c_str());
        else if (str::startsWith(a, "msga://"))
            urls.push_back(a);
        // Anything else is ignored, like the old app did.
    }

    std::string err;
#ifdef MSGA_DEMO
    if (!demo.empty() && !isolateDemoState(&err)) {
        std::fprintf(stderr, "msga --demo: %s\n", err.c_str());
        return 2;
    }
#endif
    auto app = ui::App::create(&err);
    if (!app) {
        std::fprintf(stderr, "msga: %s\n", err.c_str());
        return 1;
    }
    plat::App  &pa    = app->platform();
    // The old Qt app's ids (app/identity.h), which its msga.desktop, Start-menu
    // shortcut and bundle carry: shells find the window's, the notifications'
    // and the launcher badge's entry by them (Linux: app_id "msga", WM_CLASS
    // "msga", "MSGA"; elsewhere com.nisdos.msga).
    const char *appId = identity::appId();
    // Windows: the old app's "MSGA" Start-menu entry (with the AUMID), always
    // (not for a demo run).
    pa.setAppInfo({identity::kName, appId, {}, demo.empty()});
    // The old app still running takes this launch, as its own second
    // launches did; then a second launch of ours hands its arguments to the
    // running one (which raises its window on InstanceActivated) and exits.
    if (demo.empty() && identity::handOffToOldApp(urls.empty() ? std::string() : urls.front()))
        return 0;
    std::vector<std::string> args(argv + 1, argv + argc);
    // A demo run has its own channel (msga's followed the redirected HOME):
    // it neither hands off to the user's msga nor takes its launches.
    const std::string instance = demo.empty() ? std::string(identity::instanceKey())
                                              : str::concat({identity::instanceKey(), "-demo"});
    if (!pa.claimSingleInstance(instance, args))
        return 0;
    // A crash now prints a stack trace (stderr + crash.log, the old app's
    // file) instead of a bare "Segmentation fault", then still core-dumps.
    crash::install(identity::crashLogPath(pa));
    // Dev builds only: the main-thread hang watchdog (crash_handler.h), its
    // heartbeat started below. AddressSanitizer makes everything ~5-10x
    // slower, so ASan builds get a roomier window and only genuine hangs fire.
#if defined(__SANITIZE_ADDRESS__)
    const bool watchdog = crash::startWatchdog(20000);
#else
    const bool watchdog = crash::startWatchdog(5000);
#endif

    const std::string settingsPath = shell::Settings::defaultPath(pa);
#ifdef MSGA_LEGACY_IMPORT
    // The upgrade from the old app: its settings, workspaces and caches.
    if (demo.empty())
        legacy::importOldApp(pa, settingsPath, auth::WorkspaceStore::defaultPath(pa));
#endif
    shell::Settings settings = shell::Settings::load(settingsPath);
    // Before any UI text exists: strings are translated when a view is built,
    // so a changed language applies at the next start (as in the old app).
    app_i18n::registerLanguages();
    if (settings.language == "system" || !i18n::setLanguage(settings.language))
        i18n::setPreferredLanguage(pa.preferredLanguages());
    // Dates follow the setting at once (Shell::applySettings re-applies it);
    // Japanese writes the 24-hour clock by default.
    base::setDateLanguage(settings.language);
    if (!settings.use24hSaved)
        settings.use24h = std::strcmp(base::dateLanguage(), "ja") == 0;
    app->setThemeMode(
        theme == "dark"    ? ui::ThemeMode::Dark
        : theme == "light" ? ui::ThemeMode::Light
                           : settings.theme
    );
    app->setUserTextScale(settings.fontScale());
    settings.applyPalettes();

    // The demo workspace, or the signed-in one (Accounts swaps it in behind
    // the proxy the screens hold).
    model::Store          store;
    model::NullBackend    noWorkspace(store);
    model::BackendProxy   backend(store, noWorkspace);
    net::Client           client(pa);
    // Pictures and the update's download on a worker pool of their own, so
    // a screenful of avatars never holds up the workspace's API calls.
    net::Client           transfers(pa);
    // msga's UpdateChecker (msga.app's manifest; the shell drives it).
    update::Updater       updater(pa, transfers, MSGA_VERSION);
    // Avatars, files, emoji and previews from URLs, cached on disk.
    screens::RemoteImages remote(pa, &transfers, screens::RemoteImages::defaultDir(pa));
    // The rest of what the old CacheEvictor bounded: the inline player's
    // audio and the opened HTML files are evicted with the pictures; the
    // workspaces' data and icons only count toward the limit.
    if (const std::string cache = identity::cacheDir(pa); !cache.empty())
        remote.coverDirs(
            {file::join(cache, "audio"), file::join(cache, "files")},
            {cache::WorkspaceCache::root(pa), file::join(cache, "workspace-icons")}
        );
#ifdef MSGA_DEMO
    std::optional<fake::FakeBackend> demoBackend;
    if (!demo.empty()) {
        demoBackend.emplace(store, pa);
        demoBackend->setFixture(demo, 0);
        backend.setTarget(*demoBackend);
    }
    const bool demoMode = bool(demoBackend);
#else
    const bool demoMode = false;
#endif
#ifdef MSGA_HAVE_MESSAGES
    screens::ImageCache  images(pa);
    screens::ImageCache &imageRef = images;
    images.setRemote(&remote);
#else
    // The messages screens (and their ImageCache) are not linked yet and
    // nothing dereferences this until they are.
    alignas(16) static char noCache[16];
    screens::ImageCache    &imageRef = *reinterpret_cast<screens::ImageCache *>(noCache);
#endif
    screens::Context ctx{*app, store, backend, imageRef, {}, {}, {}, {}, {}};
    ctx.remote = &remote;
    // The AI providers (Settings → AI assistance; the shell keeps them in step).
    llm::Service ai(pa);
    ctx.ai = &ai;
    // The inline audio player: one clip at a time, across conversations.
    media::AudioPlayer audio(pa);
    ctx.audio = &audio;
#ifdef MSGA_DEMO
    if (demoBackend) // the fixture's canned answers stand in for an AI server
        ai.setStandIn(
            llm::fromSettings("demo", "Lumen AI", "demo:", "", "lumen-1", ""),
            [&fb = *demoBackend](std::string_view request) {
                return std::string(fb.aiReply(request));
            }
        );
#endif

    plat::WindowDesc desc;
    desc.title   = "MSGA";
    desc.appId   = appId;
    desc.wmClass = "MSGA"; // Qt's res_class: the application name
#ifdef __linux__
    // The logo for the X11 _NET_WM_ICON, at the sizes the old app sent.
    for (int n : {16, 20, 24, 32, 48, 64, 128, 256}) {
        gfx::Bitmap  b(n, n);
        gfx::Painter p(b.view(), 1.f);
        gfx::drawIcon(p, gfx::Icon::Logo, {0, 0, float(n), float(n)}, 0xffffffffU);
        desc.icon.push_back(shell::toPlatImage(b));
    }
#endif
    desc.size        = {settings.width, settings.height};
    desc.minSize     = shell::Shell::kMinWindowSize; // fitToScreen lowers it on small screens
    desc.decorations = plat::Decorations::Custom;
    if (settings.hasPosition)
        desc.position = plat::Point{double(settings.x), double(settings.y)};
    ui::Window win(desc);
    if (settings.maximized)
        win.native().setMaximized(true);

    shell::Shell sh(ctx, win, settings, settingsPath);
    // A restart (an applied update, new Slack app keys) runs with the same
    // arguments, minus a one-off OAuth callback URL.
    for (const std::string &a : args)
        if (!str::startsWith(a, "msga://"))
            sh.restartArgs.push_back(a);
    if (!demoMode)
        sh.setUpdater(&updater);
#ifdef MSGA_DEMO
    const std::string           tourPath = demo::tourPathFromArgs(argc, argv);
    std::unique_ptr<demo::Tour> tour;
#endif
    win.onCloseRequested = [&] {
        if (!sh.hideToTray())
            sh.quit();
    };
    sh.onQuit = [&] { app->quit(); };

    std::optional<shell::Accounts> accounts;
    app->onEvent = [&](const plat::Event &e) {
        // msga:// URLs (the OAuth callback): opened by the OS, or handed over
        // by a second launch.
        if (accounts &&
            (e.type == plat::EventType::OpenUrls || e.type == plat::EventType::InstanceActivated))
            for (const std::string &s : e.strings)
                if (str::startsWith(s, "msga://"))
                    accounts->handleUrl(s);
        if (accounts && e.type == plat::EventType::NetworkChanged)
            accounts->networkChanged(e.online);
        sh.handleAppEvent(e);
    };

    if (!demoMode) {
        // The OAuth redirect (msga://oauth/callback) comes back to us; on
        // Linux through the launcher entry the old app installed.
        shell::installDesktopEntry(pa);
        accounts.emplace(
            ctx,
            sh,
            win,
            settings,
            [&] { settings.save(settingsPath); },
            backend,
            noWorkspace,
            client,
            auth::WorkspaceStore::defaultPath(pa)
        );
        shell::Accounts &acc           = *accounts;
        sh.onAddWorkspace              = [&acc](ui::PointF at) { acc.promptAdd(at); };
        sh.menus().hooks.signOut       = [&acc](const std::string &key) { acc.signOut(key); };
        sh.menus().hooks.muteWorkspace = [&acc](const std::string &key, bool on) {
            acc.setMuted(key, on);
        };
        sh.onSwitchWorkspace   = [&acc](const std::string &key) { acc.switchTo(key); };
        sh.onReorderWorkspaces = [&acc](const std::vector<std::string> &keys) {
            acc.reorder(keys);
        };
        sh.onImportSlackSession = [&acc] { acc.importSession(); };
        sh.onConvertToSession   = [&acc] { acc.convertToSession(); };
        sh.oauthSlackWorkspaces = [&acc] { return acc.oauthSlackWorkspaces(); };
        acc.start();
        for (const std::string &u : urls)
            acc.handleUrl(u);
    }
#ifdef MSGA_DEMO
    else {
        fake::FakeBackend &fb = *demoBackend;
        const double       t0 = app->nowMs();
        sh.setSignedIn(true); // the workspace opening: msga's first-load state until connected
        fb.connect([&](bool ok, const std::string &why) {
            if (!ok) {
                std::fprintf(stderr, "msga: %s\n", why.c_str());
                app->quit();
                return;
            }
            model::ConvRef start = fb.fixture().startConversation;
            if (!openId.empty())
                if (model::ConvRef c = store.findConversation(openId); c != model::kNoConv)
                    start = c;
            sh.open(start);
            if (!threadText.empty())
                if (model::Ts root = fb.findTs(start, threadText))
                    sh.openThread(start, root);
            sh.setLive(true);
            std::fprintf(
                stderr, "msga: connected in %.0f ms, rss %ld KB\n", app->nowMs() - t0, rssKb()
            );
            if (!tourPath.empty()) {
                demo::TourScript script;
                std::string      err;
                if (!demo::loadTour(tourPath, &script, &err)) {
                    std::fprintf(stderr, "msga --demo-tour: %s\n", err.c_str());
                    exitCode = 2;
                    app->quit();
                    return;
                }
                tour = std::make_unique<demo::Tour>(ctx, sh, win, fb, std::move(script));
                tour->start();
            }
#ifdef MSGA_HAVE_MESSAGES
            if (debugScroll > 0) {
                screens::DebugScrollHooks hooks{
                    [&](model::ConvRef c) { sh.open(c); },
                    [&](model::ConvRef c, model::Ts t) { sh.openThread(c, t); },
                    [&] { sh.closeThread(); },
                };
                app->addTimer(400, false, [&, hooks, debugScroll] {
                    screens::debugScrollTour(ctx, win, hooks, debugScroll, [&](int bad) {
                        exitCode = bad ? 3 : 0;
                        app->quit();
                    });
                });
            }
#endif
            if (browseAll) {
                // Every conversation in turn (a frame or two each), then back.
                auto step = std::make_shared<std::function<void(model::ConvRef)>>();
                *step     = [&, step, start](model::ConvRef c) {
                    if (c >= store.conversationCount()) {
                        sh.open(start);
                        app->addTimer(300, false, [] {
                            std::fprintf(
                                stderr,
                                "msga: after browsing all conversations, rss %ld KB\n",
                                rssKb()
                            );
                        });
                        return;
                    }
                    sh.open(c);
                    app->addTimer(120, false, [step, c] { (*step)(c + 1); });
                };
                app->addTimer(500, false, [step] { (*step)(0); });
            }
        });
    }
#endif
    if (exitAfter > 0)
        app->addTimer(exitAfter, false, [&] { sh.quit(); });
    // Pet the hang watchdog from the event loop: while the loop pumps, this
    // re-arms its deadline every second; if the loop wedges, it fires.
    if (watchdog)
        app->addTimer(1000, true, [] { crash::heartbeat(); });
    app->run();
    return exitCode;
}
