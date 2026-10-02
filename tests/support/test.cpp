#include "support/test.h"

#include "base/process.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <csignal>
#include <ftw.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace base::test {

namespace {
Case *g_first = nullptr, *g_last = nullptr;
int   g_failures   = 0;
bool  g_caseFailed = false;
} // namespace

int add(Case *c) {
    // Keep file order: tests read top to bottom like the source.
    if (g_last)
        g_last->next = c;
    else
        g_first = c;
    g_last = c;
    return 0;
}

bool fail(const char *file, int line, const char *expr) {
    std::fprintf(stderr, "  %s:%d: FAILED: %s\n", file, line, expr);
    ++g_failures;
    g_caseFailed = true;
    return false;
}

bool failEq(const char *file, int line, const char *expr, std::string_view a, std::string_view b) {
    fail(file, line, expr);
    std::fprintf(
        stderr,
        "    got:      \"%.*s\"\n    expected: \"%.*s\"\n",
        int(a.size()),
        a.data(),
        int(b.size()),
        b.data()
    );
    return false;
}

// ── Portable helpers ────────────────────────────────────────────────────────

#ifdef _WIN32
namespace {
// UTF-8 → UTF-16 as it is (environment values).
std::wstring wideText(std::string_view s) {
    std::wstring w;
    if (s.empty())
        return w;
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    w.resize(size_t(n));
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

// A path for the Win32 calls: '\\' separators.
std::wstring wide(std::string_view s) {
    std::wstring w = wideText(s);
    for (auto &c : w)
        if (c == L'/')
            c = L'\\';
    return w;
}

std::string narrow(std::wstring_view w) {
    std::string s;
    if (w.empty())
        return s;
    const int n =
        WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    s.resize(size_t(n));
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    for (auto &c : s)
        if (c == '\\')
            c = '/';
    return s;
}

void removeTreeW(const std::wstring &path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES)
        return;
    if (attr & FILE_ATTRIBUTE_READONLY)
        SetFileAttributesW(path.c_str(), attr & ~DWORD(FILE_ATTRIBUTE_READONLY));
    // A junction or a directory symlink is removed as the link, never entered.
    if ((attr & FILE_ATTRIBUTE_DIRECTORY) && !(attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
        WIN32_FIND_DATAW fd;
        HANDLE           h = FindFirstFileW((path + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (std::wcscmp(fd.cFileName, L".") && std::wcscmp(fd.cFileName, L".."))
                    removeTreeW(path + L"\\" + fd.cFileName);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    if (attr & FILE_ATTRIBUTE_DIRECTORY)
        RemoveDirectoryW(path.c_str());
    else
        DeleteFileW(path.c_str());
}
} // namespace

static std::string newTempDir(const char *prefix, const std::string &parent) {
    std::wstring base;
    if (parent.empty()) {
        wchar_t     buf[MAX_PATH + 2];
        const DWORD n = GetTempPathW(MAX_PATH + 2, buf);
        if (!n || n > MAX_PATH + 1)
            return {};
        base.assign(buf, n);
    } else {
        base = wide(parent);
        for (size_t i = base.find(L'\\', 3); i != std::wstring::npos; i = base.find(L'\\', i + 1))
            CreateDirectoryW(base.substr(0, i).c_str(), nullptr);
        CreateDirectoryW(base.c_str(), nullptr);
    }
    while (!base.empty() && base.back() == L'\\')
        base.pop_back();
    static const char kChars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    unsigned          seed     = unsigned(GetTickCount64()) ^ unsigned(GetCurrentProcessId() << 16);
    for (int attempt = 0; attempt < 100; ++attempt) {
        std::string name = prefix;
        for (int i = 0; i < 6; ++i) {
            seed = seed * 1103515245u + 12345u;
            name += kChars[(seed >> 16) % 36];
        }
        const std::wstring dir = base + L"\\" + wide(name);
        if (CreateDirectoryW(dir.c_str(), nullptr))
            return narrow(dir);
        if (GetLastError() != ERROR_ALREADY_EXISTS)
            return {};
    }
    return {};
}

void removeTree(const std::string &path) {
    if (!path.empty())
        removeTreeW(wide(path));
}

void setEnv(const char *name, const std::string &value) {
    // _wputenv_s with an empty value unsets; tests never set "" anyway.
    _wputenv_s(wideText(name).c_str(), wideText(value).c_str());
}

void unsetEnv(const char *name) {
    _wputenv_s(wideText(name).c_str(), L"");
}

bool setModifiedTime(const std::string &path, int64_t unixSeconds) {
    HANDLE h = CreateFileW(
        wide(path).c_str(),
        FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, // directories too
        nullptr
    );
    if (h == INVALID_HANDLE_VALUE)
        return false;
    // FILETIME: 100 ns ticks since 1601.
    const uint64_t t = uint64_t(unixSeconds + 11644473600ll) * 10000000ull;
    FILETIME       ft{DWORD(t), DWORD(t >> 32)};
    const bool     ok = SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
    return ok;
}
std::string realPath(const std::string &path) {
    HANDLE h = CreateFileW(
        wide(path).c_str(),
        0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr
    );
    if (h == INVALID_HANDLE_VALUE)
        return {};
    wchar_t     buf[4096];
    const DWORD n = GetFinalPathNameByHandleW(h, buf, 4096, FILE_NAME_NORMALIZED);
    CloseHandle(h);
    if (!n || n >= 4096)
        return {};
    std::wstring_view w(buf, n);
    if (w.substr(0, 8) == L"\\\\?\\UNC\\")
        return "//" + narrow(w.substr(8));
    if (w.substr(0, 4) == L"\\\\?\\")
        w.remove_prefix(4);
    return narrow(w);
}

void killProcess(int64_t pid) {
    if (HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, DWORD(pid))) {
        TerminateProcess(h, 1);
        CloseHandle(h);
    }
}
#else
static std::string newTempDir(const char *prefix, const std::string &parent) {
    if (!parent.empty())
        for (size_t i = parent.find('/', 1);; i = parent.find('/', i + 1)) {
            mkdir(parent.substr(0, i).c_str(), 0700);
            if (i == std::string::npos)
                break;
        }
    std::string tmpl = (parent.empty() ? std::string("/tmp") : parent) + "/" + prefix + "XXXXXX";
    return mkdtemp(tmpl.data()) ? tmpl : std::string();
}

void removeTree(const std::string &path) {
    if (path.empty())
        return;
    nftw(
        path.c_str(),
        [](const char *p, const struct stat *, int, struct FTW *) { return ::remove(p); },
        16,
        FTW_DEPTH | FTW_PHYS
    );
}

void setEnv(const char *name, const std::string &value) {
    ::setenv(name, value.c_str(), 1);
}

void unsetEnv(const char *name) {
    ::unsetenv(name);
}

bool setModifiedTime(const std::string &path, int64_t unixSeconds) {
    struct timeval tv[2] = {{time_t(unixSeconds), 0}, {time_t(unixSeconds), 0}};
    return ::utimes(path.c_str(), tv) == 0;
}

std::string realPath(const std::string &path) {
    char *p = ::realpath(path.c_str(), nullptr);
    if (!p)
        return {};
    std::string out = p;
    std::free(p);
    return out;
}

void killProcess(int64_t pid) {
    ::kill(pid_t(pid), SIGKILL);
}
#endif

std::string fileUri(const std::string &path) {
    return (path.empty() || path[0] == '/' ? "file://" : "file:///") + path;
}

// Every test's scratch directory goes when the process does.
std::string makeTempDir(const char *prefix, const std::string &parent) {
    static std::vector<std::string> made;
    std::string                     dir = newTempDir(prefix, parent);
    if (!dir.empty()) {
        if (made.empty())
            std::atexit([] {
                for (const std::string &d : made)
                    removeTree(d);
            });
        made.push_back(dir);
    }
    return dir;
}

void restoreEnv(const char *name, const std::string &old) {
    if (old.empty())
        unsetEnv(name);
    else
        setEnv(name, old);
}

namespace {

void makeDir(const std::string &path) {
#ifdef _WIN32
    CreateDirectoryW(wide(path).c_str(), nullptr);
#else
    mkdir(path.c_str(), 0700);
#endif
}

// Code that must never run in a test (the keychain, secret.h) knows a test
// process by this (base::testProcess), from static-init time on, so before
// runAll too; the variable passes it on to the processes tests start.
[[maybe_unused]] const bool g_marked = [] {
    base::markTestProcess();
    setEnv("MSGA_TEST_ISOLATED", "1");
    return true;
}();

// Tests must never reach the user's session, however they are started (ctest
// sets all this too; a direct run of a test binary does not): the headless
// backend only, D-Bus pointed at nothing, and a throwaway HOME unless ctest
// already gave one.
void isolate() {
    // The keychain and the old app's settings (base/secret.h,
    // base/old_settings.h): a temporary file. Audio (plat/audio.h) looks for
    // its helper programs only here: nowhere, so nothing reaches the
    // speakers or the microphone. Audio tests point it at fake helpers of
    // their own.
    setEnv("MSGA_TEST_ISOLATED", "1");
    setEnv("PLAT_AUDIO_HELPERS", "/nonexistent/next-test-audio");
    setEnv("PLAT_BACKEND", "headless");
#ifndef _WIN32
    setEnv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/next-test-bus");
    setEnv("DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/nonexistent/next-test-sysbus");
    unsetEnv("DISPLAY");
    unsetEnv("WAYLAND_DISPLAY");
#endif
    const char *home = std::getenv("HOME");
    std::string h    = home && std::strstr(home, "test-home") ? home : "";
    if (h.empty()) {
        h = newTempDir("next-test-home-", {});
        if (h.empty())
            return;
        setEnv("HOME", h);
        static std::string g_home;
        g_home = h;
        std::atexit([] { removeTree(g_home); });
        const char *const xdg[][2] = {
            {"XDG_CONFIG_HOME", "/.config"},
            {"XDG_DATA_HOME", "/.local/share"},
            {"XDG_CACHE_HOME", "/.cache"},
            {"XDG_STATE_HOME", "/.local/state"},
            {"XDG_RUNTIME_DIR", "/run"},
        };
        for (const auto &v : xdg)
            setEnv(v[0], h + v[1]);
        makeDir(h + "/run");
    }
#ifdef _WIN32
    // The profile folders msga reads from the environment (identity, Claude
    // Code's files, the font cache): under the test home as well. The
    // headless backend's standard folders already are (it follows HOME).
    setEnv("USERPROFILE", h);
    makeDir(h + "/AppData");
    makeDir(h + "/AppData/Roaming");
    makeDir(h + "/AppData/Local");
    setEnv("APPDATA", h + "/AppData/Roaming");
    setEnv("LOCALAPPDATA", h + "/AppData/Local");
#endif
}
} // namespace

int runAll(int argc, char **argv) {
    isolate();
    const char *filter = argc > 1 ? argv[1] : nullptr;
    int         ran = 0, failed = 0;
    for (Case *c = g_first; c; c = c->next) {
        if (filter && !std::strstr(c->name, filter))
            continue;
        g_caseFailed = false;
        c->fn();
        ++ran;
        if (g_caseFailed) {
            ++failed;
            std::fprintf(stderr, "FAIL %s\n", c->name);
        }
    }
    std::printf("%d cases, %d failed, %d failed checks\n", ran, failed, g_failures);
    return failed ? 1 : 0;
}

} // namespace base::test
