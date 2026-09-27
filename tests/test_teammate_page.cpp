// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
//
// Tests for the teammate page's folder menu ("Change folder"):
//   - lists the remembered folders merged with the folders existing sessions
//     work in, newest first, the current one checked, gone ones left out,
//     then "Browse…"
//   - picking one makes it the teammate's folder and the most recent pick
#include <catch2/catch_test_macros.hpp>

#include "test_main.h"

#include "session/session.h"
#include "stub_backend.h"
#include "ui/context_menu/context_menu.h"
#include "ui/styled_button/styled_button.h"
#include "ui/teammate_page/teammate_page.h"
#include "ui/theme_manager.h"
#include "util/recent_folders.h"

#include <QApplication>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

MSGA_TEST_MAIN(argc, argv) {
    QApplication app(argc, argv);
    app.setApplicationName("msga-test-teammate-page");
    app.setOrganizationName("msga-test");
    ThemeManager::instance();
    return msga_test::runCatch(argc, argv);
}

namespace {

constexpr auto kTeam = "T_TEAMMATE_PAGE_TEST";

// One Claude Code-like session, working in `folder`.
struct StubBackend : msga_test::StubBackendBase {
    QHash<QString, QString> folders; // conv id → cwd
    QString agentSessionFolder(ConversationId c) override { return folders.value(c.value); }
};

struct Fixture {
    QTemporaryDir            settingsDir;
    QTemporaryDir            work; // the folders
    StubBackend             *stub = nullptr;
    std::unique_ptr<Session> session;

    QString dir(const char *name) const { return QDir::cleanPath(work.filePath(name)); }

    Fixture() {
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsDir.path());
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cache/" + kTeam)
            .removeRecursively();
        for (const char *d : {"used", "session", "current"})
            REQUIRE(QDir(work.path()).mkpath(d));

        QSettings s("msga", "msga");
        s.clear();
        // "gone" was used last but no longer exists; "current" is the
        // teammate's folder though nothing ever picked it.
        RecentFolders::save(
            s, RecentFolders::kClaudeCodeKey, {{dir("gone"), 500}, {dir("used"), 100}}
        );
        s.setValue("claudeCode/lastDir/coder", dir("current"));
        s.sync();

        auto backend             = std::make_unique<StubBackend>();
        stub                     = backend.get();
        stub->caps.agentSessions = true;
        stub->_meId              = UserId{"U1"};
        Conversation c;
        c.id         = ConversationId{"S1"};
        c.kind       = ConvKind::Im;
        c.name       = "A session";
        c.dmUser     = UserId{"U2"};
        c.agentRole  = "coder";
        c.latestTs   = "300.000000"; // newer than "used" (100)
        stub->_convs = std::vector<Conversation>{c};
        stub->_users = std::vector<User>{
            {.id = UserId{"U1"}, .name = "me", .displayName = "Me"},
            {.id = UserId{"U2"}, .name = "session", .displayName = "A session"}
        };
        stub->folders.insert("S1", dir("session"));
        session = std::make_unique<Session>(std::move(backend), kTeam);
        session->start();
    }
    ~Fixture() { session.reset(); }
};

ContextMenu *openFolderMenu(TeammatePage &page) {
    StyledButton *btn = nullptr;
    for (auto *b : page.findChildren<StyledButton *>())
        if (b->text() == TeammatePage::tr("Change folder"))
            btn = b;
    REQUIRE(btn != nullptr);
    QTest::mouseClick(btn, Qt::LeftButton);
    QApplication::processEvents();
    auto *menu = page.findChild<ContextMenu *>();
    REQUIRE(menu != nullptr);
    REQUIRE(menu->isVisible());
    return menu;
}

// Row `i` of a menu of plain rows (ContextMenu's shadow 8 + padding 6, rows 36).
QPoint rowCenter(const ContextMenu *m, int i) {
    return {m->width() / 2, 8 + 6 + i * 36 + 18};
}

} // namespace

TEST_CASE(
    "Teammate page: the folder menu lists recent and session folders", "[teammate][folders]"
) {
    Fixture      f;
    TeammatePage page(nullptr);
    page.setSession(f.session.get());
    page.open(AgentRole{.id = "coder", .name = "Coder"});
    page.resize(700, 500);
    page.show();
    REQUIRE(page.folder() == f.dir("current"));

    ContextMenu *menu = openFolderMenu(page);
    // current (checked, never picked) · session (300) · used (100) — "gone"
    // left out — then a separator and "Browse…".
    CHECK(menu->height() == 2 * 8 + 2 * 6 + 4 * 36 + 9);
    menu->close();
}

TEST_CASE(
    "Teammate page: picking a folder makes it the default and most recent", "[teammate][folders]"
) {
    Fixture      f;
    TeammatePage page(nullptr);
    page.setSession(f.session.get());
    page.open(AgentRole{.id = "coder", .name = "Coder"});
    page.resize(700, 500);
    page.show();

    int changed = 0;
    QObject::connect(&page, &TeammatePage::folderChanged, [&] { ++changed; });
    ContextMenu *menu = openFolderMenu(page);
    const QPoint at   = rowCenter(menu, 1); // the session's folder
    QTest::mousePress(menu, Qt::LeftButton, Qt::NoModifier, at);
    QTest::mouseRelease(menu, Qt::LeftButton, Qt::NoModifier, at);
    QApplication::processEvents();

    CHECK(page.folder() == f.dir("session"));
    CHECK(changed == 1);
    QSettings s("msga", "msga");
    CHECK(s.value("claudeCode/lastDir/coder").toString() == f.dir("session"));
    const auto mru = RecentFolders::load(s, RecentFolders::kClaudeCodeKey);
    REQUIRE(!mru.empty());
    CHECK(mru.front().path == f.dir("session"));
}
