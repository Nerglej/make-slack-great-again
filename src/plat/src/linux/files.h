// File helpers the Linux pieces share (XDG directories, URL-scheme
// registration, sound themes). POSIX calls, not <fstream>/<filesystem>: those
// cost a static binary ~400 KB of locale and filesystem code.
#pragma once

#include <optional>
#include <string>

namespace plat::linux_files {

// The whole file; nullopt when it cannot be opened.
std::optional<std::string> readFile(const std::string &path);
bool                       isDir(const std::string &path);  // follows symlinks
bool                       isFile(const std::string &path); // a regular file, ditto

} // namespace plat::linux_files
