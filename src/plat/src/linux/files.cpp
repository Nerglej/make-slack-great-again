#include "linux/files.h"

#include <cstdint>
#include <sys/stat.h>

namespace plat::linux_files {

std::optional<std::string> readFile(const std::string &path) {
    std::string out;
    if (!prim::file::readRange(path, 0, SIZE_MAX, &out))
        return std::nullopt;
    return out;
}

bool isFile(const std::string &path) {
    struct stat st{};
    return !path.empty() && ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

} // namespace plat::linux_files
