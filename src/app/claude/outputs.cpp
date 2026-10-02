#include "app/claude/outputs.h"

#include "app/claude/common.h"
#include "app/model/image_size.h"
#include "base/file.h"
#include "base/json.h"
#include "base/process.h"
#include "base/str.h"
#include "base/utf8.h"
#include "gfx/gfx.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_set>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace claude {

namespace {

constexpr int     kMaxFiles     = 10;
constexpr int64_t kMaxFileBytes = 50LL * 1024 * 1024;
// A file counts as the turn's when it was modified from a little before its
// prompt to a little after the answer (clocks, and the answer's record being
// stamped once written).
constexpr int64_t kSlackMicros  = 2'000'000;

constexpr std::string_view kIndex         = "index.json";
constexpr size_t           kMaxIndexBytes = 1024 * 1024; // ten entries are a few hundred bytes

// The kinds shown as attachments, by extension.
struct Kind {
    const char *ext;
    const char *mime;
    const char *label; // File::prettyType
};
const Kind kKinds[] = {
    {"png", "image/png", "PNG"},     {"jpg", "image/jpeg", "JPEG"},
    {"jpeg", "image/jpeg", "JPEG"},  {"gif", "image/gif", "GIF"},
    {"webp", "image/webp", "WebP"},  {"bmp", "image/bmp", "BMP"},
    {"svg", "image/svg+xml", "SVG"}, {"pdf", "application/pdf", "PDF"},
    {"mp3", "audio/mpeg", "MP3"},    {"wav", "audio/wav", "WAV"},
    {"ogg", "audio/ogg", "OGG"},     {"m4a", "audio/mp4", "M4A"},
    {"flac", "audio/flac", "FLAC"},  {"mp4", "video/mp4", "MP4"},
    {"webm", "video/webm", "WebM"},  {"mov", "video/quicktime", "MOV"},
    {"html", "text/html", "HTML"},   {"htm", "text/html", "HTML"},
    {"csv", "text/csv", "CSV"},
};

const Kind *kindOf(std::string_view path) {
    const std::string ext = str::asciiLower(file::extension(file::baseName(path)));
    for (const Kind &k : kKinds)
        if (ext == k.ext)
            return &k;
    return nullptr;
}

bool shownKind(std::string_view path) {
    return kindOf(path) != nullptr;
}

std::string_view prettyTypeOf(std::string_view name, std::string_view mime) {
    if (const Kind *k = kindOf(name); k && mime == k->mime)
        return k->label;
    return "File";
}

std::string homeDir() {
#ifdef _WIN32
    return cleanPath(base::env("USERPROFILE"));
#else
    return base::env("HOME");
#endif
}

std::string expandHome(std::string_view token) {
    if (str::startsWith(token, "~/"))
        return str::concat({homeDir(), token.substr(1)});
    return std::string(token);
}

// "file:///a/b%20c" → "/a/b c" (QUrl::toLocalFile, for the local form).
std::string localFileOf(std::string_view url) {
    return file::fromFileUrl(url);
}

bool isFile(std::string_view path) {
    return file::exists(path) && !file::isDir(path);
}

// What splits an answer into candidate paths: whitespace and `'"<>()[]{}|*,;.
bool splitsTokens(uint32_t cp) {
    return utf8::isSpace(cp) || (cp < 0x80 && std::strchr("`'\"<>()[]{}|*,;", int(cp)) && cp);
}

// Any character but [A-Za-z0-9._-] as '_': a folder name from a key.
std::string safeName(std::string_view key) {
    std::string out;
    for (size_t i = 0; i < key.size();) {
        const char c = key[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '_' || c == '-') {
            out += c;
            ++i;
        } else {
            utf8::decode(key, i);
            out += '_';
        }
    }
    return out;
}

// A folder for the copies of one answer.
std::string messageDir(const OutputContext &ctx) {
    return file::join(outputsDir(ctx.convId), safeName(ctx.messageKey));
}

// Width and height of a BMP (which model::imageSize doesn't read).
bool bmpSize(const std::string &path, int32_t *w, int32_t *h) {
    std::string data;
    if (!str::endsWith(str::asciiLower(path), ".bmp") || !file::readAll(path, &data) ||
        data.size() < 26 || data[0] != 'B' || data[1] != 'M')
        return false;
    const auto le32 = [&](size_t at) {
        const auto *p = reinterpret_cast<const unsigned char *>(data.data() + at);
        return int32_t(
            uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24
        );
    };
    *w = le32(18);
    *h = std::abs(le32(22)); // negative: top-down rows
    return *w > 0 && *h > 0;
}

model::File fileFor(json::Value e, const std::string &dir, const std::string &idPrefix, int n) {
    model::File f;
    f.id         = str::concat({idPrefix, str::number(n)});
    f.name       = e["name"].str();
    f.mime       = e["mime"].str();
    f.prettyType = prettyTypeOf(f.name, f.mime);
    f.path       = file::join(dir, e["file"].str());
    f.size       = std::max<int64_t>(0, file::size(f.path));
    // Decided here, not at copy time, so a build that decodes more (WebP)
    // shows the copies made before as pictures too.
    if (showsAsPicture(f.path, f.mime)) {
        f.width  = int32_t(e["w"].integer());
        f.height = int32_t(e["h"].integer());
    }
    return f;
}

// The copy of `src` as file `name` in `dir`, and how it's shown, written into
// `w` as an index entry; false when it can't be read.
bool copyInto(
    const std::string &src, const std::string &dir, const std::string &name, json::Writer &w
) {
    const std::string dest = file::join(dir, name);
    std::string       data;
    if (!file::readAll(src, &data) || !file::writeAtomic(dest, data, 0600))
        return false;
    const Kind *k    = kindOf(src);
    std::string mime = k ? k->mime : "application/octet-stream";
    int32_t     iw = 0, ih = 0;
    // An SVG is a picture at its own size (the image cache renders it).
    float       sw = 0, sh = 0;
    if (mime == "image/svg+xml") {
        if (gfx::svgSize(data, &sw, &sh) && sw >= 1 && sh >= 1) {
            iw = int32_t(std::lround(sw));
            ih = int32_t(std::lround(sh));
        }
    } else if (
        str::startsWith(mime, "image/") && !model::imageSize(dest, &iw, &ih) &&
        !bmpSize(dest, &iw, &ih)
    ) {
        mime = "application/octet-stream"; // unreadable: a file card, not a broken picture
    }
    w.beginObject();
    w.key("name").value(file::baseName(src));
    w.key("file").value(name);
    w.key("mime").value(mime);
    if (iw > 0) {
        w.key("w").value(int64_t(iw));
        w.key("h").value(int64_t(ih));
    }
    w.endObject();
    return true;
}

#ifdef _WIN32
std::wstring wide(std::string_view s) {
    std::wstring w;
    if (s.empty())
        return w;
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    w.resize(size_t(n));
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    for (auto &c : w)
        if (c == L'/')
            c = L'\\';
    return w;
}

bool removeTreeW(const std::wstring &path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    if (a == INVALID_FILE_ATTRIBUTES)
        return true;
    if (a & FILE_ATTRIBUTE_READONLY)
        SetFileAttributesW(path.c_str(), a & ~DWORD(FILE_ATTRIBUTE_READONLY));
    if (!(a & FILE_ATTRIBUTE_DIRECTORY))
        return DeleteFileW(path.c_str());
    if (!(a & FILE_ATTRIBUTE_REPARSE_POINT)) { // a junction or link: only itself goes
        WIN32_FIND_DATAW d;
        HANDLE           h = FindFirstFileW((path + L"\\*").c_str(), &d);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                const wchar_t *n = d.cFileName;
                if (n[0] == L'.' && (!n[1] || (n[1] == L'.' && !n[2])))
                    continue;
                removeTreeW(path + L"\\" + n);
            } while (FindNextFileW(h, &d));
            FindClose(h);
        }
    }
    return RemoveDirectoryW(path.c_str());
}
#endif

} // namespace

