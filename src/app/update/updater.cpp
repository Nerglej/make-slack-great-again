#include "app/update/updater.h"

#include "app/model/jobs.h"
#include "base/crypto.h"
#include "base/file.h"
#include "base/i18n.h"
#include "base/json.h"
#include "base/log.h"
#include "base/process.h"
#include "base/str.h"
#include "base/time.h"
#include "net/net.h"
#include "plat/plat.h"

#include <algorithm>

#ifdef _WIN32
#include "base/winstr.h"

#include <windows.h>
#endif

using i18n::arg;
using i18n::tr;

namespace update {

namespace {

constexpr const char *kBase              = "https://msga.app/download/";
// The whole binary in one answer: minutes, not the default 30 s.
constexpr int         kDownloadTimeoutMs = 10 * 60'000;

#ifdef _WIN32
using base::widePath;
#endif

// Puts the verified download in place (a worker thread); "" or why not.
std::string install(const std::string &target, const std::string &bytes) {
#if defined(_WIN32)
    // Windows locks a running .exe against replacing, not against renaming:
    // the current one moves aside first, then the new one takes its name.
    const std::string tmp = target + ".download", backup = target + ".old";
    if (!file::writeAtomic(tmp, bytes))
        return arg(tr("Cannot write update to %1"), tmp);
    file::remove(backup);
    if (!MoveFileExW(
            widePath(target).c_str(), widePath(backup).c_str(), MOVEFILE_REPLACE_EXISTING
        )) {
        file::remove(tmp);
        return arg(
            tr("Could not move current binary \xE2\x80\x94 check file permissions on %1"), target
        );
    }
    if (!MoveFileExW(widePath(tmp).c_str(), widePath(target).c_str(), MOVEFILE_REPLACE_EXISTING)) {
        MoveFileExW(widePath(backup).c_str(), widePath(target).c_str(), 0); // best-effort restore
        file::remove(tmp);
        return arg(tr("Could not place new binary at %1"), target);
    }
    return {};
#elif defined(__APPLE__)
    // The DMG, for the user to open.
    if (!file::writeAtomic(target, bytes))
        return arg(tr("Cannot write update to %1"), target);
    return {};
#else
    // A temp file next to the binary renamed over it: the directory entry
    // swaps atomically, the running process keeps its inode.
    if (!file::writeAtomic(target, bytes, 0755))
        return arg(tr("Could not replace binary: %1"), target);
    return {};
#endif
}

} // namespace

bool parseManifest(std::string_view text, Manifest *out) {
    json::Document d;
    if (!d.parse(std::string(text), nullptr))
        return false;
    const int64_t v = d.root()["version"].integer();
    if (v <= 0 || v > 1'000'000'000)
        return false;
    out->version = int(v);
    out->sha256  = str::asciiLower(d.root()["sha256"].str());
    return true;
}

std::string assetFor(std::string_view os, std::string_view arch) {
    if (os == "linux" && arch == "x86_64")
        return "msga-linux-x86_64";
    if (os == "windows" && arch == "x86_64")
        return "msga-windows-x86_64.exe";
    if (os == "macos" && arch == "arm64")
        return "msga-macos-arm64.dmg";
    return {};
}

std::string asset() {
#if defined(_WIN32)
    const char *os = "windows";
#elif defined(__APPLE__)
    const char *os = "macos";
#elif defined(__linux__)
    const char *os = "linux";
#else
    const char *os = "";
#endif
#if defined(__x86_64__) || defined(_M_X64)
    const char *arch = "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    const char *arch = "arm64";
#else
    const char *arch = "";
#endif
    return assetFor(os, arch);
}

std::string assetUrl() {
    const std::string a = asset();
    return a.empty() ? std::string() : kBase + a;
}

std::string manifestUrl() {
    const std::string a = asset();
    return a.empty() ? std::string() : str::concat({kBase, a, ".manifest"});
}

bool checksumMatches(std::string_view bytes, std::string_view expectedHex) {
    if (expectedHex.empty())
        return true; // older manifests carry no hash: unchecked
    return str::iequals(crypto::hex(crypto::bytes(crypto::sha256(bytes))), expectedHex);
}

// ── Updater ─────────────────────────────────────────────────────────────────

Updater::Updater(plat::App &app, net::Client &client, int currentVersion)
    : _app(app), _client(client), _current(currentVersion), _manifestUrl(manifestUrl()),
      _assetUrl(assetUrl()) {
#if defined(__APPLE__)
    const std::string dl = app.standardDir(plat::StandardDir::Downloads);
    if (!dl.empty() && !asset().empty())
        _target = file::join(dl, asset());
#else
    _target = base::executablePath();
#endif
#ifdef _WIN32
    // The backup the previous update's rename-away step left.
    if (!_target.empty())
        model::runInBackground(app, [old = _target + ".old"] { file::remove(old); }, [] {});
#endif
}

Updater::~Updater() = default;

void Updater::setUrls(std::string manifest, std::string asset) {
    _manifestUrl = std::move(manifest);
    _assetUrl    = std::move(asset);
}

int Updater::listen(Listener fn) {
    _listeners.push_back({_nextId, std::move(fn)});
    return _nextId++;
}

void Updater::unlisten(int id) {
    std::erase_if(_listeners, [id](const Slot &s) { return s.id == id; });
}

void Updater::emit(Event e) {
    const std::vector<Slot> copy = _listeners; // a listener may unlisten
    for (const Slot &s : copy)
        if (s.fn)
            s.fn(e);
}

void Updater::checkInBackground(bool autoCheck) {
    if (!autoCheck || _busy || _ready)
        return;
    fetch(true);
}

void Updater::checkNow() {
    if (_busy)
        return;
    emit({Event::Kind::Started, 0, {}});
    fetch(false);
}

void Updater::fetch(bool silent) {
    if (_manifestUrl.empty() || _target.empty()) {
        if (!silent)
            emit(
                {Event::Kind::Failed,
                 0,
                 tr("Automatic updates are not supported on this platform.")}
            );
        return;
    }
    _busy = true;
    net::Request req;
    req.url                   = _manifestUrl;
    std::weak_ptr<char> alive = _alive;
    _client.send(std::move(req), [this, alive, silent](net::Response r) {
        if (alive.expired())
            return;
        _busy = false;
        if (!r.ok()) {
            LOG_WARN("update", "manifest: %s (HTTP %d)", r.error.c_str(), r.status);
            if (!silent)
                emit(
                    {Event::Kind::Failed,
                     0,
                     r.error.empty() ? "HTTP " + str::number(int64_t(r.status)) : r.error}
                );
            return;
        }
        Manifest m;
        if (!parseManifest(r.body, &m)) {
            if (!silent)
                emit({Event::Kind::Failed, 0, tr("Could not parse version manifest.")});
            return;
        }
        if (onChecked)
            onChecked(base::nowSecs());
        if (m.version <= _current) {
            emit({Event::Kind::UpToDate, m.version, {}});
            return;
        }
        emit({Event::Kind::Available, m.version, {}});
        download(m.version, std::move(m.sha256));
    });
}

void Updater::download(int version, std::string sha256) {
    _busy = true;
    const int job =
        model::jobs().begin(arg(tr("Downloading %1"), "msga " + str::number(int64_t(version))));
    net::Request req;
    req.url                   = _assetUrl;
    req.timeoutMs             = kDownloadTimeoutMs;
    std::weak_ptr<char> alive = _alive;
    // Download progress: only when the server says the size.
    req.onProgress            = [this, alive, last = -1](int64_t got, int64_t total) mutable {
        if (alive.expired() || total <= 0)
            return;
        const int pct = int(got * 100 / total);
        if (pct == last)
            return;
        last = pct;
        Event e;
        e.kind    = Event::Kind::Progress;
        e.percent = pct;
        emit(std::move(e));
    };
    _client.send(std::move(req), [this, alive, job, sha256](net::Response r) {
        if (alive.expired()) {
            model::jobs().end(job);
            return;
        }
        if (!r.ok()) {
            model::jobs().end(job);
            _busy = false;
            const std::string why =
                r.error.empty() ? "HTTP " + str::number(int64_t(r.status)) : r.error;
            emit({Event::Kind::Failed, 0, arg(tr("Download failed: %1"), why)});
            return;
        }
        // Hash and write off the UI thread: tens of megabytes.
        auto body = std::make_shared<std::string>(std::move(r.body));
        auto err  = std::make_shared<std::string>();
        model::runInBackground(
            _app,
            [body, err, sha256, target = _target] {
                if (!checksumMatches(*body, sha256))
                    *err = tr("Downloaded update is corrupt (checksum mismatch).");
                else
                    *err = install(target, *body);
                body->clear();
                body->shrink_to_fit();
            },
            [this, alive, job, err] {
                model::jobs().end(job);
                if (alive.expired())
                    return;
                _busy = false;
                if (!err->empty()) {
                    LOG_WARN("update", "%s", err->c_str());
                    emit({Event::Kind::Failed, 0, *err});
                    return;
                }
                _ready      = true;
                _downloaded = _target;
                emit({Event::Kind::Ready, 0, {}});
            }
        );
    });
}

} // namespace update
