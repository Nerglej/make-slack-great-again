#include "prim/file.h"

#include <cerrno>
#include <charconv>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace prim::file {

bool readRange(std::string_view path, int64_t offset, size_t maxBytes, std::string *out) {
    out->clear();
    const std::string p(path);
    const int         fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    struct stat st;
    const bool  ok = offset >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode);
    if (ok && offset < int64_t(st.st_size)) {
        const auto left = uint64_t(st.st_size) - uint64_t(offset);
        out->resize(left < maxBytes ? size_t(left) : maxBytes);
        size_t got = 0;
        while (got < out->size()) {
            const ssize_t r =
                ::pread(fd, out->data() + got, out->size() - got, off_t(offset + got));
            if (r < 0 && errno == EINTR)
                continue;
            if (r <= 0)
                break;
            got += size_t(r);
        }
        out->resize(got); // the file may have shrunk under us
    }
    ::close(fd);
    return ok;
}

bool writeAtomic(std::string_view path, std::string_view data, int mode, bool durable) {
    const std::string target(path);
    // The parent ("/a" has the root, which always exists).
    if (const size_t slash = path.rfind('/');
        slash != std::string_view::npos && slash > 0 && !makeDirs(path.substr(0, slash)))
        return false;
    // Same directory as the target so rename() stays within one filesystem;
    // the pid keeps two processes from sharing a temp name.
    char        pid[16];
    const auto  end = std::to_chars(pid, pid + sizeof pid, int(getpid())).ptr;
    std::string tmp = target + ".tmp";
    tmp.append(pid, end);
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0)
        return false;
    size_t put = 0;
    bool   ok  = true;
    while (put < data.size()) {
        const ssize_t w = ::write(fd, data.data() + put, data.size() - put);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0) {
            ok = false;
            break;
        }
        put += size_t(w);
    }
    // fsync before rename: otherwise a crash can leave the renamed file empty
    // on ext4/xfs with delayed allocation.
    ok = ok && (!durable || ::fsync(fd) == 0);
    ok = (::close(fd) == 0) && ok;
    if (ok)
        ok = ::rename(tmp.c_str(), target.c_str()) == 0;
    if (!ok)
        ::unlink(tmp.c_str());
    return ok;
}

bool isDir(std::string_view path) {
    struct stat st;
    return ::stat(std::string(path).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool makeDirs(std::string_view path) {
    if (path.empty())
        return false;
    if (isDir(path))
        return true;
    // Create each missing ancestor, shallowest first.
    for (size_t i = 1; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            const std::string p(path.substr(0, i));
            if (p.back() != '/' && !isDir(p) && ::mkdir(p.c_str(), 0755) != 0 && errno != EEXIST)
                return false;
        }
    }
    return isDir(path);
}

} // namespace prim::file
