#include "app/claude/common.h"

#include "base/process.h"

namespace claude {

namespace {
Dirs gDirs;
} // namespace

void setDirs(Dirs d) {
    gDirs = std::move(d);
}

const Dirs &dirs() {
    return gDirs;
}

std::string homeRelative(std::string_view path) {
#ifdef _WIN32
    std::string home = base::env("USERPROFILE");
    for (char &c : home)
        if (c == '\\')
            c = '/';
#else
    const std::string home = base::env("HOME");
#endif
    if (!home.empty() && path.substr(0, home.size()) == home &&
        (path.size() == home.size() || path[home.size()] == '/'))
        return "~" + std::string(path.substr(home.size()));
    std::string out(path);
#ifdef _WIN32
    for (char &c : out)
        if (c == '/')
            c = '\\';
#endif
    return out;
}

} // namespace claude
