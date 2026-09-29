// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
#include <catch2/catch_test_macros.hpp>

#include "test_main.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QUrl>
#include <memory>

#include "session/session.h"
#include "stub_backend.h"
#include "text/mrkdwn_parser.h"
#include "ui/conv_selector/conv_selector_widget.h"
#include "ui/dropdown/dropdown.h"
#include "ui/forward_dialog/forward_dialog.h"
#include "ui/message_list/file_chip_widget.h"

MSGA_TEST_MAIN(argc, argv) {
    QApplication app(argc, argv);
    app.setApplicationName("msga-test-forward-dialog");
    app.setOrganizationName("msga-test");
    // Teammate folders live in QSettings("msga", "msga"): keep them off the real one.
    QTemporaryDir settingsDir;
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsDir.path());
    return msga_test::runCatch(argc, argv);
}

namespace {

void copyLink(ForwardDialog &dialog) {
    for (auto *button : dialog.findChildren<QPushButton *>()) {
        if (button->text() == ForwardDialog::tr("Copy Link")) {
            button->click();
            return;
        }
    }
    FAIL("Copy Link button is missing");
}

Message linkedMessage(const QString &url) {
    Message message;
    message.text = MrkdwnParser::parse("<" + url + "|original link>");
    return message;
}

} // namespace

TEST_CASE("forward copy link retains the original message data", "[forward_dialog][clipboard]") {
    QString url;
    SECTION("ordinary link") {
        url = "https://example.com/original";
    }
    SECTION("Slack message link") {
        url = "https://team.slack.com/archives/C123/p1700000000000100";
    }
    auto          message = linkedMessage(url);
    ForwardDialog dialog(message, nullptr);
    // The caller can reuse its message after constructing the modeless dialog.
    message = linkedMessage("https://example.com/replacement");
    QApplication::clipboard()->setText("before copy");
    copyLink(dialog);
    CHECK(QApplication::clipboard()->text() == url);
}

TEST_CASE("forward copy link outlives the source message", "[forward_dialog][clipboard]") {
    const QString                  url = "https://example.com/original";
    std::unique_ptr<ForwardDialog> dialog;
    {
        const auto message = linkedMessage(url);
        dialog             = std::make_unique<ForwardDialog>(message, nullptr);
    }
    QApplication::clipboard()->setText("before copy");
    copyLink(*dialog);
    CHECK(QApplication::clipboard()->text() == url);
}

// ── Cross-workspace targets ─────────────────────────────────────────────────

namespace {

void wipeTeamCache(const QString &teamId) {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cache/" + teamId)
        .removeRecursively();
}

std::unique_ptr<Session> sessionWithChannel(const QString &teamId, const QString &channel) {
    wipeTeamCache(teamId);
    auto        *stub = new msga_test::StubBackendBase;
    Conversation c;
    c.id         = ConversationId{"C_" + channel};
    c.name       = channel;
    c.kind       = ConvKind::PublicChannel;
    stub->_convs = std::vector<Conversation>{c};
    auto session = std::make_unique<Session>(std::unique_ptr<Backend>(stub), teamId);
    session->start();
    return session;
}

QLineEdit *convSearch(ForwardDialog &dialog) {
    auto *selector = dialog.findChild<ConvSelectorWidget *>();
    return selector ? selector->findChild<QLineEdit *>() : nullptr;
}

// Type into the conversation search and return the labels it offers.
QStringList offeredConversations(ForwardDialog &dialog, const QString &query) {
    auto *edit = convSearch(dialog);
    REQUIRE(edit);
    edit->clear();
    edit->setText(query);
    QStringList labels;
    for (auto *list : dialog.window()->findChildren<QListWidget *>())
        for (int i = 0; i < list->count(); ++i)
            labels << list->item(i)->text();
    return labels;
}

QPushButton *forwardButton(ForwardDialog &dialog) {
    for (auto *button : dialog.findChildren<QPushButton *>())
        if (button->text() == ForwardDialog::tr("Forward"))
            return button;
    return nullptr;
}

} // namespace

TEST_CASE("forward stays in its workspace without a choice", "[forward_dialog][workspaces]") {
    auto    source = sessionWithChannel("T_FWD_A", "general");
    Message message;
    SECTION("no workspaces") {
        ForwardDialog dialog(message, source.get());
        CHECK_FALSE(dialog.findChild<Dropdown *>());
        CHECK(dialog.targetSession() == source.get());
    }
    SECTION("only its own workspace") {
        ForwardDialog dialog(message, source.get(), {{source.get(), "A"}});
        CHECK_FALSE(dialog.findChild<Dropdown *>());
        CHECK(dialog.targetSession() == source.get());
    }
}

