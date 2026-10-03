#include "base/thread.h"

#include <chrono>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace base {

namespace {
#ifdef _WIN32
DWORD WINAPI trampoline(void *p) {
#else
void *trampoline(void *p) {
#endif
    auto *fn = static_cast<std::function<void()> *>(p);
    (*fn)();
    delete fn;
    return 0;
}
} // namespace

Thread::~Thread() {
    join();
}

bool Thread::start(std::function<void()> fn, size_t stackBytes) {
    if (_handle)
        return false;
    auto *heap = new std::function<void()>(std::move(fn));
#ifdef _WIN32
    HANDLE h = CreateThread(
        nullptr, stackBytes, trampoline, heap, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr
    );
    if (!h) {
        delete heap;
        return false;
    }
    _handle = h;
#else
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stackBytes);
    auto     *t  = new pthread_t;
    const int rc = pthread_create(t, &attr, trampoline, heap);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        delete t;
        delete heap;
        return false;
    }
    _handle = t;
#endif
    return true;
}

void Thread::join() {
    if (!_handle)
        return;
#ifdef _WIN32
    WaitForSingleObject(static_cast<HANDLE>(_handle), INFINITE);
    CloseHandle(static_cast<HANDLE>(_handle));
#else
    auto *t = static_cast<pthread_t *>(_handle);
    pthread_join(*t, nullptr);
    delete t;
#endif
    _handle = nullptr;
}

void Thread::detach() {
    if (!_handle)
        return;
#ifdef _WIN32
    CloseHandle(static_cast<HANDLE>(_handle));
#else
    auto *t = static_cast<pthread_t *>(_handle);
    pthread_detach(*t);
    delete t;
#endif
    _handle = nullptr;
}

struct WorkerPool::Worker {
    Thread thread;
};

WorkerPool::WorkerPool(const Options &o) : _opt(o), _idleMs(o.idleMs) {}

WorkerPool::~WorkerPool() {
    stop();
}

void WorkerPool::loop(Worker *self) {
    using Clock = std::chrono::steady_clock;
    std::unique_lock<std::mutex> lock(_mutex);
    for (;;) {
        if (_queue.empty() && !_stopping && _parked < _opt.maxParked) {
            ++_parked;
            const Clock::time_point since = Clock::now();
            for (;;) { // the idle time may change while it waits
                const auto until = since + std::chrono::milliseconds(_idleMs);
                if (!_queue.empty() || _stopping || Clock::now() >= until)
                    break;
                _wake.wait_until(lock, until);
            }
            --_parked;
        }
        if (_stopping)
            return; // stop() joins it
        if (_queue.empty()) {
            // Idle long enough, or enough are parked: it ends. Detached, it
            // touches nothing of the pool once the lock is released.
            for (size_t i = 0; i < _workers.size(); ++i)
                if (_workers[i].get() == self) {
                    self->thread.detach();
                    _workers.erase(_workers.begin() + ptrdiff_t(i));
                    break;
                }
            return;
        }
        std::function<void()> task = std::move(_queue.front());
        _queue.pop_front();
        lock.unlock();
        task();
        task = nullptr; // what it held goes before the next one, outside the lock
        lock.lock();
    }
}

void WorkerPool::post(std::function<void()> task, bool front) {
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_stopping)
            return; // `task` goes with the call, outside the lock
        if (front)
            _queue.push_front(std::move(task));
        else
            _queue.push_back(std::move(task));
        // More queued than the parked workers can take: another worker, up
        // to the cap (then the task waits for one to finish).
        if (int(_queue.size()) > _parked && int(_workers.size()) < _opt.maxWorkers) {
            _workers.push_back(std::make_unique<Worker>());
            Worker *w = _workers.back().get();
            if (w->thread.start([this, w] { loop(w); }, _opt.stackBytes))
                ++_started;
            else
                _workers.pop_back();
        }
    }
    _wake.notify_one();
}

void WorkerPool::stop() {
    std::deque<std::function<void()>>    dropped;
    std::vector<std::unique_ptr<Worker>> workers;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _stopping = true;
        dropped.swap(_queue);
        workers.swap(_workers);
    }
    _wake.notify_all();
    dropped.clear();
    workers.clear(); // joins each, after its task under way
}

void WorkerPool::setIdleMs(int ms) {
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _idleMs = ms;
    }
    _wake.notify_all();
}

WorkerPool::Stats WorkerPool::stats() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return {int(_workers.size()), _parked, _started};
}

} // namespace base
