// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QString>
#include <QtGlobal>
#include <functional>
#include <vector>

class QSettings;

// Recently used folders, most recent first — the quick picks for where a new
// Claude Code session starts (the teammate page's folder menu). One list for
// every teammate, kept in QSettings under kClaudeCodeKey. The pure parts
// (bumped/rank) are what the tests pin; load/save/bump are the QSettings glue.
namespace RecentFolders {

inline constexpr int  kMax             = 10;
inline constexpr char kClaudeCodeKey[] = "claudeCode/recentDirs";

struct Entry {
    QString path;
    qint64  usedAt = 0; // epoch seconds
};

// A folder existing sessions work in, with the session's latest activity.
struct SessionFolder {
    QString path;
    qint64  activity = 0; // epoch seconds, 0 = unknown
};

// One pick in the menu.
struct Choice {
    QString path;
    qint64  lastUsed = 0; // latest of its own use and its sessions' activity
    int     sessions = 0; // existing sessions working there
};

// The one spelling a folder is compared and stored by (no trailing slash,
// no "." / ".." hops).
QString normalized(const QString &path);

// `list` with `path` moved (or added) to the front, stamped `now`, capped.
std::vector<Entry> bumped(std::vector<Entry> list, const QString &path, qint64 now, int cap = kMax);

// The menu: the remembered folders merged with the ones existing sessions
// work in, most recently used first (a tie keeps the remembered order), those
// `exists` rejects left out, at most `cap`.
std::vector<Choice> rank(
    const std::vector<Entry>                   &remembered,
    const std::vector<SessionFolder>           &sessions,
    const std::function<bool(const QString &)> &exists,
    int                                         cap = kMax
);

std::vector<Entry> load(QSettings &s, const QString &key);
void               save(QSettings &s, const QString &key, const std::vector<Entry> &list);
// load + bumped(now) + save.
void               bump(QSettings &s, const QString &key, const QString &path);

// Where a new session with teammate `role` (AgentRole::id) starts: the folder
// last picked for it (teammateFolderKey), else the last one any session
// started in ("claudeCode/lastDir"), else home.
QString teammateFolderKey(const QString &role);
QString teammateFolder(QSettings &s, const QString &role);
// A session just started in `dir`: the last folder for all of them, and first
// among the recent ones (kClaudeCodeKey).
void    noteSessionStarted(QSettings &s, const QString &dir);

} // namespace RecentFolders
