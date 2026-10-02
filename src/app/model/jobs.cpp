#include "app/model/jobs.h"

#include "plat/plat.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace model {

namespace {

// Workers outlive nothing they post to: once stopBackground() ran, a worker
// that finishes drops its result (the app is going away; the loop may be gone).
std::mutex              gMutex;
std::condition_variable gIdle; // gRunning dropped to 0
bool                    gStopped = false;
int                     gRunning = 0; // workers under way (waitBackground)

} // namespace

void runInBackground(plat::App &app, std::function<void()> work, std::function<void()> then) {
    {
        std::lock_guard<std::mutex> lock(gMutex);
        ++gRunning;
    }
    std::thread([&app, work = std::move(work), then = std::move(then)]() mutable {
        if (work)
            work();
        work = nullptr; // what it held goes before waitBackground returns
        std::lock_guard<std::mutex> lock(gMutex);
        if (!gStopped)
            app.post([then = std::move(then)] {
                if (then)
                    then();
            });
        if (--gRunning == 0)
            gIdle.notify_all();
    }).detach();
}

void stopBackground() {
    std::lock_guard<std::mutex> lock(gMutex);
    gStopped = true;
}

void waitBackground() {
    std::unique_lock<std::mutex> lock(gMutex);
    gIdle.wait(lock, [] { return gRunning == 0; });
}

int Jobs::begin(std::string description) {
    const int id = _nextId++;
    _active.push_back({id, std::move(description)});
    changed();
    return id;
}

void Jobs::end(int id) {
    for (size_t i = 0; i < _active.size(); ++i)
        if (_active[i].id == id) {
            _active.erase(_active.begin() + ptrdiff_t(i));
            changed();
            return;
        }
}

std::vector<std::string> Jobs::descriptions() const {
    std::vector<std::string> out;
    for (const Job &j : _active)
        if (!j.description.empty())
            out.push_back(j.description);
    return out;
}

Jobs::ObserverId Jobs::observe(std::function<void()> fn) {
    const ObserverId id = _nextObserver++;
    _observers.push_back({id, std::move(fn)});
    return id;
}

void Jobs::unobserve(ObserverId id) {
    for (Slot &s : _observers)
        if (s.id == id)
            s.fn = nullptr; // compacted by changed(): safe inside a callback
}

void Jobs::changed() {
    ++_dispatching;
    for (size_t i = 0; i < _observers.size(); ++i) // observe() inside a callback appends
        if (std::function<void()> fn = _observers[i].fn)
            fn();
    if (--_dispatching == 0)
        std::erase_if(_observers, [](const Slot &s) { return !s.fn; });
}

Jobs &jobs() {
    static Jobs j;
    return j;
}

} // namespace model