TEST_CASE("forward picks conversations from the chosen workspace", "[forward_dialog][workspaces]") {
    auto    agents = sessionWithChannel("T_FWD_CC", "agents");
    auto    team   = sessionWithChannel("T_FWD_TEAM", "general");
    Message message;
    message.text = MrkdwnParser::parse("done");

    ForwardDialog dialog(message, agents.get(), {{team.get(), "Team"}, {agents.get(), "Agents"}});
    CHECK(dialog.usesSession(team.get())); // the host must close it if Team goes away
    CHECK(dialog.usesSession(agents.get()));
    auto *picker = dialog.findChild<Dropdown *>();
    REQUIRE(picker);
    REQUIRE(forwardButton(dialog));
    CHECK(picker->currentText() == "Agents"); // starts on the message's own workspace
    CHECK(dialog.targetSession() == agents.get());
    CHECK(offeredConversations(dialog, "a") == QStringList{"#agents"});

    // Pick a conversation, then switch workspace: the pick goes with it.
    emit convSearch(dialog)->returnPressed();
    CHECK(dialog.target() == ChatTarget::conversation(ConversationId{"C_agents"}));
    CHECK(forwardButton(dialog)->isEnabled());

    picker->setCurrentIndex(0);
    CHECK(dialog.targetSession() == team.get());
    CHECK(dialog.target().isEmpty());
    CHECK_FALSE(forwardButton(dialog)->isEnabled());
    CHECK(offeredConversations(dialog, "e") == QStringList{"#general"});

    emit convSearch(dialog)->returnPressed();
    CHECK(dialog.target() == ChatTarget::conversation(ConversationId{"C_general"}));
    CHECK(forwardButton(dialog)->isEnabled());
}

TEST_CASE(
    "a chat message offers every workspace and goes portable", "[forward_dialog][workspaces]"
) {
    // A Slack-shaped source among three workspaces, listed in the middle.
    auto    other = sessionWithChannel("T_FWD_OTHER", "random");
    auto    slack = sessionWithChannel("T_FWD_SLACK", "general");
    auto    mail  = sessionWithChannel("T_FWD_MAIL", "inbox");
    Message message;
    message.rawText = "*ship* it <!here>, see <#C_general> &amp; <https://a.example|the notes>";
    message.text    = MrkdwnParser::parse(message.rawText);

    ForwardDialog dialog(
        message, slack.get(), {{other.get(), "Other"}, {slack.get(), "Slack"}, {mail.get(), "Mail"}}
    );
    auto *picker = dialog.findChild<Dropdown *>();
    REQUIRE(picker);
    CHECK(picker->currentText() == "Slack");
    CHECK(dialog.targetSession() == slack.get());
    for (auto *s : {other.get(), slack.get(), mail.get()})
        CHECK(dialog.usesSession(s));

    picker->setCurrentIndex(0);
    CHECK(picker->currentText() == "Other");
    CHECK(dialog.targetSession() == other.get());
    picker->setCurrentIndex(2);
    CHECK(picker->currentText() == "Mail");
    CHECK(dialog.targetSession() == mail.get());
    CHECK(offeredConversations(dialog, "i") == QStringList{"#inbox"});

    // Its own workspace gets the mrkdwn as is; another one the words it reads as.
    CHECK(forwardedText(message, slack.get(), /*verbatim=*/true) == message.rawText);
    CHECK(
        forwardedText(message, slack.get(), /*verbatim=*/false) ==
        "**ship** it @here, see #general & [the notes](https://a.example)"
    );
}

TEST_CASE("a portable rawText forwards verbatim", "[forward_dialog][workspaces]") {
    // Claude Code: plain markdown, nothing workspace-local — sent bit for bit.
    Message message;
    message.rawText = "**Done.** Changed `a.cpp`:\n- one\n- two\n\n```cpp\nint x;\n```";
    message.text    = MrkdwnParser::parse(message.rawText);
    CHECK(forwardedText(message, nullptr, /*verbatim=*/true) == message.rawText);
    // No rawText (email): the plain text.
    Message mail;
    mail.text.text = "Hi,\nsee below";
    CHECK(forwardedText(mail, nullptr, /*verbatim=*/true) == mail.text.text);
    CHECK(forwardedText(mail, nullptr, /*verbatim=*/false) == mail.text.text);
}

