// Recently used folders, most recent first — the quick picks for where a new
// Claude Code session starts (the teammate page's folder menu), msga's
// RecentFolders. One list for every teammate, kept in Settings
// (claudeRecentDirs); the pure parts (bumped / rank) are what the tests pin.
#pragma once

#include "screens/shell/settings.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace shell::recent_folders {

constexpr int kMax = 10;

using Entry = Settings::RecentDir;

// A folder an existing session works in, with the session's latest activity.
struct SessionFolder {
    std::string path;
    int64_t     activity = 0; // epoch seconds, 0 = unknown
};

// One pick in the menu.
struct Choice {
    std::string path;
    int64_t     lastUsed = 0; // latest of its own use and its sessions' activity
    int         sessions = 0; // existing sessions working there
};

// The one spelling a folder is compared and stored by (forward slashes, no
// trailing slash, no "." / ".." hops).
std::string normalized(std::string_view path);

// `list` with `path` moved (or added) to the front, stamped `now`, capped.
std::vector<Entry>
bumped(std::vector<Entry> list, std::string_view path, int64_t now, int cap = kMax);

// The menu: the remembered folders merged with the ones existing sessions
// work in, most recently used first (a tie keeps the remembered order), those
// `exists` rejects left out, at most `cap`.
std::vector<Choice> rank(
    const std::vector<Entry>                       &remembered,
    const std::vector<SessionFolder>               &sessions,
    const std::function<bool(const std::string &)> &exists,
    int                                             cap = kMax
);

// Where a new session with teammate `role` starts: the folder last picked for
// it, else the last one any session started in, else `home`.
std::string teammateFolder(const Settings &s, const std::string &role, const std::string &home);
// The folder picked for a teammate: its default from now on, and first among
// the recent ones.
void        pickTeammateFolder(Settings &s, const std::string &role, const std::string &dir);
// A session actually started in `dir`: first among the recent ones
// (msga's bumpRecentAgentFolder).
void        bump(Settings &s, const std::string &dir);

} // namespace shell::recent_folders
