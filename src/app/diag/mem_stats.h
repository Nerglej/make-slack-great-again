// Process memory numbers for leak hunting (dev builds: SIGUSR1,
// MSGA_MEMSTATS=<s>, msga --soak; see scripts/soak.sh and
// scripts/run-heaptrack.sh). Not linked into release builds, but for rssKb
// (the Settings memory figure's fallback on old kernels).
//
//   heap     bytes malloc'd and not freed yet: the number a leak grows. glibc
//            mallinfo2, ASan's allocator, macOS malloc zones; -1 elsewhere
//            (musl, Windows).
//   rss      resident memory (includes freed pages malloc keeps).
//   fds      open file descriptors (Linux) — a socket or file leak.
//   threads  live threads (Linux) — a worker never joined.
// A field the platform can't tell stays -1.
#pragma once

#include <string>

namespace diag {

struct MemStats {
    long heapKb = -1, rssKb = -1;
    long fds = -1, threads = -1;
};

MemStats    sampleMem();
// Resident memory in KB (Linux VmRSS, macOS resident size); -1 elsewhere.
long        rssKb();
// "heap 41234 KB, rss 98765 KB, fds 37, threads 14" (unknown fields left out).
std::string formatMem(const MemStats &m);

// An ASan build (scripts/run-asan.sh): a LeakSanitizer pass now, with the
// app still running — its report goes to stderr. True if it found leaks.
// Other builds: false, nothing done.
bool checkLeaksNow();

} // namespace diag