// ── Chats that start on the way ─────────────────────────────────────────────

namespace {

User person(const QString &id, const QString &name, const QString &display) {
    User u;
    u.id          = UserId{id};
    u.name        = name;
    u.displayName = display;
    return u;
}

// A workspace that opens DMs and starts sessions on request, and says so.
struct ChatBackend : msga_test::StubBackendBase {
    QStringList dmsOpened;
    QStringList sessionsStarted; // "role in dir"
    QString     blocker;
    QString     openError;

    void openDm(
        UserId user, std::function<void(ConversationId)> ok, std::function<void(QString)> err
    ) override {
        dmsOpened << user.value;
        if (!openError.isEmpty())
            err(openError);
        else
            ok(ConversationId{"D_" + user.value});
    }
    void startAgentSession(
        const QString &dir,
        bool,
        const QString                      &role,
        std::function<void(ConversationId)> ok,
        std::function<void(QString)>
    ) override {
        sessionsStarted << role + " in " + dir;
        ok(ConversationId{"S_" + role});
    }
    QString                agentSessionBlocker(const QString &) override { return blocker; }
    std::vector<AgentRole> agentRoles() override {
        AgentRole engineer;
        engineer.id   = "engineer";
        engineer.name = "Engineer";
        engineer.user = UserId{"R_engineer"};
        AgentRole designer;
        designer.id   = "designer";
        designer.name = "Designer";
        designer.user = UserId{"R_designer"};
        return {engineer, designer};
    }
};

// A Slack-shaped team: #general, a DM with Alice, and a roster of whom only
// Bob (and Alice, through her DM) can be written to.
std::unique_ptr<Session> peopleSession(const QString &teamId, ChatBackend **out, bool openDm) {
    wipeTeamCache(teamId);
    auto *stub        = new ChatBackend;
    stub->caps.openDm = openDm;
    stub->_meId       = UserId{"U_me"};
    Conversation general;
    general.id   = ConversationId{"C_general"};
    general.name = "general";
    general.kind = ConvKind::PublicChannel;
    Conversation alice;
    alice.id     = ConversationId{"D_alice"};
    alice.name   = "U_alice";
    alice.kind   = ConvKind::Im;
    alice.dmUser = UserId{"U_alice"};
    stub->_convs = std::vector<Conversation>{general, alice};

    User stranger       = person("U_carol", "carol", "Carol");
    stranger.isStranger = true;
    User gone           = person("U_dave", "dave", "Dave");
    gone.isDeactivated  = true;
    User bot            = person("U_bot", "buildbot", "Buildbot");
    bot.isBot           = true;
    stub->_users        = std::vector<User>{
        person("U_me", "me", "Me Myself"),
        person("U_alice", "alice", "Alice"),
        person("U_bob", "bobby", "Bob"),
        stranger,
        gone,
        bot,
    };
    auto session = std::make_unique<Session>(std::unique_ptr<Backend>(stub), teamId);
    session->start();
    if (out)
        *out = stub;
    return session;
}

std::unique_ptr<Session> agentSession(const QString &teamId, ChatBackend **out) {
    wipeTeamCache(teamId);
    auto *stub               = new ChatBackend;
    stub->caps.agentSessions = true;
    Conversation run;
    run.id       = ConversationId{"S_old"};
    run.name     = "Fix the build";
    run.kind     = ConvKind::Im;
    run.dmUser   = UserId{"U_run"};
    stub->_convs = std::vector<Conversation>{run};
    // The teammates are listed with the users too; they are offered as teammates.
    stub->_users = std::vector<User>{
        person("U_run", "run", "Fix the build"),
        person("R_engineer", "engineer", "Engineer"),
    };
    auto session = std::make_unique<Session>(std::unique_ptr<Backend>(stub), teamId);
    session->start();
    if (out)
        *out = stub;
    return session;
}

} // namespace

