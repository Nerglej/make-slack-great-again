// poll(2)-based loop shared by the Wayland, X11 and headless backends.
// The display connection is just another watched fd; `beforeWait` lets a
// backend drain already-queued events and flush its requests before sleeping
// (wl_display_prepare_read / xcb_flush), `afterWait` lets it cancel/read.
#pragma once

#include "core/loop_core.h"

#include <map>

namespace plat::posix {

class PosixLoop {
public:
    PosixLoop();
    ~PosixLoop();
    PosixLoop(const PosixLoop &)            = delete;
    PosixLoop &operator=(const PosixLoop &) = delete;

    core::LoopCore core;

    // Return false to skip the blocking poll this iteration (work pending).
    std::function<bool()> beforeWait;
    std::function<void()> afterWait;

    uint64_t watch(int fd, uint32_t events, std::function<void(uint32_t)> fn);
    void     unwatch(uint64_t id);

    // One iteration: wait up to timeoutMs, then fds → posted → timers.
    void iterate(int timeoutMs);
    void run();
    void quit() {
        _quit = true;
        wakeUp();
    }
    bool quitting() const { return _quit; }
    void resetQuit() { _quit = false; }

    void wakeUp(); // thread-safe

private:
    struct Watch {
        int                           fd;
        uint32_t                      events;
        std::function<void(uint32_t)> fn;
    };
    std::map<uint64_t, Watch> _watches;
    uint64_t                  _nextWatch = 1;
    int                       _wakeRead = -1, _wakeWrite = -1;
    bool                      _quit = false;
};

} // namespace plat::posix
