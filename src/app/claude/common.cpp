#include "app/claude/common.h"

#include "base/file.h"
#include "base/process.h"
#include "base/str.h"
#include "gfx/gfx.h"

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
    const std::string home = base::homeDir();
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

bool showsAsPicture(std::string_view path, std::string_view mime) {
    if (mime == "image/svg+xml")
        return true;
    std::string head;
    return str::startsWith(mime, "image/") && file::readRange(path, 0, 64, &head) &&
           gfx::canDecodeImage(head);
}

} // namespace claude
