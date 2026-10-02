#include "base/file.h"

#include "base/str.h"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace file {

#ifdef _WIN32
namespace {
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
} // namespace

bool readAll(std::string_view path, std::string *out) {
    out->clear();
    HANDLE h = CreateFileW(
        wide(path).c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (h == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER sz;
    bool          ok = GetFileSizeEx(h, &sz) && sz.QuadPart < (1ll << 31);
    if (ok) {
        out->resize(size_t(sz.QuadPart));
        DWORD got = 0;
        ok = out->empty() ||
             (ReadFile(h, out->data(), DWORD(out->size()), &got, nullptr) && got == out->size());
    }
    CloseHandle(h);
    if (!ok)
        out->clear();
    return ok;
}

bool readRange(std::string_view path, int64_t offset, size_t maxBytes, std::string *out) {
    out->clear();
    HANDLE h = CreateFileW(
        wide(path).c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (h == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER sz, at;
    at.QuadPart = offset;
    bool ok = offset >= 0 && GetFileSizeEx(h, &sz) && SetFilePointerEx(h, at, nullptr, FILE_BEGIN);
    if (ok && offset < sz.QuadPart) {
        const int64_t left = sz.QuadPart - offset;
        out->resize(size_t(left < int64_t(maxBytes) ? left : int64_t(maxBytes)));
        DWORD got = 0;
        ok        = ReadFile(h, out->data(), DWORD(out->size()), &got, nullptr);
        out->resize(ok ? size_t(got) : 0);
    }
    CloseHandle(h);
    return ok;
}

bool overwrite(std::string_view path, std::string_view data) {
    HANDLE h = CreateFileW(
        wide(path).c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        TRUNCATE_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD      put = 0;
    const bool ok = data.empty() || (WriteFile(h, data.data(), DWORD(data.size()), &put, nullptr) &&
                                     put == data.size());
    CloseHandle(h);
    return ok;
}

bool writeAtomic(std::string_view path, std::string_view data, int mode) {
    (void)mode; // NTFS: the user's profile ACLs apply
    makeDirs(dirName(path));
    const std::wstring target = wide(path);
    const std::wstring tmp    = target + L".tmp~";
    HANDLE             h      = CreateFileW(
        tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr
    );
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD put = 0;
    bool  ok = data.empty() ||
               (WriteFile(h, data.data(), DWORD(data.size()), &put, nullptr) && put == data.size());
    ok       = ok && FlushFileBuffers(h);
    CloseHandle(h);
    if (ok)
        ok = MoveFileExW(
            tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
        );
    if (!ok)
        DeleteFileW(tmp.c_str());
    return ok;
}

bool exists(std::string_view path) {
    return GetFileAttributesW(wide(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool isDir(std::string_view path) {
    const DWORD a = GetFileAttributesW(wide(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

int64_t size(std::string_view path) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(wide(path).c_str(), GetFileExInfoStandard, &d))
        return -1;
    return (int64_t(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
}

bool listDir(std::string_view dir, std::vector<DirEntry> *out) {
    out->clear();
    WIN32_FIND_DATAW d;
    HANDLE           h = FindFirstFileExW(
        wide(join(dir, "*")).c_str(), FindExInfoBasic, &d, FindExSearchNameMatch, nullptr, 0
    );
    if (h == INVALID_HANDLE_VALUE)
        return false;
    do {
        const wchar_t *n = d.cFileName;
        if (n[0] == L'.' && (!n[1] || (n[1] == L'.' && !n[2])))
            continue;
        DirEntry  e;
        const int len = WideCharToMultiByte(CP_UTF8, 0, n, -1, nullptr, 0, nullptr, nullptr);
        e.name.resize(size_t(len > 0 ? len - 1 : 0));
        WideCharToMultiByte(CP_UTF8, 0, n, -1, e.name.data(), len, nullptr, nullptr);
        e.isDir  = d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
        e.hidden = (d.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) || n[0] == L'.';
        e.size   = e.isDir ? 0 : (int64_t(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
        // FILETIME: 100 ns ticks since 1601.
        const uint64_t ft =
            (uint64_t(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
        e.mtime = int64_t(ft / 10000000ULL) - 11644473600LL;
        out->push_back(std::move(e));
    } while (FindNextFileW(h, &d));
    FindClose(h);
    return true;
}

static bool makeDir(std::string_view p) {
    return CreateDirectoryW(wide(p).c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool remove(std::string_view path) {
    const std::wstring w = wide(path);
    return isDir(path) ? RemoveDirectoryW(w.c_str()) : DeleteFileW(w.c_str());
}

bool touch(std::string_view path) {
    HANDLE h = CreateFileW(
        wide(path).c_str(),
        FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (h == INVALID_HANDLE_VALUE)
        return false;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    const bool ok = SetFileTime(h, nullptr, nullptr, &now) != 0;
    CloseHandle(h);
    return ok;
}

bool isAbsolute(std::string_view p) {
    return (p.size() >= 3 && p[1] == ':' && (p[2] == '/' || p[2] == '\\')) ||
           (p.size() >= 2 && (p[0] == '/' || p[0] == '\\') && (p[1] == '/' || p[1] == '\\'));
}

std::string absolute(std::string_view path) {
    if (isAbsolute(path))
        return std::string(path);
    wchar_t     buf[MAX_PATH];
    const DWORD n = GetCurrentDirectoryW(MAX_PATH, buf);
    std::string cwd;
    if (n > 0 && n < MAX_PATH) {
        const int len = WideCharToMultiByte(CP_UTF8, 0, buf, int(n), nullptr, 0, nullptr, nullptr);
        cwd.resize(size_t(len));
        WideCharToMultiByte(CP_UTF8, 0, buf, int(n), cwd.data(), len, nullptr, nullptr);
        for (auto &c : cwd)
            if (c == '\\')
                c = '/';
    }
    // "/x" is rooted on the current drive: "C:/x", never "<cwd>/x".
    if (!path.empty() && (path[0] == '/' || path[0] == '\\'))
        return cwd.size() >= 2 && cwd[1] == ':' ? cwd.substr(0, 2) + std::string(path)
                                                : std::string(path);
    return join(cwd, path);
}
#else
bool readAll(std::string_view path, std::string *out) {
    out->clear();
    const std::string p(path);
    const int         fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    struct stat st;
    bool        ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode);
    if (ok) {
        out->resize(size_t(st.st_size));
        size_t got = 0;
        while (got < out->size()) {
            const ssize_t r = ::read(fd, out->data() + got, out->size() - got);
            if (r < 0 && errno == EINTR)
                continue;
            if (r <= 0)
                break;
            got += size_t(r);
        }
        out->resize(got); // the file may have shrunk under us
    }
    ::close(fd);
    if (!ok)
        out->clear();
    return ok;
}

bool readRange(std::string_view path, int64_t offset, size_t maxBytes, std::string *out) {
    out->clear();
    const std::string p(path);
    const int         fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    struct stat st;
    const bool  ok = offset >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode);
    if (ok && offset < int64_t(st.st_size)) {
        const int64_t left = int64_t(st.st_size) - offset;
        out->resize(size_t(left < int64_t(maxBytes) ? left : int64_t(maxBytes)));
        size_t got = 0;
        while (got < out->size()) {
            const ssize_t r =
                ::pread(fd, out->data() + got, out->size() - got, off_t(offset + got));
            if (r < 0 && errno == EINTR)
                continue;
            if (r <= 0)
                break;
            got += size_t(r);
        }
        out->resize(got);
    }
    ::close(fd);
    return ok;
}

bool overwrite(std::string_view path, std::string_view data) {
    const std::string p(path);
    const int         fd = ::open(p.c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd < 0)
        return false;
    size_t put = 0;
    while (put < data.size()) {
        const ssize_t w = ::write(fd, data.data() + put, data.size() - put);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            break;
        put += size_t(w);
    }
    return ::close(fd) == 0 && put == data.size();
}

bool writeAtomic(std::string_view path, std::string_view data, int mode) {
    const std::string target(path);
    const std::string dir(dirName(path));
    if (!dir.empty() && !makeDirs(dir))
        return false;
    // Same directory as the target so rename() stays within one filesystem;
    // the pid keeps two processes from sharing a temp name.
    const std::string tmp = str::concat({target, ".tmp", str::number(getpid())});
    const int         fd  = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0)
        return false;
    size_t put = 0;
    bool   ok  = true;
    while (put < data.size()) {
        const ssize_t w = ::write(fd, data.data() + put, data.size() - put);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0) {
            ok = false;
            break;
        }
        put += size_t(w);
    }
    // fsync before rename: otherwise a crash can leave the renamed file empty
    // on ext4/xfs with delayed allocation.
    ok = ok && ::fsync(fd) == 0;
    ok = (::close(fd) == 0) && ok;
    if (ok)
        ok = ::rename(tmp.c_str(), target.c_str()) == 0;
    if (!ok)
        ::unlink(tmp.c_str());
    return ok;
}

bool exists(std::string_view path) {
    struct stat st;
    return ::stat(std::string(path).c_str(), &st) == 0;
}

bool isDir(std::string_view path) {
    struct stat st;
    return ::stat(std::string(path).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

int64_t size(std::string_view path) {
    struct stat st;
    if (::stat(std::string(path).c_str(), &st) != 0)
        return -1;
    return int64_t(st.st_size);
}

bool listDir(std::string_view dir, std::vector<DirEntry> *out) {
    out->clear();
    const std::string d(dir);
    DIR              *h = ::opendir(d.c_str());
    if (!h)
        return false;
    while (const dirent *e = ::readdir(h)) {
        const char *n = e->d_name;
        if (n[0] == '.' && (!n[1] || (n[1] == '.' && !n[2])))
            continue;
        DirEntry de;
        de.name   = n;
        de.hidden = n[0] == '.';
        struct stat st;
        // stat, not lstat: a link to a folder is browsed like one.
        if (::stat(join(d, n).c_str(), &st) == 0) {
            de.isDir = S_ISDIR(st.st_mode);
            de.size  = de.isDir ? 0 : int64_t(st.st_size);
            de.mtime = int64_t(st.st_mtime);
        }
        out->push_back(std::move(de));
    }
    ::closedir(h);
    return true;
}

static bool makeDir(std::string_view p) {
    return ::mkdir(std::string(p).c_str(), 0755) == 0 || errno == EEXIST;
}

bool remove(std::string_view path) {
    return ::remove(std::string(path).c_str()) == 0;
}

bool touch(std::string_view path) {
    return ::utimensat(AT_FDCWD, std::string(path).c_str(), nullptr, 0) == 0;
}

bool isAbsolute(std::string_view p) {
    return !p.empty() && p[0] == '/';
}

std::string absolute(std::string_view path) {
    if (isAbsolute(path))
        return std::string(path);
    char buf[4096];
    if (!::getcwd(buf, sizeof buf))
        return std::string(path);
    return join(buf, path);
}
#endif

bool copy(std::string_view from, std::string_view to) {
    // Attachments are small enough to hold once; atomic so a failed copy
    // never leaves half a file under the chosen name.
    std::string data;
    return readAll(from, &data) && writeAtomic(to, data);
}

static bool isSep(char c) {
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

bool makeDirs(std::string_view path) {
    if (path.empty())
        return false;
    if (isDir(path))
        return true;
    // Create each missing ancestor, shallowest first.
    for (size_t i = 1; i <= path.size(); ++i) {
        if (i == path.size() || isSep(path[i])) {
            const std::string_view prefix = path.substr(0, i);
            if (!prefix.empty() && !isSep(prefix.back()) &&
                !(prefix.size() == 2 && prefix[1] == ':'))
                if (!isDir(prefix) && !makeDir(prefix))
                    return false;
        }
    }
    return isDir(path);
}

std::string_view dirName(std::string_view path) {
    size_t i = path.size();
    while (i > 0 && !isSep(path[i - 1]))
        --i;
    if (i == 0)
        return {};
    // Keep the root: dirName("/a") is "/".
    return i == 1 ? path.substr(0, 1) : path.substr(0, i - 1);
}

std::string_view baseName(std::string_view path) {
    size_t i = path.size();
    while (i > 0 && !isSep(path[i - 1]))
        --i;
    return path.substr(i);
}

std::string_view extension(std::string_view path) {
    const std::string_view b   = baseName(path);
    const size_t           dot = b.rfind('.');
    if (dot == std::string_view::npos || dot == 0)
        return {};
    return b.substr(dot + 1);
}

std::string join(std::string_view dir, std::string_view rel) {
    if (dir.empty())
        return std::string(rel);
    std::string out(dir);
    if (!isSep(out.back()))
        out += '/';
    while (rel.size() >= 2 && rel[0] == '.' && isSep(rel[1]))
        rel.remove_prefix(2); // "./assets" → "assets"
    out.append(rel);
    return out;
}

std::string resolve(std::string_view base, std::string_view rel) {
    return isAbsolute(rel) ? std::string(rel) : join(base, rel);
}

// ── file:// URLs ────────────────────────────────────────────────────────────

std::string toFileUrl(std::string_view path) {
    std::string p(path);
#ifdef _WIN32
    for (char &c : p)
        if (c == '\\')
            c = '/';
#endif
    std::string out = "file://";
    size_t      i   = 0;
    if (p.size() >= 2 && p[0] == '/' && p[1] == '/')
        i = 2; // UNC "//server/share/x": the server is the authority
    else if (p.empty() || p[0] != '/')
        out += '/'; // "C:/x" → "file:///C:/x"
    // RFC 3986 pchar, as QUrl encodes a path: unreserved, sub-delims, ':',
    // '@' and '/' as they are, every other byte (UTF-8 too) as %XX.
    static const char kHex[] = "0123456789ABCDEF";
    for (; i < p.size(); ++i) {
        const unsigned char c    = (unsigned char)p[i];
        const bool          keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                   (c >= '0' && c <= '9') || std::strchr("-._~!$&'()*+,;=:@/", c);
        if (keep && c) {
            out += char(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
    }
    return out;
}

std::string fromFileUrl(std::string_view url) {
    if (url.substr(0, 7) != "file://")
        return {};
    std::string_view rest = url.substr(7);
    std::string      out;
    if (!rest.empty() && rest[0] != '/') { // an authority
        const size_t     slash = rest.find('/');
        std::string_view host  = rest.substr(0, slash);
        rest = slash == std::string_view::npos ? std::string_view() : rest.substr(slash);
#ifdef _WIN32
        if (host != "localhost")
            out = "//" + std::string(host); // UNC
#else
        (void)host; // another machine's file: its path here, as before
#endif
    }
    const auto hex = [](char c) {
        return c >= '0' && c <= '9'   ? c - '0'
               : c >= 'a' && c <= 'f' ? c - 'a' + 10
               : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                      : -1;
    };
    for (size_t i = 0; i < rest.size(); ++i) {
        int hi = -1, lo = -1;
        if (rest[i] == '%' && i + 2 < rest.size() && (hi = hex(rest[i + 1])) >= 0 &&
            (lo = hex(rest[i + 2])) >= 0) {
            out += char(hi << 4 | lo);
            i += 2;
        } else {
            out += rest[i];
        }
    }
#ifdef _WIN32
    if (out.size() > 2 && out[0] == '/' && out[2] == ':')
        out.erase(0, 1); // "/C:/x"
#endif
    return out;
}

} // namespace file
