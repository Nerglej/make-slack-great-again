#include "app/claude/catalog.h"

#include "app/claude/outputs.h"
#include "app/claude/roles.h"
#include "app/claude/roster.h"
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

// What someone typed, from a "user" record — not tool output, not what Claude
// Code tells the model, not a command's terminal output. "" when it's none.
std::string typedPrompt(json::Value o) {
    if (o["isMeta"].boolean() || o["isCompactSummary"].boolean() || o["isSidechain"].boolean())
        return {};
    const json::Value origin = o["origin"];
    if (origin.isObject() && origin["kind"].str() != "human")
        return {};
    const json::Value content = o["message"]["content"];
    std::string       text;
    if (content.isString()) {
        text = content.str();
    } else {
        for (json::Value b : content) {
            const std::string_view t = b["type"].str();
            if (t == "tool_result")
                return {};
            if (t == "text") {
                text += b["text"].str();
                text += ' ';
            }
        }
    }
    const std::string_view     t        = trimmed(text);
    // "<command-name>/compact</command-name>…" is how a slash command is kept.
    constexpr std::string_view kCommand = "<command-name>";
    if (str::startsWith(t, kCommand)) {
        const size_t end = t.find("</command-name>");
        return end != std::string_view::npos && end > 0
                   ? std::string(str::trim(t.substr(kCommand.size(), end - kCommand.size())))
                   : std::string();
    }
    if (str::startsWith(t, "<"))
        return {}; // caveats, command output, task notifications
    return oneLine(t);
}

// Calls `read` for each line of `bytes`, less the first when it may have been
// cut (a tail).
template <class F>
void eachLine(std::string_view bytes, bool skipFirst, F &&read) {
    bool first = true;
    while (true) {
        const size_t           nl   = bytes.find('\n');
        const std::string_view line = bytes.substr(0, nl);
        if (!(first && skipFirst))
            read(line);
        first = false;
        if (nl == std::string_view::npos)
            break;
        bytes.remove_prefix(nl + 1);
    }
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
            std::string p = typedPrompt(o);
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
    eachLine(head, false, [&](std::string_view l) { read(l, true); });
    eachLine(tail, true, [&](std::string_view l) { read(l, false); });
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
    e.modifiedMs          = modifiedMicros(abs) / 1000;
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