TEST_CASE("forward offers people with no DM yet", "[forward_dialog][targets]") {
    auto          team = peopleSession("T_FWD_PEOPLE", nullptr, /*openDm=*/true);
    Message       message;
    ForwardDialog dialog(message, team.get());

    // Conversations first, then everyone else one can write to: no strangers,
    // no deactivated people, no bots, not ourselves, no second row for Alice.
    CHECK(offeredConversations(dialog, "@") == QStringList{"Alice", "Bob"});
    CHECK(offeredConversations(dialog, "") == QStringList{"#general", "Alice", "Bob"});
    CHECK(offeredConversations(dialog, "#") == QStringList{"#general"});
    CHECK(offeredConversations(dialog, "b") == QStringList{"Bob"});
    // Found by username too, listed by name.
    CHECK(offeredConversations(dialog, "@bobby") == QStringList{"Bob"});

    emit convSearch(dialog)->returnPressed();
    CHECK(dialog.target() == ChatTarget::person(UserId{"U_bob"}));
    REQUIRE(forwardButton(dialog));
    CHECK(forwardButton(dialog)->isEnabled());
}

TEST_CASE("forward offers no new DMs where none can start", "[forward_dialog][targets]") {
    // Email, Claude Code: a DM exists or it doesn't.
    auto          mail = peopleSession("T_FWD_NODM", nullptr, /*openDm=*/false);
    Message       message;
    ForwardDialog dialog(message, mail.get());
    CHECK(offeredConversations(dialog, "@") == QStringList{"Alice"});
    CHECK(offeredConversations(dialog, "b").isEmpty());
}

TEST_CASE("a people list stays bounded", "[forward_dialog][targets]") {
    wipeTeamCache("T_FWD_ROSTER");
    auto *stub        = new ChatBackend;
    stub->caps.openDm = true;
    std::vector<User> roster;
    for (int i = 0; i < 500; ++i)
        roster.push_back(person(
            QString("U%1").arg(i),
            QString("user%1").arg(i),
            QString("User %1").arg(i, 3, 10, QChar('0'))
        ));
    stub->_users = roster;
    Session session(std::unique_ptr<Backend>(stub), "T_FWD_ROSTER");
    session.start();
    Message           message;
    ForwardDialog     dialog(message, &session);
    const QStringList offered = offeredConversations(dialog, "user");
    CHECK(offered.size() == ConvSelectorWidget::kMaxPeopleRows);
    CHECK(offered.front() == "User 000");
}

TEST_CASE("forward offers an agent workspace's teammates", "[forward_dialog][targets]") {
    auto          agents = agentSession("T_FWD_AGENTS", nullptr);
    auto          team   = peopleSession("T_FWD_HUMANS", nullptr, /*openDm=*/true);
    Message       message;
    ForwardDialog dialog(message, team.get(), {{team.get(), "Team"}, {agents.get(), "Agents"}});
    auto         *picker = dialog.findChild<Dropdown *>();
    REQUIRE(picker);

    // Pick a person, then switch workspace: the pick goes with it.
    offeredConversations(dialog, "bob");
    emit convSearch(dialog)->returnPressed();
    CHECK(dialog.target() == ChatTarget::person(UserId{"U_bob"}));
    picker->setCurrentIndex(1);
    CHECK(dialog.target().isEmpty());
    CHECK_FALSE(forwardButton(dialog)->isEnabled());

    // Sessions first, then the teammates (not their user rows).
    CHECK(
        offeredConversations(dialog, "@") == QStringList{"Fix the build", "Engineer", "Designer"}
    );
    CHECK(offeredConversations(dialog, "") == QStringList{"Fix the build", "Engineer", "Designer"});
    CHECK(offeredConversations(dialog, "#").isEmpty());
    CHECK(offeredConversations(dialog, "eng") == QStringList{"Engineer"});
    emit convSearch(dialog)->returnPressed();
    CHECK(dialog.target() == ChatTarget::teammate("engineer"));
    CHECK(forwardButton(dialog)->isEnabled());
}

