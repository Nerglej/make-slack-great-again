// File helpers the Linux pieces share (XDG directories, URL-scheme
// registration, sound themes). POSIX calls, not <fstream>/<filesystem>: those
// cost a static binary ~400 KB of locale and filesystem code.
#pragma once

#include "prim/file.h"

#include <optional>
#include <string>

namespace plat::linux_files {

// The whole regular file; nullopt when it cannot be read.
std::optional<std::string> readFile(const std::string &path);
using prim::file::isDir;              // follows symlinks
bool isFile(const std::string &path); // a regular file, ditto

} // namespace plat::linux_files
