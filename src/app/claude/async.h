// Blocking work (a CLI run, a directory scan, a git command) off the UI
// thread: each call runs on a short-lived worker thread and hands its result
// to the loop with plat::App::post. Callbacks run on the UI thread, never
// re-entrantly from inside the call.
//
// Nothing is cancelled: a caller that may be gone by then guards its callback
// (a shared alive flag, as SlackBackend does).
#pragma once

#include "base/process.h"

#include <functional>
#include <string>
#include <vector>

namespace plat {
class App;
}

namespace claude {

using RunDone = std::function<void(base::RunResult)>;
// base::run(exe, args, opts) on a worker thread; done(result) on the loop.
void runAsync(
    plat::App               &app,
    std::string              exe,
    std::vector<std::string> args,
    base::RunOptions         opts,
    RunDone                  done
);

// work() on a worker thread, then then() on the loop.
void offThread(plat::App &app, std::function<void()> work, std::function<void()> then);

// The app is shutting down: results still to come are dropped, never posted
// to a loop that may be gone. Call before the plat::App is destroyed.
void stopAsync();

} // namespace claude