std::string cleanPath(std::string_view in) {
    std::string path(in);
#ifdef _WIN32
    std::replace(path.begin(), path.end(), '\\', '/');
#endif
    if (path.empty())
        return path;
    // The root part kept as it is: "/", "C:/", "//server".
    size_t rootLen = 0;
    if (path.size() >= 2 && path[1] == ':')
        rootLen = path.size() >= 3 && path[2] == '/' ? 3 : 2;
    else if (str::startsWith(path, "//"))
        rootLen = 2;
    else if (path[0] == '/')
        rootLen = 1;
    const bool                    absolute = rootLen > 0;
    std::vector<std::string_view> parts;
    std::string_view              rest = std::string_view(path).substr(rootLen);
    while (!rest.empty()) {
        const size_t           slash = rest.find('/');
        const std::string_view part  = rest.substr(0, slash);
        rest = slash == std::string_view::npos ? std::string_view() : rest.substr(slash + 1);
        if (part.empty() || part == ".")
            continue;
        if (part == "..") {
            if (!parts.empty() && parts.back() != "..")
                parts.pop_back();
            else if (!absolute)
                parts.push_back(part);
            continue;
        }
        parts.push_back(part);
    }
    std::string out = path.substr(0, rootLen);
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0)
            out += '/';
        out.append(parts[i]);
    }
    if (out.empty())
        out = ".";
    return out;
}

