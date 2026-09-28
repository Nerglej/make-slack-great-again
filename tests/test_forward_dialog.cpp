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
    CHECK(dialog.targetConv() == ConversationId{"C_agents"});
    CHECK(forwardButton(dialog)->isEnabled());

    picker->setCurrentIndex(0);
    CHECK(dialog.targetSession() == team.get());
    CHECK(dialog.targetConv().value.isEmpty());
    CHECK_FALSE(forwardButton(dialog)->isEnabled());
    CHECK(offeredConversations(dialog, "e") == QStringList{"#general"});

    emit convSearch(dialog)->returnPressed();
    CHECK(dialog.targetConv() == ConversationId{"C_general"});
    CHECK(forwardButton(dialog)->isEnabled());
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
