// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "recent_folders.h"

#include <QDateTime>
#include <QDir>
#include <QHash>
#include <QSettings>
#include <algorithm>

namespace RecentFolders {

QString normalized(const QString &path) {
    return path.isEmpty() ? QString() : QDir::cleanPath(QDir::fromNativeSeparators(path));
}

std::vector<Entry> bumped(std::vector<Entry> list, const QString &path, qint64 now, int cap) {
    const QString p = normalized(path);
    if (p.isEmpty())
        return list;
    std::erase_if(list, [&](const Entry &e) { return normalized(e.path) == p; });
    list.insert(list.begin(), Entry{p, now});
    if (cap >= 0 && static_cast<int>(list.size()) > cap)
        list.resize(cap);
    return list;
}

std::vector<Choice> rank(
    const std::vector<Entry>                   &remembered,
    const std::vector<SessionFolder>           &sessions,
    const std::function<bool(const QString &)> &exists,
    int                                         cap
) {
    std::vector<Choice> out;
    QHash<QString, int> at; // path → index in out
    // Index of `raw`'s row in out (added on first sight), -1 for no path.
    auto                slot = [&](const QString &raw) {
        const QString p = normalized(raw);
        if (p.isEmpty())
            return -1;
        if (const auto it = at.constFind(p); it != at.cend())
            return *it;
        at.insert(p, static_cast<int>(out.size()));
        out.push_back(Choice{p});
        return static_cast<int>(out.size()) - 1;
    };
    for (const Entry &e : remembered)
        if (const int i = slot(e.path); i >= 0)
            out[i].lastUsed = std::max(out[i].lastUsed, e.usedAt);
    for (const SessionFolder &f : sessions)
        if (const int i = slot(f.path); i >= 0) {
            out[i].lastUsed = std::max(out[i].lastUsed, f.activity);
            ++out[i].sessions;
        }
    if (exists)
        std::erase_if(out, [&](const Choice &c) { return !exists(c.path); });
    // Stable: equal times keep the remembered (MRU) order, then first-seen.
    std::stable_sort(out.begin(), out.end(), [](const Choice &a, const Choice &b) {
        return a.lastUsed > b.lastUsed;
    });
    if (cap >= 0 && static_cast<int>(out.size()) > cap)
        out.resize(cap);
    return out;
}

std::vector<Entry> load(QSettings &s, const QString &key) {
    std::vector<Entry> list;
    const int          n = s.beginReadArray(key);
    for (int i = 0; i < n; ++i) {
        s.setArrayIndex(i);
        Entry e{
            normalized(s.value(QStringLiteral("path")).toString()),
            s.value(QStringLiteral("usedAt")).toLongLong()
        };
        if (!e.path.isEmpty())
            list.push_back(std::move(e));
    }
    s.endArray();
    return list;
}

void save(QSettings &s, const QString &key, const std::vector<Entry> &list) {
    s.remove(key); // drop the old array's extra rows
    s.beginWriteArray(key, static_cast<int>(list.size()));
    for (int i = 0; i < static_cast<int>(list.size()); ++i) {
        s.setArrayIndex(i);
        s.setValue(QStringLiteral("path"), list[i].path);
        s.setValue(QStringLiteral("usedAt"), list[i].usedAt);
    }
    s.endArray();
}

void bump(QSettings &s, const QString &key, const QString &path) {
    save(s, key, bumped(load(s, key), path, QDateTime::currentSecsSinceEpoch()));
}

QString teammateFolderKey(const QString &role) {
    return QStringLiteral("claudeCode/lastDir/") + role;
}

QString teammateFolder(QSettings &s, const QString &role) {
    const QString any = s.value(QStringLiteral("claudeCode/lastDir"), QDir::homePath()).toString();
    return s.value(teammateFolderKey(role), any).toString();
}

void noteSessionStarted(QSettings &s, const QString &dir) {
    s.setValue(QStringLiteral("claudeCode/lastDir"), dir);
    bump(s, QString::fromLatin1(kClaudeCodeKey), dir);
}

} // namespace RecentFolders