bool removeTree(std::string_view path) {
#ifdef _WIN32
    return removeTreeW(wide(path)) || !file::exists(path);
#else
    const std::string p(path);
    struct stat       st;
    if (::lstat(p.c_str(), &st) != 0)
        return true; // nothing there
    if (!S_ISDIR(st.st_mode))
        return ::unlink(p.c_str()) == 0;
    if (DIR *d = ::opendir(p.c_str())) {
        while (const dirent *e = ::readdir(d)) {
            const char *n = e->d_name;
            if (n[0] == '.' && (!n[1] || (n[1] == '.' && !n[2])))
                continue;
            removeTree(file::join(p, n));
        }
        ::closedir(d);
    }
    return ::rmdir(p.c_str()) == 0;
#endif
}

int64_t modifiedMicros(std::string_view path) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(wide(path).c_str(), GetFileExInfoStandard, &d))
        return -1;
    // FILETIME: 100 ns ticks since 1601.
    const uint64_t ft =
        (uint64_t(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
    return int64_t(ft / 10) - 11644473600LL * 1000000;
#else
    struct stat st;
    if (::stat(std::string(path).c_str(), &st) != 0)
        return -1;
#ifdef __APPLE__
    return int64_t(st.st_mtimespec.tv_sec) * 1000000 + st.st_mtimespec.tv_nsec / 1000;
#else
    return int64_t(st.st_mtim.tv_sec) * 1000000 + st.st_mtim.tv_nsec / 1000;
#endif
#endif
}

bool fileStat(std::string_view path, int64_t *size, int64_t *mtimeMicros) {
    *size        = -1;
    *mtimeMicros = -1;
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(wide(path).c_str(), GetFileExInfoStandard, &d))
        return false;
    const uint64_t ft =
        (uint64_t(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
    *mtimeMicros = int64_t(ft / 10) - 11644473600LL * 1000000;
    *size        = int64_t((uint64_t(d.nFileSizeHigh) << 32) | d.nFileSizeLow);
#else
    struct stat st;
    if (::stat(std::string(path).c_str(), &st) != 0)
        return false;
    *size = int64_t(st.st_size);
#ifdef __APPLE__
    *mtimeMicros = int64_t(st.st_mtimespec.tv_sec) * 1000000 + st.st_mtimespec.tv_nsec / 1000;
#else
    *mtimeMicros = int64_t(st.st_mtim.tv_sec) * 1000000 + st.st_mtim.tv_nsec / 1000;
#endif
#endif
    return true;
}

std::string simplified(std::string_view s) {
    std::string out;
    bool        gap = false;
    for (size_t i = 0; i < s.size();) {
        const size_t   at = i;
        const uint32_t cp = utf8::decode(s, i);
        if (utf8::isSpace(cp)) {
            gap = !out.empty();
            continue;
        }
        if (gap)
            out += ' ';
        gap = false;
        out.append(s.substr(at, i - at));
    }
    return out;
}

std::string_view trimmed(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e) {
        size_t i = b;
        if (!utf8::isSpace(utf8::decode(s, i)))
            break;
        b = i;
    }
    while (e > b) {
        size_t p = utf8::prevBoundary(s, e), i = p;
        if (!utf8::isSpace(utf8::decode(s, i)))
            break;
        e = p;
    }
    return s.substr(b, e - b);
}

std::vector<std::string> mentionedFiles(std::string_view text, std::string_view cwd) {
    std::vector<std::string> absolute, relative, dirs;
    for (size_t i = 0; i < text.size();) {
        // The next token: up to a separator.
        size_t start = i;
        while (start < text.size()) {
            size_t         j  = start;
            const uint32_t cp = utf8::decode(text, j);
            if (!splitsTokens(cp))
                break;
            start = j;
        }
        size_t end = start;
        while (end < text.size()) {
            size_t         j  = end;
            const uint32_t cp = utf8::decode(text, j);
            if (splitsTokens(cp))
                break;
            end = j;
        }
        i                      = end;
        std::string_view token = text.substr(start, end - start);
        while (!token.empty() && std::strchr(".:!?", token.back()))
            token.remove_suffix(1);
        std::string path;
        if (str::startsWith(token, "file://"))
            path = localFileOf(token);
        else if (token.find("://") != std::string_view::npos)
            continue; // a web link
        else
            path = expandHome(token);
        if (path.empty())
            continue;
        if (file::isAbsolute(path)) {
            if (file::isDir(path)) {
                dirs.push_back(cleanPath(path));
            } else if (shownKind(path)) {
                absolute.push_back(cleanPath(path));
                dirs.emplace_back(file::dirName(absolute.back())); // "…/x/a.svg and a.png"
            }
        } else if (shownKind(path)) {
            relative.push_back(std::move(path));
        }
    }
    if (!cwd.empty())
        dirs.emplace_back(cwd);

    std::vector<std::string>        out;
    std::unordered_set<std::string> seen;
    const auto                      add = [&](const std::string &path) {
        if (!isFile(path))
            return false;
        std::string abs = cleanPath(file::absolute(path));
        if (seen.insert(abs).second)
            out.push_back(std::move(abs));
        return true;
    };
    // In the order the text names them: absolute ones first is close enough —
    // answers name a folder, then what's in it.
    for (const std::string &p : absolute)
        add(p);
    for (const std::string &r : relative)
        for (const std::string &d : dirs)
            if (add(file::join(d, r)))
                break;
    return out;
}

std::string outputsFolder(const OutputContext &ctx) {
    return messageDir(ctx);
}

bool cachedOutputs(const OutputContext &ctx, std::vector<model::File> *files) {
    files->clear();
    if (ctx.convId.empty() || ctx.messageKey.empty() || dirs().cache.empty())
        return true; // nowhere to keep copies: none to make either
    const std::string dir = messageDir(ctx);
    std::string       indexJson;
    if (!file::readRange(file::join(dir, kIndex), 0, kMaxIndexBytes, &indexJson))
        return false;
    json::Document doc;
    if (!doc.parse(std::move(indexJson)))
        return true;
    const std::string idPrefix = str::concat({"out-", ctx.messageKey, "-"});
    int               i        = 0;
    for (json::Value e : doc.root()) {
        if (file::exists(file::join(dir, e["file"].str())))
            files->push_back(fileFor(e, dir, idPrefix, i));
        ++i;
    }
    return true;
}

void makeOutputs(std::string_view text, const OutputContext &ctx) {
    if (ctx.convId.empty() || ctx.messageKey.empty() || dirs().cache.empty())
        return;
    const std::string        dir = messageDir(ctx);
    std::vector<std::string> made;
    for (const std::string &path : mentionedFiles(text, ctx.cwd)) {
        const int64_t mtime = modifiedMicros(path);
        if (mtime < ctx.turnStart - kSlackMicros || mtime > ctx.date + kSlackMicros)
            continue; // made before this turn, or changed since the answer
        if (file::size(path) > kMaxFileBytes)
            continue;
        made.push_back(path);
        if (int(made.size()) == kMaxFiles)
            break;
    }
    file::makeDirs(dir);
    json::Writer w;
    w.beginArray();
    for (size_t i = 0; i < made.size(); ++i)
        copyInto(
            made[i], dir, str::concat({str::number(int64_t(i)), "-", file::baseName(made[i])}), w
        );
    w.endArray();
    file::writeAtomic(file::join(dir, kIndex), w.take());
}

std::vector<model::File> outputFiles(std::string_view text, const OutputContext &ctx) {
    std::vector<model::File> files;
    if (!cachedOutputs(ctx, &files)) {
        makeOutputs(text, ctx);
        cachedOutputs(ctx, &files);
    }
    return files;
}

std::string outputsDir(std::string_view convId) {
    return str::concat({dirs().cache, "/files/", safeName(convId)});
}

void clearOutputs(std::string_view convId) {
    if (!convId.empty() && !dirs().cache.empty())
        removeTree(outputsDir(convId));
}

void pruneOutputs(const std::vector<std::string> &keep) {
    if (dirs().cache.empty())
        return;
    std::unordered_set<std::string> kept;
    for (const std::string &id : keep)
        kept.insert(safeName(id));
    const std::string           root = dirs().cache + "/files";
    std::vector<file::DirEntry> entries;
    file::listDir(root, &entries);
    for (const file::DirEntry &e : entries)
        if (e.isDir && !kept.count(e.name))
            removeTree(file::join(root, e.name));
}

} // namespace claude