TEST_CASE("a forward target opens the chat it stands for", "[forward_dialog][targets]") {
    ChatBackend *stub = nullptr;
    auto         team = peopleSession("T_FWD_OPEN", &stub, /*openDm=*/true);
    QStringList  opened, errors;
    const auto   open = [&](Session *s, const ChatTarget &t) {
        openForwardTarget(
            s, t, [&](ConversationId id) { opened << id.value; }, [&](QString e) { errors << e; }
        );
    };

    SECTION("a conversation is already there") {
        open(team.get(), ChatTarget::conversation(ConversationId{"C_general"}));
        CHECK(opened == QStringList{"C_general"});
        CHECK(stub->dmsOpened.isEmpty());
    }
    SECTION("a person gets a DM") {
        open(team.get(), ChatTarget::person(UserId{"U_bob"}));
        CHECK(opened == QStringList{"D_U_bob"});
        CHECK(stub->dmsOpened == QStringList{"U_bob"});
        CHECK(errors.isEmpty());
    }
    SECTION("a DM that won't open says so") {
        stub->openError = "user_not_found";
        open(team.get(), ChatTarget::person(UserId{"U_bob"}));
        CHECK(opened.isEmpty());
        CHECK(errors.size() == 1);
    }
    SECTION("nothing opens where no DM can start") {
        ChatBackend *mailStub = nullptr;
        auto         mail     = peopleSession("T_FWD_OPEN_MAIL", &mailStub, /*openDm=*/false);
        open(mail.get(), ChatTarget::person(UserId{"U_bob"}));
        CHECK(opened.isEmpty());
        CHECK(errors.size() == 1);
        CHECK(mailStub->dmsOpened.isEmpty());
        // An existing one still does.
        open(mail.get(), ChatTarget::person(UserId{"U_alice"}));
        CHECK(opened == QStringList{"D_alice"});
    }
}

TEST_CASE("a teammate target starts a session in its folder", "[forward_dialog][targets]") {
    ChatBackend  *stub   = nullptr;
    auto          agents = agentSession("T_FWD_START", &stub);
    QTemporaryDir any, own;
    {
        QSettings s("msga", "msga");
        s.clear();
        s.setValue("claudeCode/lastDir", any.path());
        s.setValue("claudeCode/lastDir/engineer", own.path());
    }
    QStringList opened, errors;
    const auto  open = [&](const ChatTarget &t) {
        openForwardTarget(
            agents.get(),
            t,
            [&](ConversationId id) { opened << id.value; },
            [&](QString e) { errors << e; }
        );
    };

    SECTION("its own folder, else the last one") {
        open(ChatTarget::teammate("designer"));
        open(ChatTarget::teammate("engineer"));
        CHECK(opened == QStringList{"S_designer", "S_engineer"});
        CHECK(
            stub->sessionsStarted ==
            QStringList{"designer in " + any.path(), "engineer in " + own.path()}
        );
        // The folder a session last started in is the next default for all.
        open(ChatTarget::teammate("designer"));
        CHECK(stub->sessionsStarted.last() == "designer in " + own.path());
    }
    SECTION("a folder no session can start in") {
        stub->blocker = "Not a folder Claude Code can work in.";
        open(ChatTarget::teammate("engineer"));
        CHECK(opened.isEmpty());
        CHECK(errors == QStringList{stub->blocker});
        CHECK(stub->sessionsStarted.isEmpty());
    }
}

// ── Files ───────────────────────────────────────────────────────────────────

namespace {

File file(const QString &id, const QString &name) {
    File f;
    f.id         = id;
    f.name       = name;
    f.mimeType   = "text/csv";
    f.urlPrivate = "https://files.example.com/" + id + "/" + name;
    f.permalink  = "https://team.example.com/files/" + id;
    return f;
}

Message messageWithFiles() {
    Message m;
    m.ts    = "100.200";
    m.text  = MrkdwnParser::parse("Both are done, the list is attached");
    m.files = {file("F1", "stations.csv"), file("F2", "notes.txt")};
    return m;
}

bool hasLabel(ForwardDialog &dialog, const QString &text) {
    for (auto *label : dialog.findChildren<QLabel *>())
        if (label->text() == text)
            return true;
    return false;
}

// Serves every download from `bytes` (keyed by URL) or fails it.
struct DownloadBackend : msga_test::StubBackendBase {
    QHash<QString, QByteArray> bytes;
    QStringList                requested;
    void                       downloadFile(
        const QString                  &url,
        std::function<void(QByteArray)> onData,
        std::function<void(QString)>    onError
    ) override {
        requested << url;
        if (bytes.contains(url))
            onData(bytes.value(url));
        else if (onError)
            onError("not_found");
    }
};

} // namespace

TEST_CASE("forwarding a message takes its text and every file", "[forward_dialog][files]") {
    const auto    message = messageWithFiles();
    ForwardDialog dialog(message, nullptr);
    CHECK(hasLabel(dialog, ForwardDialog::tr("Forward this message")));
    CHECK_FALSE(dialog.fileOnly());
    REQUIRE(dialog.files().size() == 2);
    CHECK(dialog.findChildren<FileChipWidget *>().size() == 2);
    auto *preview = dialog.findChild<QTextBrowser *>();
    REQUIRE(preview);
    CHECK_FALSE(preview->isHidden());
}

