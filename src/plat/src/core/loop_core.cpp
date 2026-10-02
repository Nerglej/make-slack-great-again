#include "core/loop_core.h"

#include <algorithm>

namespace plat::core {

TimerId LoopCore::addTimer(int intervalMs, bool repeat, std::function<void()> fn) {
    const auto    iv = std::chrono::milliseconds(std::max(0, intervalMs));
    const TimerId id = _nextTimer++;
    _timers.emplace(id, Timer{Clock::now() + iv, iv, repeat, std::move(fn)});
    return id;
}

void LoopCore::cancelTimer(TimerId id) {
    _timers.erase(id);
}

int LoopCore::msUntilNextTimer() const {
    if (_timers.empty())
        return -1;
    auto due = Clock::time_point::max();
    for (const auto &[id, t] : _timers)
        due = std::min(due, t.due);
    const auto now = Clock::now();
    if (due <= now)
        return 0;
    // Round up: waking a millisecond early just spins the loop once more.
    return int(std::chrono::ceil<std::chrono::milliseconds>(due - now).count());
}

int LoopCore::clampTimeout(int timeoutMs) const {
    const int t = msUntilNextTimer();
    if (t < 0)
        return timeoutMs;
    return timeoutMs < 0 ? t : std::min(t, timeoutMs);
}

void LoopCore::runDueTimers() {
    const auto                                         now = Clock::now();
    // Collect first: a callback may add or cancel timers (including itself).
    std::vector<std::pair<Clock::time_point, TimerId>> due;
    for (const auto &[id, t] : _timers)
        if (t.due <= now)
            due.emplace_back(t.due, id);
    std::sort(due.begin(), due.end());
    for (const auto &[when, id] : due) {
        auto it = _timers.find(id);
        if (it == _timers.end())
            continue; // cancelled by an earlier callback this round
        auto fn = it->second.fn;
        if (it->second.repeat) {
            // Re-arm from the scheduled time, not from now, so a repeating
            // timer does not drift; skip missed ticks rather than bursting.
            auto &t = it->second;
            do
                t.due += std::max(t.interval, std::chrono::milliseconds(1));
            while (t.due <= now);
        } else {
            _timers.erase(it);
        }
        fn();
    }
}

void LoopCore::post(std::function<void()> fn) {
    // `fn`, when dropped, is destroyed after the lock is released. Waking
    // under the lock means no wake() can still be running once shutdown()
    // returns and the backend tears down what it signals.
    std::lock_guard lock(_postMutex);
    if (_closed)
        return;
    _posted.push_back(std::move(fn));
    if (wake)
        wake();
}

bool LoopCore::hasPosted() {
    std::lock_guard lock(_postMutex);
    return !_posted.empty();
}

void LoopCore::runPosted() {
    std::vector<std::function<void()>> batch;
    {
        std::lock_guard lock(_postMutex);
        batch.swap(_posted);
    }
    // Work posted by these callbacks runs next iteration, so a closure that
    // re-posts itself cannot starve input.
    for (auto &fn : batch)
        fn();
}

void LoopCore::shutdown() {
    // In rounds, each destroyed outside the lock and outside the members: a
    // closure holding a retired net::WebSocket joins its reader thread, which
    // posts its close event while we are still destroying the batch.
    for (;;) {
        std::map<TimerId, Timer>           timers;
        std::vector<std::function<void()>> posted;
        timers.swap(_timers);
        std::lock_guard lock(_postMutex);
        posted.swap(_posted);
        if (timers.empty() && posted.empty()) {
            _closed = true;
            return;
        }
    }
}

} // namespace plat::core
