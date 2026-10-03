#include "app/model/jobs.h"

#include "plat/plat.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace model {

namespace {

using Clock = std::chrono::steady_clock;

// A small pool of parked workers: a call wakes one, else starts a new
// thread. Nothing ever waits behind another task: a CLI run may block its
// worker up to its timeout, and work may wait on other work. A finished
// worker parks while fewer than kMaxParked do, else retires; a parked one
// retires after the idle time, so a quiet app holds no threads.
//
// Workers outlive nothing they post to: once stopBackground() ran, a worker
// that finishes drops its result (the app is going away; the loop may be gone).
constexpr int kMaxParked = 4;

struct Task {
    plat::App            *app;
    std::function<void()> work, then;
};

struct Pool {
    std::mutex                mutex;
    std::condition_variable   wake;  // a task was queued, or the idle time changed
    std::condition_variable   idle;  // running dropped to 0
    std::vector<Task>         queue; // handed to parked workers, oldest first
    bool                      stopped = false;
    int                       running = 0; // tasks queued or under way (waitBackground)
    int                       workers = 0; // threads alive
    int                       parked  = 0; // of them, waiting for a task
    uint64_t                  started = 0; // threads ever started
    std::chrono::milliseconds idleRetire{30'000};
};

// Never destroyed: a parked worker may still be waiting on it as the
// process exits.
Pool &pool() {
    static Pool *p = new Pool;
    return *p;
}

void workerLoop() {
    Pool                        &p = pool();
    std::unique_lock<std::mutex> lock(p.mutex);
    for (;;) {
        if (p.queue.empty()) {
            if (p.parked >= kMaxParked) { // enough are waiting already
                --p.workers;
                return;
            }
            ++p.parked;
            const Clock::time_point since = Clock::now();
            while (p.queue.empty() && Clock::now() < since + p.idleRetire)
                p.wake.wait_until(lock, since + p.idleRetire);
            --p.parked;
            if (p.queue.empty()) { // idle long enough: retire
                --p.workers;
                return;
            }
        }
        Task t = std::move(p.queue.front());
        p.queue.erase(p.queue.begin());
        lock.unlock();
        if (t.work)
            t.work();
        t.work = nullptr; // what it held goes before waitBackground returns
        lock.lock();
        if (!p.stopped)
            t.app->post([then = std::move(t.then)] {
                if (then)
                    then();
            });
        if (--p.running == 0)
            p.idle.notify_all();
        if (t.then) { // dropped: released outside the lock
            lock.unlock();
            t.then = nullptr;
            lock.lock();
        }
    }
}

} // namespace

void runInBackground(plat::App &app, std::function<void()> work, std::function<void()> then) {
    Pool                       &p = pool();
    std::lock_guard<std::mutex> lock(p.mutex);
    ++p.running;
    p.queue.push_back({&app, std::move(work), std::move(then)});
    // More queued than the parked workers can take: another worker.
    if (int(p.queue.size()) > p.parked) {
        ++p.workers;
        ++p.started;
        std::thread(workerLoop).detach();
    }
    if (p.parked > 0)
        p.wake.notify_one();
}

void stopBackground() {
    Pool                       &p = pool();
    std::lock_guard<std::mutex> lock(p.mutex);
    p.stopped = true;
}

void waitBackground() {
    Pool                        &p = pool();
    std::unique_lock<std::mutex> lock(p.mutex);
    p.idle.wait(lock, [&p] { return p.running == 0; });
}

BackgroundStats backgroundStats() {
    Pool                       &p = pool();
    std::lock_guard<std::mutex> lock(p.mutex);
    return {p.workers, p.parked, p.started};
}

void setBackgroundIdleRetire(int ms) {
    Pool                       &p = pool();
    std::lock_guard<std::mutex> lock(p.mutex);
    p.idleRetire = std::chrono::milliseconds(ms);
    p.wake.notify_all(); // parked workers measure against the new time
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
