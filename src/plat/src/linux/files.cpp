#include "linux/files.h"

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace plat::linux_files {

std::optional<std::string> readFile(const std::string &path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return std::nullopt;
    std::string out;
    char        buf[4096];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        out.append(buf, size_t(n));
    }
    ::close(fd);
    return out;
}

bool isDir(const std::string &path) {
    struct stat st{};
    return !path.empty() && ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool isFile(const std::string &path) {
    struct stat st{};
    return !path.empty() && ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

} // namespace plat::linux_files
