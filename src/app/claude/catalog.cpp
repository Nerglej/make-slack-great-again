#include "app/claude/catalog.h"

#include "app/claude/outputs.h"
#include "app/claude/roles.h"
#include "app/claude/roster.h"
#include "app/claude/transcript.h"
#include "base/file.h"
#include "base/json.h"
#include "base/str.h"
#include "base/utf8.h"

#include <algorithm>

namespace claude {
namespace {

constexpr int64_t kEndBytes = 96 * 1024; // read from each end of a transcript

// One line, at most 200 characters.
std::string oneLine(std::string_view s) {
    std::string out = str::simplified(s);
    if (utf8::countCodePoints(out) > 200) {
        size_t at = 0;
        for (int n = 0; n < 199; ++n)
            at = utf8::nextBoundary(out, at);
        out.resize(at);
        out += "…";
    }
    return out;
}

// What someone typed, from a "user" record, on one line; "" when it's none
// (as the session's chat has it: promptOfRecord). A sidechain is a
// subagent's, not the session's.
std::string typedLine(const json::Value &o) {
    return o["isSidechain"].boolean() ? std::string() : oneLine(promptOfRecord(o));
}

} // namespace

bool catalogEntryFrom(std::string_view head, std::string_view tail, CatalogEntry &e) {
    std::string aiTitle, customTitle, summary;
    const auto  read = [&](std::string_view line, bool fromHead) {
        if (line.empty())
            return;
        json::Document doc;
        if (!doc.parse(std::string(line)))
            return;
        const json::Value      o    = doc.root();
        const std::string_view type = o["type"].str();
        if (e.cwd.empty())
            e.cwd = o["cwd"].str();
        if (type == "user") {
            std::string p = typedLine(o);
            if (p.empty())
                return;
            if (fromHead && e.firstPrompt.empty())
                e.firstPrompt = p;
            if (!fromHead)
                e.lastPrompt = std::move(p);
        } else if (type == "custom-title") {
            customTitle = str::trim(o["customTitle"].str());
        } else if (type == "ai-title") {
            aiTitle = str::trim(o["aiTitle"].str());
        } else if (type == "summary") {
            summary = str::trim(o["summary"].str());
        } else if (type == "last-prompt" && !fromHead) {
            if (std::string p = oneLine(o["lastPrompt"].str()); !p.empty())
                e.lastPrompt = std::move(p);
        }
    };
    str::Splitter    headLines(head, '\n'), tailLines(tail, '\n');
    std::string_view line;
    while (headLines.next(&line))
        read(line, true);
    tailLines.next(&line); // the tail's first line may have been cut
    while (tailLines.next(&line))
        read(line, false);
    if (e.firstPrompt.empty() && e.lastPrompt.empty())
        return false;
    // Recorded with the first request, so near the start — a long line may be
    // cut there, hence the raw search.
    RoleMark mark = roleInTranscriptBytes(head);
    if (mark.id.empty())
        mark = roleInTranscriptBytes(tail);
    e.role     = std::move(mark.id);
    e.roleName = std::move(mark.name);
    e.title    = !customTitle.empty() ? customTitle : !aiTitle.empty() ? aiTitle : summary;
    return true;
}

bool readCatalogEntry(const std::string &transcriptPath, CatalogEntry &e) {
    const int64_t size = file::size(transcriptPath);
    if (size < 0)
        return false;
    std::string head, tail;
    if (size <= 2 * kEndBytes) {
        if (!file::readAll(transcriptPath, &head))
            return false;
        tail = "\n" + head; // both ends are the whole file
    } else {
        // A cut last line of the head just doesn't parse.
        if (!file::readRange(transcriptPath, 0, size_t(kEndBytes), &head) ||
            !file::readRange(transcriptPath, size - kEndBytes, size_t(kEndBytes), &tail))
            return false;
    }
    const std::string abs = cleanPath(file::absolute(transcriptPath));
    e.sessionId           = Paths::transcriptSessionId(abs);
    e.transcriptPath      = abs;
    file::Stat st;
    e.modifiedMs = file::stat(abs, &st) ? st.mtimeMicros / 1000 : 0;
    return catalogEntryFrom(head, tail, e);
}

std::vector<CatalogEntry> scanCatalog(const std::string &projectsDir) {
    std::vector<CatalogEntry>   out;
    std::vector<file::DirEntry> folders, files;
    file::listDir(projectsDir, &folders);
    for (const file::DirEntry &d : folders) {
        if (!d.isDir)
            continue;
        const std::string folder = file::join(projectsDir, d.name);
        file::listDir(folder, &files);
        for (const file::DirEntry &f : files) {
            if (f.isDir || !str::endsWith(f.name, ".jsonl"))
                continue;
            CatalogEntry e;
            if (readCatalogEntry(file::join(folder, f.name), e))
                out.push_back(std::move(e));
        }
    }
    std::sort(out.begin(), out.end(), [](const CatalogEntry &a, const CatalogEntry &b) {
        return a.modifiedMs > b.modifiedMs;
    });
    return out;
}

} // namespace claude
