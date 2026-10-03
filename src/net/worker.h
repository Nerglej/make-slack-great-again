// A joinable thread with an explicit stack size (internal). std::thread
// cannot set one, and musl's default 128 KiB is too tight for a TLS
// handshake plus certificate parsing.
#pragma once

#include <cstddef>
#include <functional>

namespace net::detail {

class Thread {
public:
    Thread() = default;
    ~Thread(); // joins
    Thread(const Thread &)            = delete;
    Thread &operator=(const Thread &) = delete;

    bool start(std::function<void()> fn, size_t stackBytes = 256 * 1024);
    void join();

private:
    void *_handle = nullptr; // pthread_t* / HANDLE
};

} // namespace net::detail
