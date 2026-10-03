// Threads: one with a stack size of our own, and a pool of parked workers.
//
// Why a stack size of our own: std::thread takes the platform's default,
// and those differ wildly — glibc reserves RLIMIT_STACK (usually 8 MiB),
// Windows the executable's reserve, a dynamically linked musl only 128 KiB
// (too tight for a TLS handshake plus certificate parsing, or a raster fill:
// issue #70). The static musl release links with -z stack-size (8 MiB), so
// std::thread is safe there. An explicit size makes every build behave the
// same and bounds the address space each worker reserves; the pages a thread
// touches are what it costs either way.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace base {

// Workers' stack unless a caller knows better: room for the recursive
// parsers (JSON nests 256 deep) and the decoders, 8x musl's default.
constexpr size_t kThreadStack = 1024 * 1024;

// A joinable thread.
class Thread {
public:
    Thread() = default;
    ~Thread(); // joins
    Thread(const Thread &)            = delete;
    Thread &operator=(const Thread &) = delete;

    bool start(std::function<void()> fn, size_t stackBytes = kThreadStack);
    void join();
    // It runs on unjoined and ends on its own; this object is free again.
    void detach();

private:
    void *_handle = nullptr; // pthread_t* / HANDLE
};

// Up to maxWorkers threads take posted tasks, oldest first (a front post
// jumps the queue). A task wakes a parked worker, else starts a thread while
// fewer than maxWorkers run; past that it waits for the next free one, so a
// task must never wait on another posted to the same pool. A finished worker
// parks while fewer than maxParked do, else ends; a parked one ends after
// idleMs, so a quiet pool holds no threads.
//
// maxWorkers 1 makes a serial worker: tasks run one at a time, in order.
//
// Destroying the pool (or stop()) drops the queued tasks and joins the
// workers, waiting for the tasks under way. Thread-safe.
class WorkerPool {
public:
    struct Options {
        int    maxWorkers = 4;
        int    maxParked  = 4;
        int    idleMs     = 30'000;
        size_t stackBytes = kThreadStack;
    };
    explicit WorkerPool(const Options &o);
    ~WorkerPool();
    WorkerPool(const WorkerPool &)            = delete;
    WorkerPool &operator=(const WorkerPool &) = delete;

    // Dropped once stopped. A task is destroyed on its worker, outside any
    // lock of the pool.
    void post(std::function<void()> task, bool front = false);
    void stop();
    // Parked workers measure against the new time (tests shorten it).
    void setIdleMs(int ms);

    // Threads alive, of them parked, threads ever started.
    struct Stats {
        int      workers = 0, parked = 0;
        uint64_t started = 0;
    };
    Stats stats() const;

private:
    struct Worker;
    void loop(Worker *self);

    const Options                        _opt;
    mutable std::mutex                   _mutex;
    std::condition_variable              _wake; // a task was posted, stop, or the idle time changed
    std::deque<std::function<void()>>    _queue;
    std::vector<std::unique_ptr<Worker>> _workers; // alive; a retiring one detaches itself
    int                                  _idleMs;
    int                                  _parked   = 0;
    uint64_t                             _started  = 0;
    bool                                 _stopping = false;
};

} // namespace base