TEST_CASE("sharing a file forwards that file alone", "[forward_dialog][files]") {
    const auto    message = messageWithFiles();
    ForwardDialog dialog(message, nullptr, {}, message.files[1]);
    CHECK(hasLabel(dialog, ForwardDialog::tr("Forward this file")));
    CHECK(dialog.fileOnly());
    REQUIRE(dialog.files().size() == 1);
    CHECK(dialog.files().front().id == "F2");
    // Only the file is previewed — the message text stays behind.
    CHECK(dialog.findChildren<FileChipWidget *>().size() == 1);
    auto *preview = dialog.findChild<QTextBrowser *>();
    REQUIRE(preview);
    CHECK(preview->isHidden());
    // Copy Link copies the file's link, not one from the message.
    QApplication::clipboard()->setText("before copy");
    copyLink(dialog);
    CHECK(QApplication::clipboard()->text() == "https://team.example.com/files/F2");
}

TEST_CASE("forwarded files are fetched for re-upload", "[forward_dialog][files]") {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cache/forward")
        .removeRecursively();
    auto *stub    = new DownloadBackend;
    auto  session = std::make_unique<Session>(std::unique_ptr<Backend>(stub), "T_FWD_FILES");

    const File remote = file("F1", "stations.csv");
    stub->bytes.insert(remote.urlPrivate, "uid,name\n1,Berlin\n");

    QTemporaryDir tmp;
    const QString localPath = tmp.filePath("local.txt");
    {
        QFile f(localPath);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("on disk");
    }
    File local       = file("F2", "local.txt");
    local.urlPrivate = QUrl::fromLocalFile(localPath).toString();

    File canvas     = file("F3", "Notes");
    canvas.mimeType = "application/vnd.slack-docs";

    int         calls = 0;
    QStringList paths, links;
    QString     error;
    fetchForwardedFiles(
        session.get(), {remote, local, canvas}, [&](QStringList p, QStringList l, QString e) {
            ++calls;
            paths = p;
            links = l;
            error = e;
        }
    );
    CHECK(calls == 1);
    CHECK(error.isEmpty());
    REQUIRE(paths.size() == 2);
    // The download keeps its name (the upload is called that) and its bytes.
    CHECK(QFileInfo(paths[0]).fileName() == "stations.csv");
    QFile downloaded(paths[0]);
    REQUIRE(downloaded.open(QIODevice::ReadOnly));
    CHECK(downloaded.readAll() == "uid,name\n1,Berlin\n");
    // A file already on disk goes as is; a canvas has no bytes and goes as a link.
    CHECK(paths[1] == localPath);
    CHECK(links == QStringList{"https://team.example.com/files/F3"});
    CHECK(stub->requested == QStringList{remote.urlPrivate});

    SECTION("audio forwards the original upload, not Slack's transcode") {
        File audio               = file("F4", "memo.mp3");
        audio.urlPrivateDownload = "https://files.example.com/F4/download/memo.mp3";
        stub->bytes.insert(audio.urlPrivateDownload, "ID3");
        fetchForwardedFiles(session.get(), {audio}, [&](QStringList p, QStringList, QString e) {
            paths = p;
            error = e;
        });
        CHECK(error.isEmpty());
        CHECK(stub->requested.last() == audio.urlPrivateDownload);
        CHECK(paths.size() == 1);
    }

    SECTION("a failed download sends nothing") {
        calls = 0;
        fetchForwardedFiles(
            session.get(),
            {remote, file("F9", "gone.txt")},
            [&](QStringList p, QStringList, QString e) {
                ++calls;
                paths = p;
                error = e;
            }
        );
        CHECK(calls == 1);
        CHECK_FALSE(error.isEmpty());
        CHECK(paths.isEmpty());
    }

    SECTION("without a source every file goes as a link") {
        fetchForwardedFiles(nullptr, {remote}, [&](QStringList p, QStringList l, QString e) {
            paths = p;
            links = l;
            error = e;
        });
        CHECK(error.isEmpty());
        CHECK(paths.isEmpty());
        CHECK(links == QStringList{"https://team.example.com/files/F1"});
    }
}
