#include "net/worker.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace net::detail {

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

} // namespace net::detail
