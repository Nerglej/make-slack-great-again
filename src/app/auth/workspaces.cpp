#include "app/auth/workspaces.h"

#include "app/identity.h"
#include "base/file.h"
#include "base/json.h"
#include "base/log.h"
#include "base/secret.h"
#include "plat/plat.h"

namespace auth {

namespace {

std::string secretKey(const WorkspaceRecord &r) {
    return "workspace/" + r.key() + "/auth";
}

} // namespace

WorkspaceStore::WorkspaceStore(std::string path) : _path(std::move(path)) {
    std::string text;
    if (_path.empty() || !file::readAll(_path, &text))
        return;
    json::Document doc;
    std::string    err;
    if (!doc.parse(std::move(text), &err)) {
        LOG_WARN("auth", "%s: %s", _path.c_str(), err.c_str());
        return;
    }
    const json::Value root = doc.root();
    for (const json::Value w : root["workspaces"]) {
        WorkspaceRecord r;
        r.service     = std::string(w["service"].str());
        r.id          = std::string(w["id"].str());
        r.displayName = std::string(w["displayName"].str());
        r.iconUrl     = std::string(w["iconUrl"].str());
        r.auth        = std::string(w["auth"].str());
        r.muted       = w["muted"].boolean();
        if (r.service.empty() || r.id.empty() || find(r.key()))
            continue;
        if (r.auth.empty() && secret::available()) {
            r.auth               = secret::read(secretKey(r));
            _inKeychain[r.key()] = r.auth;
        }
        _records.push_back(std::move(r));
    }
    _active = std::string(root["active"].str());
    if (!find(_active))
        _active = _records.empty() ? std::string() : _records.front().key();
}

const WorkspaceRecord *WorkspaceStore::find(std::string_view key) const {
    for (const auto &r : _records)
        if (r.key() == key)
            return &r;
    return nullptr;
}

void WorkspaceStore::save(WorkspaceRecord rec) {
    for (auto &r : _records)
        if (r.key() == rec.key()) {
            rec.muted = rec.muted || r.muted;
            r         = std::move(rec);
            flush();
            return;
        }
    _records.push_back(std::move(rec));
    if (_active.empty())
        _active = _records.back().key();
    flush();
}

void WorkspaceStore::remove(std::string_view key) {
    for (size_t i = 0; i < _records.size(); ++i)
        if (_records[i].key() == key) {
            if (secret::available()) {
                secret::remove(secretKey(_records[i]));
                _inKeychain.erase(_records[i].key());
            }
            _records.erase(_records.begin() + i);
            break;
        }
    if (!find(_active))
        _active = _records.empty() ? std::string() : _records.front().key();
    flush();
}

void WorkspaceStore::setMuted(std::string_view key, bool muted) {
    for (auto &r : _records)
        if (r.key() == key && r.muted != muted) {
            r.muted = muted;
            flush();
        }
}

void WorkspaceStore::setOrder(const std::vector<std::string> &keys) {
    std::vector<WorkspaceRecord> next;
    next.reserve(_records.size());
    for (const auto &k : keys)
        for (auto &r : _records)
            if (!r.service.empty() && r.key() == k) {
                next.push_back(std::move(r));
                r.service.clear(); // taken
                break;
            }
    for (auto &r : _records)
        if (!r.service.empty())
            next.push_back(std::move(r));
    _records = std::move(next);
    flush();
}

void WorkspaceStore::importRecords(std::vector<WorkspaceRecord> records, std::string_view active) {
    _records.clear();
    _inKeychain.clear();
    for (WorkspaceRecord &r : records) {
        if (r.service.empty() || r.id.empty() || find(r.key()))
            continue;
        if (r.auth.empty() && secret::available()) {
            r.auth               = secret::read(secretKey(r));
            _inKeychain[r.key()] = r.auth;
        }
        _records.push_back(std::move(r));
    }
    _active = std::string(active);
    if (!find(_active))
        _active = _records.empty() ? std::string() : _records.front().key();
    flush();
}

void WorkspaceStore::setActive(std::string_view key) {
    if (!find(key) || _active == key)
        return;
    _active = std::string(key);
    flush();
}

bool WorkspaceStore::flush() {
    if (_path.empty())
        return false;
    json::Writer w(true);
    w.beginObject().key("active").value(_active).key("workspaces").beginArray();
    for (const auto &r : _records) {
        w.beginObject();
        w.key("service").value(r.service).key("id").value(r.id);
        w.key("displayName").value(r.displayName).key("iconUrl").value(r.iconUrl);
        if (r.muted)
            w.key("muted").value(true);
        // The keychain where there is one; the file when it refuses. Only a
        // changed blob is written (each keychain write may raise an OS prompt).
        bool inKeychain = false;
        if (secret::available()) {
            auto it    = _inKeychain.find(r.key());
            inKeychain = it != _inKeychain.end() && it->second == r.auth;
            if (!inKeychain && secret::write(secretKey(r), r.auth)) {
                _inKeychain[r.key()] = r.auth;
                inKeychain           = true;
            }
        }
        if (!inKeychain)
            w.key("auth").value(r.auth);
        w.endObject();
    }
    w.endArray().endObject();
    if (!file::writeAtomic(_path, w.str(), 0600)) {
        LOG_WARN("auth", "could not write %s", _path.c_str());
        return false;
    }
    return true;
}

std::string WorkspaceStore::defaultPath(plat::App &app) {
    const std::string dir = identity::configDir(app);
    return dir.empty() ? std::string() : file::join(dir, "workspaces.json");
}

} // namespace auth
