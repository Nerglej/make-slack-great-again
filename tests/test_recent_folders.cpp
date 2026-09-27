// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
//
// Tests for RecentFolders — the MRU behind the teammate page's folder menu:
//   - bumped(): moves to the front, de-duplicates by normalized path, caps
//   - rank(): merges remembered folders with existing sessions' folders,
//     newest first, counts sessions, drops folders that are gone
//   - load/save round trip through QSettings (shrinking list leaves no rows)
#include <catch2/catch_test_macros.hpp>

#include "util/recent_folders.h"

#include <QSettings>
#include <QTemporaryDir>

using namespace RecentFolders;

static QStringList paths(const std::vector<Entry> &l) {
    QStringList out;
    for (const auto &e : l)
        out << e.path;
    return out;
}

static QStringList paths(const std::vector<Choice> &l) {
    QStringList out;
    for (const auto &c : l)
        out << c.path;
    return out;
}

static const auto kAll = [](const QString &) { return true; };

// ── bumped ────────────────────────────────────────────────────────────────────

TEST_CASE("RecentFolders: a bump puts the folder first, stamped", "[recent_folders]") {
    auto l = bumped({}, "/a", 10);
    l      = bumped(l, "/b", 20);
    REQUIRE(paths(l) == QStringList{"/b", "/a"});
    CHECK(l[0].usedAt == 20);
    CHECK(l[1].usedAt == 10);
}

TEST_CASE("RecentFolders: bumping a known folder moves it, no duplicate", "[recent_folders]") {
    auto l = bumped(bumped(bumped({}, "/a", 1), "/b", 2), "/c", 3);
    l      = bumped(l, "/a/", 4); // trailing slash = the same folder
    REQUIRE(paths(l) == QStringList{"/a", "/c", "/b"});
    CHECK(l[0].usedAt == 4);
}

TEST_CASE("RecentFolders: the list is capped, oldest dropped", "[recent_folders]") {
    std::vector<Entry> l;
    for (int i = 0; i < 5; ++i)
        l = bumped(l, QString("/f%1").arg(i), i, 3);
    CHECK(paths(l) == QStringList{"/f4", "/f3", "/f2"});
}

TEST_CASE("RecentFolders: an empty path is not remembered", "[recent_folders]") {
    CHECK(bumped({}, "", 1).empty());
}

// ── rank ──────────────────────────────────────────────────────────────────────

TEST_CASE("RecentFolders: sessions' folders join the remembered ones", "[recent_folders]") {
    const std::vector<Entry>         mru{{"/a", 100}, {"/b", 50}};
    const std::vector<SessionFolder> sessions{{"/c", 70}, {"/b", 200}, {"/b/", 10}};
    const auto                       r = rank(mru, sessions, kAll);
    // /b's latest session (200) beats /a's use (100), which beats /c's session (70).
    REQUIRE(paths(r) == QStringList{"/b", "/a", "/c"});
    CHECK(r[0].lastUsed == 200);
    CHECK(r[0].sessions == 2);
    CHECK(r[1].sessions == 0);
    CHECK(r[2].sessions == 1);
}

TEST_CASE("RecentFolders: equal times keep the remembered order", "[recent_folders]") {
    const std::vector<Entry> mru{{"/x", 0}, {"/y", 0}, {"/z", 0}};
    CHECK(paths(rank(mru, {}, kAll)) == QStringList{"/x", "/y", "/z"});
}

TEST_CASE("RecentFolders: folders that are gone are left out", "[recent_folders]") {
    const std::vector<Entry>         mru{{"/gone", 100}, {"/here", 50}};
    const std::vector<SessionFolder> sessions{{"/also-gone", 300}};
    const auto r = rank(mru, sessions, [](const QString &p) { return p == "/here"; });
    CHECK(paths(r) == QStringList{"/here"});
}

TEST_CASE("RecentFolders: the menu is capped after ranking", "[recent_folders]") {
    const std::vector<Entry>         mru{{"/old", 1}, {"/mid", 5}};
    const std::vector<SessionFolder> sessions{{"/new", 9}};
    CHECK(paths(rank(mru, sessions, kAll, 2)) == QStringList{"/new", "/mid"});
}

// ── QSettings ─────────────────────────────────────────────────────────────────

TEST_CASE("RecentFolders: save and load round-trip", "[recent_folders]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    QSettings s(dir.filePath("t.ini"), QSettings::IniFormat);
    save(s, kClaudeCodeKey, {{"/a", 3}, {"/b", 2}, {"/c", 1}});
    save(s, kClaudeCodeKey, {{"/b", 5}}); // shrinking leaves no stale rows behind
    const auto l = load(s, kClaudeCodeKey);
    REQUIRE(paths(l) == QStringList{"/b"});
    CHECK(l[0].usedAt == 5);
}

TEST_CASE("RecentFolders: bump goes through the stored list", "[recent_folders]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    QSettings s(dir.filePath("t.ini"), QSettings::IniFormat);
    bump(s, kClaudeCodeKey, "/a");
    bump(s, kClaudeCodeKey, "/b");
    bump(s, kClaudeCodeKey, "/a");
    const auto l = load(s, kClaudeCodeKey);
    REQUIRE(paths(l) == QStringList{"/a", "/b"});
    CHECK(l[0].usedAt >= l[1].usedAt);
    CHECK(l[0].usedAt > 0);
}
