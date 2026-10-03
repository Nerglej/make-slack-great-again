#include "app/diag/mem_stats.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/lsan_interface.h>
// In every ASan runtime, but GCC ships no sanitizer/allocator_interface.h.
extern "C" size_t __sanitizer_get_current_allocated_bytes();
#elif defined(__linux__) && defined(__GLIBC__)
#include <malloc.h>
#elif defined(__APPLE__)
#include <malloc/malloc.h>
#endif
#if defined(__linux__)
#include <dirent.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace diag {

MemStats sampleMem() {
    MemStats m;
#if defined(__SANITIZE_ADDRESS__)
    // ASan replaces malloc: mallinfo2 would describe its own arenas.
    m.heapKb = long(__sanitizer_get_current_allocated_bytes() / 1024);
#elif defined(__linux__) && defined(__GLIBC__)
    const struct mallinfo2 mi = mallinfo2();
    m.heapKb                  = long((mi.uordblks + mi.hblkhd) / 1024);
#elif defined(__APPLE__)
    malloc_statistics_t st{};
    malloc_zone_statistics(nullptr, &st); // null: every zone
    m.heapKb = long(st.size_in_use / 1024);
#endif
#if defined(__linux__)
    if (FILE *f = std::fopen("/proc/self/status", "r")) {
        char line[256];
        while (std::fgets(line, sizeof line, f)) {
            if (std::strncmp(line, "VmRSS:", 6) == 0)
                m.rssKb = std::atol(line + 6);
            else if (std::strncmp(line, "Threads:", 8) == 0)
                m.threads = std::atol(line + 8);
        }
        std::fclose(f);
    }
    if (DIR *d = opendir("/proc/self/fd")) {
        long n = 0;
        while (const dirent *e = readdir(d))
            if (e->d_name[0] != '.')
                ++n;
        closedir(d);
        m.fds = n - 1; // the one opendir holds
    }
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t      count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, task_info_t(&info), &count) ==
        KERN_SUCCESS)
        m.rssKb = long(info.resident_size / 1024);
#endif
    return m;
}

std::string formatMem(const MemStats &m) {
    std::string out;
    auto        add = [&out](const char *fmt, long v) {
        if (v < 0)
            return;
        char buf[48];
        std::snprintf(buf, sizeof buf, fmt, v);
        if (!out.empty())
            out += ", ";
        out += buf;
    };
    add("heap %ld KB", m.heapKb);
    add("rss %ld KB", m.rssKb);
    add("fds %ld", m.fds);
    add("threads %ld", m.threads);
    return out;
}

bool checkLeaksNow() {
#if defined(__SANITIZE_ADDRESS__)
    return __lsan_do_recoverable_leak_check() != 0;
#else
    return false;
#endif
}

} // namespace diag
