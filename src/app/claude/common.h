// What the Claude Code modules share: where msga keeps its own files for the
// workspace, and the small types several modules pass around.
//
// The directories are set once, by whoever creates the backend (the accounts
// controller; tests point them at a temp dir), before any module touches a
// file. Nothing here depends on plat.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace claude {

struct Dirs {
    // msga's own data for the workspace: the team (team/<id>.md), the known
    // sessions, your profile — <dataDir>/claude-code (app/identity.h).
    std::string data;
    // Copies msga can make again or lose: pasted images (images/), files sent
    // to sessions (uploads/), files answers made (files/) —
    // <cacheDir>/claude-code.
    std::string cache;
};
void        setDirs(Dirs d);
const Dirs &dirs();

// "~/src/x" for a path under $HOME (native separators elsewhere).
std::string homeRelative(std::string_view path);

// Whether the image file at `path` (of type `mime`) shows as a picture: an
// SVG, or a format this build decodes. Anything else is a file card, never an
// empty preview.
bool showsAsPicture(std::string_view path, std::string_view mime);

// A slash command a session offers (Claude Code's own, a skill's, a project
// command), or one msga runs itself (source "msga").
struct SlashCommand {
    std::string name;          // without the slash: "compact"
    std::string desc;          // one line
    std::string usage;         // argument hint: "<question>"; may be empty
    std::string source;        // "builtin", "project", "user", "plugin", "msga", …
    bool        local = false; // msga answers it itself, nothing is sent (/status, /clear)
};

} // namespace claude
