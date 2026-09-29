// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
// Claude Code backend: transcript parsing, roster parsing, and the backend
// end to end against a fake ~/.claude (CLAUDE_CONFIG_DIR) — docs/backend-modules-plan.md §5.
#include <catch2/catch_test_macros.hpp>

#include "test_main.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QBuffer>
#include <QImage>
#include <QJsonArray>
#include <QFileInfo>
#include <QUrl>
#include <QDateTime>
#include <QTimeZone>

#include "backend/claude_code/cc_attach.h"
#include "backend/claude_code/cc_catalog.h"
#include "backend/claude_code/cc_launcher.h"
#include "backend/claude_code/cc_outputs.h"
#include "backend/claude_code/cc_roster.h"
#include "backend/claude_code/cc_transcript.h"
#include "backend/claude_code/cc_vt.h"
#include "backend/claude_code/cc_worktrees.h"
#include "backend/claude_code/claude_code_backend.h"

using namespace claude_code;
using namespace Qt::StringLiterals;
using Kind  = TranscriptItem::Kind;
using State = TranscriptItem::State;

MSGA_TEST_MAIN(argc, argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("msga-test");
    app.setOrganizationName("msga-test");
    QStandardPaths::setTestModeEnabled(true);
    return msga_test::runCatch(argc, argv);
}

namespace {

QByteArray line(const QJsonObject &o) {
    return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
}

QByteArray prompt(const QString &text, const char *ts, bool viaOrigin = true) {
    QJsonObject o{
        {"type", "user"},
        {"timestamp", ts},
        {"message", QJsonObject{{"role", "user"}, {"content", text}}},
    };
    if (viaOrigin)
        o["origin"] = QJsonObject{{"kind", "human"}};
    return line(o);
}

QByteArray assistantText(const QString &text, const char *ts) {
    return line({
        {"type", "assistant"},
        {"timestamp", ts},
        {"message",
         QJsonObject{{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", text}}}}}},
    });
}

QByteArray
toolUse(const QString &id, const QString &name, const QJsonObject &input, const char *ts) {
    return line({
        {"type", "assistant"},
        {"timestamp", ts},
        {"message",
         QJsonObject{
             {"content",
              QJsonArray{
                  QJsonObject{{"type", "tool_use"}, {"id", id}, {"name", name}, {"input", input}}
              }}
         }},
    });
}

// An Agent call's result names the subagent; `status` is "async_launched" for
// a background one (just started), "completed" for a foreground one (done).
QByteArray toolResult(
    const QString &id,
    const char    *ts,
    bool           error   = false,
    const QString &agentId = {},
    const QString &status  = {}
) {
    QJsonObject o{
        {"type", "user"},
        {"timestamp", ts},
        {"message",
         QJsonObject{
             {"content",
              QJsonArray{
                  QJsonObject{{"type", "tool_result"}, {"tool_use_id", id}, {"is_error", error}}
              }}
         }},
    };
    if (!agentId.isEmpty()) {
        QJsonObject result{{"agentId", agentId}};
        if (!status.isEmpty())
            result["status"] = status;
        o["toolUseResult"] = result;
    }
    return line(o);
}

QByteArray turnEnd(const char *ts) {
    return line({{"type", "system"}, {"subtype", "turn_duration"}, {"timestamp", ts}});
}

// What the session is told when a background task (a subagent) stops, as
// Claude Code writes it: queued the moment it stops, delivered as a prompt.
QString taskNotification(const QString &taskId) {
    return "<task-notification>\n<task-id>" + taskId +
           "</task-id>\n<status>completed</status>\n<summary>Agent finished</summary>\n"
           "</task-notification>";
}
QByteArray taskStopped(const QString &taskId, const char *ts) {
    return line({
        {"type", "queue-operation"},
        {"operation", "enqueue"},
        {"timestamp", ts},
        {"content", taskNotification(taskId)},
    });
}

// One realistic turn: prompt, a remark, two tool calls, the answer, turn end.
QByteArray sampleTurn() {
    return prompt("fix the build", "2026-09-25T10:00:00.000Z") +
           assistantText("Let me look.", "2026-09-25T10:00:01.000Z") +
           toolUse(
               "t1",
               "Bash",
               {{"command", "make"}, {"description", "Build it"}},
               "2026-09-25T10:00:02.000Z"
           ) +
           toolResult("t1", "2026-09-25T10:00:03.000Z") +
           toolUse("t2", "Edit", {{"file_path", "/x/main.cpp"}}, "2026-09-25T10:00:04.000Z") +
           toolResult("t2", "2026-09-25T10:00:05.000Z", /*error=*/true) +
           assistantText("Fixed: a missing **include**.", "2026-09-25T10:00:06.000Z") +
           turnEnd("2026-09-25T10:00:07.000Z");
}

} // namespace

// ── Transcript parsing ────────────────────────────────────────────────────────

TEST_CASE(
    "a turn becomes prompt, progress remark, tool card and final answer", "[claude][transcript]"
) {
    TranscriptParser p;
    p.feed(sampleTurn());
    const auto &items = p.items();
    REQUIRE(items.size() == 4);
    CHECK(items[0].kind == Kind::UserPrompt);
    CHECK(items[0].text == "fix the build");
    CHECK(items[1].kind == Kind::AssistantText);
    CHECK(items[1].state == State::Progress); // Claude kept working after it
    CHECK(items[2].kind == Kind::ToolGroup);
    REQUIRE(items[2].tools.size() == 2);
    CHECK(items[2].tools[0].summary == "Build it");
    CHECK_FALSE(items[2].tools[0].error);
    CHECK(items[2].tools[1].error);
    CHECK(items[3].kind == Kind::AssistantText);
    CHECK(items[3].state == State::Final);
    CHECK_FALSE(p.turnOpen());
}

TEST_CASE(
    "the last text of a live turn stays pending until the turn resolves it", "[claude][transcript]"
) {
    TranscriptParser p;
    p.feed(
        prompt("hi", "2026-09-25T10:00:00.000Z") +
        assistantText("Hello!", "2026-09-25T10:00:01.000Z")
    );
    REQUIRE(p.items().size() == 2);
    CHECK(p.items()[1].state == State::Pending);
    CHECK(p.turnOpen());
    // Hidden while the session works, shown once it stopped.
    CHECK_FALSE(isVisible(p.items()[1], /*sessionBusy=*/true));
    CHECK(isVisible(p.items()[1], /*sessionBusy=*/false));

    p.feed(turnEnd("2026-09-25T10:00:02.000Z"));
    CHECK(p.items()[1].state == State::Final);
}

TEST_CASE("feeding byte by byte yields the same items as one piece", "[claude][transcript]") {
    const QByteArray all = sampleTurn();
    TranscriptParser whole;
    whole.feed(all);
    TranscriptParser pieces;
    for (qsizetype i = 0; i < all.size(); i += 7)
        pieces.feed(all.mid(i, 7));
    CHECK(pieces.items() == whole.items());
}

TEST_CASE("system text recorded as user turns is hidden", "[claude][transcript]") {
    TranscriptParser p;
    QJsonObject      meta{
        {"type", "user"},
        {"isMeta", true},
        {"timestamp", "2026-09-25T10:00:00.000Z"},
        {"message", QJsonObject{{"content", "caveat"}}},
    };
    QJsonObject notification{
        {"type", "user"},
        {"timestamp", "2026-09-25T10:00:01.000Z"},
        {"origin", QJsonObject{{"kind", "task-notification"}}},
        {"message", QJsonObject{{"content", "<task-notification>done</task-notification>"}}},
    };
    p.feed(
        line(meta) + line(notification) +
        prompt("[Request interrupted by user]", "2026-09-25T10:00:03.000Z") +
        prompt(
            "<command-name>/model</command-name>\n<command-args>opus</command-args>",
            "2026-09-25T10:00:04.000Z"
        )
    );
    REQUIRE(p.items().size() == 1);
    CHECK(p.items()[0].text == "/model opus"); // a slash command reads as typed
}

TEST_CASE("a command Claude Code runs itself shows with its output", "[claude][transcript]") {
    TranscriptParser p;
    // /context: both halves are "local_command" system records.
    p.feed(
        line({
            {"type", "system"},
            {"subtype", "local_command"},
            {"timestamp", "2026-09-25T10:00:00.000Z"},
            {"content",
             "<command-name>/context</command-name>\n<command-message>context</command-message>\n"
             "<command-args></command-args>"},
        }) +
        line({
            {"type", "system"},
            {"subtype", "local_command"},
            {"timestamp", "2026-09-25T10:00:01.000Z"},
            {"content",
             "<local-command-stdout>\x1b[1mContext Usage\x1b[22m\n42k / 1m</local-command-stdout>"},
        })
    );
    REQUIRE(p.items().size() == 2);
    CHECK(p.items()[0].kind == Kind::UserPrompt);
    CHECK(p.items()[0].text == "/context");
    CHECK(p.items()[1].kind == Kind::AssistantText);
    CHECK(p.items()[1].state == State::Final);
    CHECK(p.items()[1].text == "```\nContext Usage\n42k / 1m\n```"); // colours gone
    CHECK_FALSE(p.turnOpen()); // no turn_duration comes: the output ends it

    // …followed by the same report as markdown for the model, which is shown instead.
    p.feed(line({
        {"type", "user"},
        {"isMeta", true},
        {"timestamp", "2026-09-25T10:00:01.500Z"},
        {"message", QJsonObject{{"content", "## Context Usage\n\n**Tokens:** 42k / 1m"}}},
    }));
    REQUIRE(p.items().size() == 2);
    CHECK(p.items()[1].text == "## Context Usage\n\n**Tokens:** 42k / 1m");

    // /compact: user records, after the summary it starts the session over from.
    QJsonObject summary{
        {"type", "user"},
        {"isCompactSummary", true},
        {"timestamp", "2026-09-25T10:01:00.000Z"},
        {"message", QJsonObject{{"content", "This session is being continued…"}}},
    };
    QJsonObject synthetic{
        {"type", "assistant"},
        {"timestamp", "2026-09-25T10:00:59.000Z"},
        {"message",
         QJsonObject{
             {"model", "<synthetic>"},
             {"content",
              QJsonArray{QJsonObject{{"type", "text"}, {"text", "No response requested."}}}},
         }},
    };
    p.feed(
        line(synthetic) + prompt("/compact keep it short", "2026-09-25T10:00:59.500Z") +
        line(summary) +
        prompt(
            "<command-name>/compact</command-name>\n<command-args>keep it short</command-args>",
            "2026-09-25T10:01:01.000Z"
        ) +
        prompt(
            "<local-command-stdout>\x1b[2mCompacted\x1b[22m</local-command-stdout>",
            "2026-09-25T10:01:02.000Z"
        )
    );
    REQUIRE(p.items().size() == 4);
    CHECK(p.items()[2].text == "/compact keep it short");
    CHECK(p.items()[3].text == "Compacted");
    CHECK_FALSE(p.turnOpen());
}

TEST_CASE("Claude Code's command list", "[claude][commands]") {
    const QByteArray out =
        R"j({"type":"control_response","response":{"subtype":"success","request_id":"msga",)j"
        R"j("response":{"commands":[)j"
        R"j({"name":"compact","description":"Free up context","argumentHint":"<instructions>","builtin":true},)j"
        R"j({"name":"verify","description":"Drive the app (project)","argumentHint":""},)j"
        R"j({"name":"gui-sudo","description":"Run a root command (user)","argumentHint":""},)j"
        R"j({"name":"__remote-workflow","description":"internal","builtin":true},)j"
        R"j({"name":"agents","description":"(removed) Ask Claude","builtin":true},)j"
        R"j({"name":"extra-usage","description":"Renamed to /usage-credits","builtin":true}]}}})j"
        "\n";
    const auto cmds = parseCommandList(out);
    REQUIRE(cmds.size() == 3);
    CHECK(cmds[0].name == "compact");
    CHECK(cmds[0].usage == "<instructions>");
    CHECK(cmds[0].source == "Claude Code");
    CHECK(cmds[1].desc == "Drive the app");
    CHECK(cmds[1].source == "Project skill");
    CHECK(cmds[2].source == "Skill");
    CHECK(parseCommandList("not json\n").empty());
}

TEST_CASE("a prompt queued mid-turn shows without ending the turn", "[claude][transcript]") {
    TranscriptParser p;
    p.feed(
        prompt("go", "2026-09-25T10:00:00.000Z") +
        assistantText("Working on it", "2026-09-25T10:00:01.000Z")
    );
    p.feed(line({
        {"type", "attachment"},
        {"timestamp", "2026-09-25T10:00:02.000Z"},
        {"attachment",
         QJsonObject{{"type", "queued_command"}, {"commandMode", "prompt"}, {"prompt", "also X"}}},
    }));
    REQUIRE(p.items().size() == 3);
    CHECK(p.items()[2].kind == Kind::UserPrompt);
    CHECK(p.items()[2].text == "also X");
    CHECK(p.items()[1].state == State::Pending); // the turn goes on
}

TEST_CASE("an Agent call becomes a subagent item linked by its agent id", "[claude][transcript]") {
    TranscriptParser p;
    p.feed(
        prompt("research", "2026-09-25T10:00:00.000Z") +
        toolUse(
            "a1",
            "Agent",
            {{"description", "Search the docs"}, {"prompt", "…"}},
            "2026-09-25T10:00:01.000Z"
        ) +
        toolResult("a1", "2026-09-25T10:00:09.000Z", false, "abc123")
    );
    REQUIRE(p.items().size() == 2);
    CHECK(p.items()[1].kind == Kind::Subagent);
    CHECK(p.items()[1].text == "Search the docs");
    CHECK(p.items()[1].agentId == "abc123");
}

TEST_CASE("a subagent item keeps the type it was started as", "[claude][transcript]") {
    TranscriptParser p;
    p.feed(toolUse(
        "a1",
        "Agent",
        {{"description", "Draw it"}, {"subagent_type", "designer"}},
        "2026-09-25T10:00:01.000Z"
    ));
    REQUIRE(p.items().size() == 1);
    CHECK(p.items()[0].agentType == "designer");
}

TEST_CASE("a prompt mentioning teammates says how to spawn them", "[claude][roles]") {
    const auto find = [](const QString &id) -> const Role * {
        for (const Role &r : builtInRoles())
            if (r.id == id)
                return &r;
        return nullptr;
    };
    const QString typed =
        "ask one @claude:role:engineer to fix 1-4, another @claude:role:engineer for p. 11";
    const QString note = teammateNote(typed, find);
    // Once per teammate, its prompt included: a session started before the team
    // were subagent types (or a copy of one) has no "engineer" type to pick.
    CHECK(note.count("# Your role: Engineer (msga: engineer)") == 1);
    CHECK(note.contains("subagent_type \"engineer\""));
    CHECK(note.contains("subagent_type \"claude\""));
    CHECK(withoutTeammateNote(typed + note) == typed);
    CHECK(withoutTeammateNote(typed) == typed);
    // The Generalist and unknown ids add nothing; nor does a bare word.
    CHECK(teammateNote("ask @claude:agent and @claude:role:astronaut", find).isEmpty());
    CHECK(teammateNote("ask an engineer", find).isEmpty());

    // The prompt reads back as typed.
    TranscriptParser p;
    p.feed(prompt(typed + note, "2026-09-25T10:00:00.000Z"));
    REQUIRE(p.items().size() == 1);
    CHECK(p.items()[0].text == typed);

    // A subagent's teammate, from its prompt: our header, or Claude's own line.
    CHECK(
        roleInAgentPrompt("# Your role: Engineer (msga: engineer)\nYou are…\n\nFix it.") ==
        "engineer"
    );
    CHECK(roleInAgentPrompt("Role: engineer. Repo: msga …") == "engineer");
    CHECK(roleInAgentPrompt("Fix the role: engineer bug").isEmpty());
}

TEST_CASE("teammates a session has no types for count when merely named", "[claude][roles]") {
    const auto find = [](const QString &id) -> const Role * {
        for (const Role &r : builtInRoles())
            if (r.id == id)
                return &r;
        return nullptr;
    };
    const std::vector<Role> &team = builtInRoles();

    // "Spawn a marketer agent" in a session started without --agents: Claude
    // would otherwise spawn a general-purpose one and write its own role.
    const QString typed = "Spawn a marketer agent to do the investigation.";
    const QString note  = teammateNote(typed, find, team);
    CHECK(note.count("# Your role: Marketer (msga: marketer)") == 1);
    CHECK(note.contains("subagent_type \"claude\""));
    CHECK(!note.contains("msga: engineer"));
    CHECK(withoutTeammateNote(typed + note) == typed);
    CHECK(roleInAgentPrompt(appendedPrompt(*find("marketer")) + "\n\nFind links.") == "marketer");

    // Any case, plural, the id; a mention and the name make one entry.
    CHECK(teammateNote("ask two Engineers", find, team).contains("msga: engineer"));
    CHECK(teammateNote("the RESEARCHER", find, team).contains("msga: researcher"));
    CHECK(
        teammateNote("@claude:role:designer — the designer", find, team)
            .count("# Your role: Designer") == 1
    );
    // Not inside other words, paths or mentions of something else; the
    // Generalist adds nothing; a type the session offers isn't in `byName`.
    CHECK(teammateNote("engineering the design", find, team).isEmpty());
    CHECK(teammateNote("see src/engineer/ and @claude:engineer", find, team).isEmpty());
    CHECK(teammateNote("the generalist", find, team).isEmpty());
    CHECK(teammateNote("spawn a marketer", find).isEmpty());

    // An added teammate with a two-word name.
    Role analyst;
    analyst.id     = "data-analyst";
    analyst.name   = "Data analyst";
    analyst.prompt = "Dig.";
    CHECK(teammateNote("ask the data analysts", find, {analyst}).contains("msga: data-analyst"));
    CHECK(teammateNote("the Data-Analyst", find, {analyst}).contains("msga: data-analyst"));
    CHECK(teammateNote("the data", find, {analyst}).isEmpty());
}

TEST_CASE("the subagent types a session offers are read", "[claude][transcript]") {
    const auto listing = [](const QStringList &added, const QStringList &removed, bool initial) {
        return line(
            {{"type", "attachment"},
             {"timestamp", "2026-09-25T10:00:02.000Z"},
             {"attachment",
              QJsonObject{
                  {"type", "agent_listing_delta"},
                  {"addedTypes", QJsonArray::fromStringList(added)},
                  {"removedTypes", QJsonArray::fromStringList(removed)},
                  {"isInitial", initial},
              }}}
        );
    };
    TranscriptParser p;
    CHECK(p.agentTypes().isEmpty());
    p.feed(listing({"claude", "Explore", "marketer"}, {}, true));
    CHECK(p.agentTypes() == QSet<QString>{"claude", "Explore", "marketer"});
    p.feed(listing({"engineer"}, {"marketer"}, false));
    CHECK(p.agentTypes() == QSet<QString>{"claude", "Explore", "engineer"});
    p.feed(listing({"claude"}, {}, true));
    CHECK(p.agentTypes() == QSet<QString>{"claude"});
    CHECK(p.items().empty());
}

TEST_CASE("a background task's stop notifications are noted", "[claude][transcript]") {
    TranscriptParser p;
    // Queued, delivered mid-turn, delivered as a turn of its own: the latest counts.
    p.feed(taskStopped("agent42", "2026-09-25T10:00:05.000Z"));
    CHECK(p.taskStoppedAt("agent42") == 1790330405000000);
    p.feed(line({
        {"type", "attachment"},
        {"timestamp", "2026-09-25T10:01:00.000Z"},
        {"attachment",
         QJsonObject{
             {"type", "queued_command"},
             {"commandMode", "task-notification"},
             {"prompt", taskNotification("agent42")},
         }},
    }));
    CHECK(p.taskStoppedAt("agent42") == 1790330460000000);
    p.feed(line({
        {"type", "user"},
        {"timestamp", "2026-09-25T10:02:00.000Z"},
        {"origin", QJsonObject{{"kind", "task-notification"}}},
        {"message", QJsonObject{{"role", "user"}, {"content", taskNotification("agent7")}}},
    }));
    CHECK(p.taskStoppedAt("agent7") == 1790330520000000);
    CHECK(p.items().empty()); // none of it is anything anyone said
    CHECK(p.activity().size() == 3);

    // A notification quoted by a tool's output isn't one.
    TranscriptParser q;
    q.feed(
        toolUse("t1", "Bash", {{"command", "grep"}}, "2026-09-25T10:00:00.000Z") +
        line({
            {"type", "user"},
            {"timestamp", "2026-09-25T10:00:01.000Z"},
            {"message",
             QJsonObject{
                 {"content",
                  QJsonArray{QJsonObject{
                      {"type", "tool_result"},
                      {"tool_use_id", "t1"},
                      {"content", taskNotification("agent42")},
                  }}}
             }},
        })
    );
    CHECK(q.taskStoppedAt("agent42") == 0);
    CHECK(p.taskStoppedAt("nobody") == 0);
}

TEST_CASE("a foreground subagent's result is its stop", "[claude][transcript]") {
    // Only a background subagent is notified when it stops; a foreground one
    // just returns, its result "completed" (Claude Code 2.1.283's Agent tool).
    TranscriptParser p;
    p.feed(
        toolUse("a1", "Agent", {{"description", "Fix it"}}, "2026-09-25T10:00:01.000Z") +
        toolResult("a1", "2026-09-25T10:00:09.000Z", false, "fg1", "completed") +
        toolUse("a2", "Agent", {{"description", "Look"}}, "2026-09-25T10:00:10.000Z") +
        toolResult("a2", "2026-09-25T10:00:10.500Z", false, "bg1", "async_launched")
    );
    CHECK(p.taskStoppedAt("fg1") == 1790330409000000);
    CHECK(p.taskStoppedAt("bg1") == 0); // started, not stopped
}

TEST_CASE("a reply relayed to a subagent reads back as the reply alone", "[claude][transcript]") {
    TranscriptParser p;
    const QString    relay = subagentReplyPrompt("ab0ae6eeb1648c1f4", "What codeword?\n\nJust it.");
    p.feed(prompt(relay, "2026-09-25T10:00:00.000Z") + prompt("plain", "2026-09-25T10:00:01.000Z"));
    REQUIRE(p.items().size() == 2);
    CHECK(p.items()[0].relayTo == "ab0ae6eeb1648c1f4");
    CHECK(p.items()[0].text == "What codeword?\n\nJust it.");
    CHECK(p.items()[1].relayTo.isEmpty());
}

// ── Prompt history (history.jsonl, what ↑ steps through) ──────────────────────

TEST_CASE("the prompt history is the folder's, this session's first", "[claude][history]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString pastes = dir.filePath("paste-cache");
    REQUIRE(QDir().mkpath(pastes));
    QFile cached(pastes + "/abc123.txt");
    REQUIRE(cached.open(QIODevice::WriteOnly));
    cached.write("pasted from the cache");
    cached.close();

    const auto entry = [](const QString     &text,
                          const QString     &project,
                          const QString     &session,
                          const QJsonObject &pasted = {}) {
        return line({
            {"display", text},
            {"pastedContents", pasted},
            {"timestamp", qint64(1790419488582)},
            {"project", project},
            {"sessionId", session},
        });
    };
    QFile f(dir.filePath("history.jsonl"));
    REQUIRE(f.open(QIODevice::WriteOnly));
    f.write(
        entry("other session, older", "/src/app", "S2") + entry("mine, older", "/src/app", "S1") +
        entry("another folder", "/src/other", "S1") +
        entry(
            "[Pasted text #1 +3 lines] and inline",
            "/src/app",
            "S2",
            QJsonObject{{"1", QJsonObject{{"id", 1}, {"type", "text"}, {"content", "PASTE"}}}}
        ) +
        entry(
            "see [Pasted text #7 +9 lines]",
            "/src/app",
            "S2",
            QJsonObject{{"7", QJsonObject{{"id", 7}, {"type", "text"}, {"contentHash", "abc123"}}}}
        ) +
        entry("[Image #1] what's wrong here?", "/src/app/", "S1") +
        entry(subagentReplyPrompt("agent42", "Which page?"), "/src/app", "S1") +
        entry("[Image #2]", "/src/app", "S1") + // nothing left to recall
        entry("again", "/src/app", "S1") + entry("again", "/src/app", "S1") + "not json\n"
    );
    f.close();

    const QStringList h = promptHistory(f.fileName(), pastes, "/src/app", "S1");
    CHECK(
        h == QStringList{
                 "again",
                 "Which page?",        // the relayed reply as typed, not msga's wrapper
                 "what's wrong here?", // the image placeholder dropped
                 "mine, older",
                 "see pasted from the cache",
                 "PASTE and inline",
                 "other session, older",
             }
    );
    CHECK(promptHistory(f.fileName(), pastes, "/src/app", "S1", 2).size() == 2);
    // No session yet (a teammate's page): the folder's prompts, newest first.
    CHECK(
        promptHistory(f.fileName(), pastes, "/src/app", QString()) ==
        QStringList{
            "again",
            "Which page?",
            "what's wrong here?",
            "see pasted from the cache",
            "PASTE and inline",
            "mine, older",
            "other session, older",
        }
    );
    CHECK(promptHistory(dir.filePath("missing.jsonl"), pastes, "/src/app", "S1").isEmpty());
}

TEST_CASE(
    "records in the same millisecond still get distinct, ordered ids", "[claude][transcript]"
) {
    TranscriptParser p;
    p.feed(
        prompt("a", "2026-09-25T10:00:00.000Z") + prompt("b", "2026-09-25T10:00:00.000Z") +
        prompt("c", "2026-09-25T09:00:00.000Z")
    ); // even a clock step back
    REQUIRE(p.items().size() == 3);
    CHECK(p.items()[0].ts < p.items()[1].ts);
    CHECK(p.items()[1].ts < p.items()[2].ts);
    CHECK(p.items()[0].ts.size() == p.items()[2].ts.size()); // fixed width: string order works
}

TEST_CASE("torn and foreign lines are skipped, not fatal", "[claude][transcript]") {
    TranscriptParser p;
    p.feed(
        "{not json\n" + line({{"type", "file-history-snapshot"}}) +
        prompt("still here", "2026-09-25T10:00:00.000Z")
    );
    REQUIRE(p.items().size() == 1);
    CHECK(p.items()[0].text == "still here");
}

TEST_CASE(
    "markdown renders headings bold, keeps angle brackets, fences tables", "[claude][markdown]"
) {
    const auto heading = renderMarkdown("## Result\nuse a < b && c > d");
    CHECK(heading.text.startsWith("Result\n"));
    CHECK(heading.text.contains("use a < b && c > d"));
    bool bold = false;
    for (const auto &e : heading.entities)
        bold = bold || (e.type == EntityType::Bold && e.offset == 0 && e.length == 6);
    CHECK(bold);

    const auto table = renderMarkdown("| a | b |\n|---|---|\n| 1 | 2 |");
    bool       pre   = false;
    for (const auto &e : table.entities)
        pre = pre || e.type == EntityType::Pre;
    CHECK(pre); // monospace keeps the columns aligned
}

TEST_CASE("bare URLs in Claude's text are links", "[claude][message]") {
    auto links = [](const QString &md) {
        const TextWithEntities t = renderMarkdown(md);
        QStringList            out;
        for (const auto &e : t.entities)
            if (e.type == EntityType::Link)
                out << e.data;
        return out;
    };
    CHECK(
        links("Here: https://ex.example.com/iA09a_HA?x=1&y=2.") ==
        QStringList{"https://ex.example.com/iA09a_HA?x=1&y=2"}
    );
    CHECK(links("(see https://a.example/b)") == QStringList{"https://a.example/b"});
    CHECK(
        links("https://en.wikipedia.org/wiki/Foo_(bar)") ==
        QStringList{"https://en.wikipedia.org/wiki/Foo_(bar)"}
    );
    CHECK(links("<https://auto.example/x>") == QStringList{"https://auto.example/x"});
    CHECK(links("[the docs](https://docs.example/p)") == QStringList{"https://docs.example/p"});
    CHECK(links("run `curl https://code.example/`").isEmpty());
    CHECK(links("```\nhttps://fenced.example/\n```").isEmpty());
    const TextWithEntities t = renderMarkdown("go to https://x.example/a now");
    CHECK(t.text == "go to https://x.example/a now");
}

TEST_CASE("a teammate mention renders as a mention", "[claude][message]") {
    auto mentions = [](const QString &md) {
        QStringList out;
        for (const auto &e : renderMarkdown(md).entities)
            if (e.type == EntityType::UserMention)
                out << e.data;
        return out;
    };
    CHECK(
        mentions("Do you know who @claude:role:engineer is?") == QStringList{"claude:role:engineer"}
    );
    CHECK(
        mentions("ask @claude:agent and @claude:role:data-analyst.") ==
        QStringList{"claude:agent", "claude:role:data-analyst"}
    );
    CHECK(mentions("run `echo @claude:role:engineer`").isEmpty());
    // Quoted on its own, it's still the teammate (Claude's habit).
    const auto quoted = renderMarkdown("Yes. `@claude:role:researcher` is the id.");
    CHECK(quoted.text == "Yes. @claude:role:researcher is the id.");
    CHECK(
        mentions("Yes. `@claude:role:researcher` is the id.") ==
        QStringList{"claude:role:researcher"}
    );
    CHECK(std::none_of(quoted.entities.begin(), quoted.entities.end(), [](const auto &e) {
        return e.type == EntityType::Code;
    }));
    CHECK(mentions("```\n@claude:role:engineer\n```").isEmpty());
    CHECK(mentions("mail x@claude:role:engineer").isEmpty());
    // A composer pill's raw token (older prompts hold it) gains no extra brackets.
    const auto pill = renderMarkdown("start an <@claude:role:engineer> subagent");
    CHECK(pill.text == "start an @claude:role:engineer subagent");
    CHECK(
        mentions("start an <@claude:role:engineer> subagent") == QStringList{"claude:role:engineer"}
    );
}

TEST_CASE("a pasted image is attached to the prompt", "[claude][message]") {
    QImage img(3, 2, QImage::Format_RGB32);
    img.fill(Qt::red);
    QByteArray png;
    QBuffer    buf(&png);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    QJsonObject rec{
        {"type", "user"},
        {"timestamp", "2026-09-25T10:00:00.000Z"},
        {"origin", QJsonObject{{"kind", "human"}}},
        {"imagePasteIds", QJsonArray{14}},
        {"message",
         QJsonObject{
             {"content",
              QJsonArray{
                  QJsonObject{{"type", "text"}, {"text", "look at this: [Image #14] please"}},
                  QJsonObject{
                      {"type", "image"},
                      {"source",
                       QJsonObject{
                           {"type", "base64"},
                           {"media_type", "image/png"},
                           {"data", QString::fromLatin1(png.toBase64())}
                       }}
                  },
              }}
         }},
    };
    TranscriptParser p;
    p.feed(line(rec));
    REQUIRE(p.items().size() == 1);
    const auto &item = p.items()[0];
    REQUIRE(item.images.size() == 1);
    CHECK(QFile::exists(item.images[0]));
    CHECK(item.text == "look at this: please"); // the placeholder goes
    const Message m = toMessage(item, UserId{"me"}, UserId{"claude:agent"});
    REQUIRE(m.files.size() == 1);
    CHECK(m.files[0].name == "Image 14.png");
    CHECK(m.files[0].imageWidth == 3);
    CHECK(m.files[0].imageHeight == 2);
    CHECK(m.files[0].thumbUrl.startsWith("file:"));
    // The same image again is the same cached file.
    CHECK(cachePastedImage("image/png", png.toBase64()) == item.images[0]);
}

TEST_CASE("files sent to a session ride the prompt as mentions", "[claude][message]") {
    QTemporaryDir dir;
    QImage        img(3, 2, QImage::Format_RGB32);
    img.fill(Qt::red);
    REQUIRE(img.save(dir.path() + "/shot.png"));
    {
        QFile f(dir.path() + "/my notes.txt");
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("notes");
    }
    // Copied into msga's cache, where a temp folder's cleanup can't take them;
    // the same content twice is the same copy.
    const QString shot  = cacheUpload(dir.path() + "/shot.png");
    const QString notes = cacheUpload(dir.path() + "/my notes.txt");
    REQUIRE(shot.startsWith(uploadsDir() + '/'));
    CHECK(shot.endsWith("/shot.png"));
    CHECK(QFile::exists(notes));
    CHECK(cacheUpload(dir.path() + "/shot.png") == shot);
    CHECK(cacheUpload(dir.path() + "/missing.png").isEmpty());

    // Mentions first — typed live, one ending the prompt would lose the Enter to
    // the completion list — a path with spaces quoted.
    const QString sent = withAttachments("what's **this**?", {shot, notes});
    CHECK(sent == "@" + shot + " @\"" + notes + "\" what's **this**?");
    CHECK(withAttachments("", {shot}) == "@" + shot + " (attached)");
    CHECK(withAttachments("/btw why", {shot}) == "/btw why @" + shot);
    CHECK(withAttachments("plain", {}) == "plain");

    // Read back, the mentions are the prompt's files again.
    TranscriptParser p;
    p.feed(prompt(sent, "2026-09-26T10:00:00.000Z"));
    p.feed(prompt(withAttachments("", {shot}), "2026-09-26T10:00:01.000Z"));
    p.feed(prompt("see @/etc/hosts", "2026-09-26T10:00:02.000Z"));
    REQUIRE(p.items().size() == 3);
    const auto &item = p.items()[0];
    CHECK(item.text == "what's **this**?");
    CHECK(item.images == QStringList{shot, notes});
    const Message m = toMessage(item, UserId{"me"}, UserId{"claude:agent"});
    REQUIRE(m.files.size() == 2);
    CHECK(m.files[0].name == "shot.png");
    CHECK(m.files[0].mimeType == "image/png");
    CHECK(m.files[0].imageWidth == 3);
    CHECK(m.files[0].thumbUrl.startsWith("file:"));
    CHECK(m.files[1].name == "my notes.txt");
    CHECK(m.files[1].mimeType == "text/plain");
    CHECK(m.files[1].thumbUrl.isEmpty()); // a download, no picture
    CHECK(m.files[1].urlPrivateDownload == QUrl::fromLocalFile(notes).toString());
    // Files alone: no stand-in text shown.
    CHECK(p.items()[1].text.isEmpty());
    CHECK(p.items()[1].images == QStringList{shot});
    // A file the user mentioned themselves stays what they typed.
    CHECK(p.items()[2].text == "see @/etc/hosts");
    CHECK(p.items()[2].images.isEmpty());
}

TEST_CASE("markdown tables become table blocks in reading order", "[claude][message]") {
    const QString md     = "Before the table.\n\n"
                           "| Session | Can you write? |\n"
                           "|---|:---:|\n"
                           "| Open in a terminal | **no** |\n"
                           "| Closed \\| gone | `a|b` |\n"
                           "\nAfter it.";
    const auto    blocks = markdownBlocks(md);
    REQUIRE(blocks.size() == 3);
    CHECK(blocks[0].typeStr == "rich_text");
    CHECK(blocks[0].text.text == "Before the table.");
    REQUIRE(blocks[1].typeStr == "table");
    const auto &rows = blocks[1].tableRows;
    REQUIRE(rows.size() == 3); // header + 2, the separator dropped
    REQUIRE(rows[0].size() == 2);
    CHECK(rows[0][0].text == "Session");
    CHECK(std::any_of(rows[0][0].entities.begin(), rows[0][0].entities.end(), [](const auto &e) {
        return e.type == EntityType::Bold; // header row is bold
    }));
    CHECK(rows[1][1].text == "no");
    CHECK(std::any_of(rows[1][1].entities.begin(), rows[1][1].entities.end(), [](const auto &e) {
        return e.type == EntityType::Bold; // the cell's own **bold**
    }));
    CHECK(rows[2][0].text == "Closed | gone"); // escaped pipe stays in the cell
    CHECK(rows[2][1].text == "a|b");           // so does one inside code
    CHECK(blocks[2].text.text == "After it.");

    CHECK(markdownBlocks("no table here\n| just a pipe line |").empty());
    CHECK(markdownBlocks("```\n| a | b |\n|---|---|\n```").empty()); // fenced stays code
}

TEST_CASE("items map to messages: authors, progress subtype, tool card", "[claude][message]") {
    TranscriptParser p;
    p.feed(sampleTurn());
    const UserId me{"me"}, claude{"claude:S1"};
    const auto   items = p.items();
    CHECK(toMessage(items[0], me, claude).author == me);
    const Message remark = toMessage(items[1], me, claude);
    CHECK(remark.author == claude);
    CHECK(isProgressMessage(remark));
    const Message card = toMessage(items[2], me, claude);
    CHECK(isProgressMessage(card));
    REQUIRE(card.attachments.size() == 1);
    CHECK(card.attachments[0].text.text.contains("Bash  Build it"));
    CHECK(card.attachments[0].text.text.contains("✗")); // the failed Edit
    const Message answer = toMessage(items[3], me, claude);
    CHECK_FALSE(isProgressMessage(answer));
    CHECK(answer.text.text == "Fixed: a missing include.");
}

// ── Roster ────────────────────────────────────────────────────────────────────

TEST_CASE("roster files parse into sessions", "[claude][roster]") {
    const auto live = parseInteractiveSession(
        R"({"pid":42,"sessionId":"S1","cwd":"/src/app","name":"app-1","status":"waiting",
            "statusUpdatedAt":1790000000000,"entrypoint":"cli","kind":"interactive"})"
    );
    REQUIRE(live);
    CHECK(live->sessionId == "S1");
    CHECK(live->kind == SessionInfo::Kind::Interactive);
    CHECK(live->entrypoint == "cli");
    CHECK(statusNeedsUser(live->status));

    const auto job = parseBackgroundJob(
        R"({"state":"done","sessionId":"S2","name":"Refactor","cwd":"/src/x","needs":null,
            "linkScanPath":"/p/S2.jsonl","updatedAt":"2026-09-02T06:56:01.982Z"})"
    );
    REQUIRE(job);
    CHECK(job->kind == SessionInfo::Kind::Background);
    CHECK_FALSE(job->running); // done: msga may continue it
    CHECK(job->transcriptPath == "/p/S2.jsonl");
    CHECK(job->statusSinceMs > 0);
    CHECK(job->worktreePath.isEmpty());
    const auto inWorktree = parseBackgroundJob(
        R"({"state":"done","sessionId":"S4","cwd":"/src/x/.claude/worktrees/w",)"
        R"("worktreePath":"/src/x/.claude/worktrees/w","worktreeBranch":"worktree-w"})"
    );
    REQUIRE(inWorktree);
    CHECK(inWorktree->worktreePath == "/src/x/.claude/worktrees/w");

    // Waiting for an approval: "working" plus a needs line (verified live).
    const auto approval = parseBackgroundJob(
        R"({"state":"working","sessionId":"S3","needs":"approve Bash: touch x"})"
    );
    REQUIRE(approval);
    CHECK(statusNeedsUser(approval->status));
    CHECK_FALSE(statusIsBusy(approval->status));
    CHECK(approval->awaitsApproval);
    // A question that happens to begin with "approve" is still a question
    // (seen in 2.1.284): answered by message, not in the terminal.
    const auto question = parseBackgroundJob(
        R"({"state":"blocked","tempo":"blocked","sessionId":"S5",)"
        R"("needs":"approve the data-origin wording before filing the issue"})"
    );
    REQUIRE(question);
    CHECK(statusNeedsUser(question->status));
    CHECK_FALSE(question->awaitsApproval);
    const auto mcp = parseBackgroundJob(
        R"({"state":"blocked","sessionId":"S6","needs":)"
        R"("approve 1 new project MCP server (x) — attach to respond"})"
    );
    REQUIRE(mcp);
    CHECK(mcp->awaitsApproval);

    // A done job whose worker is still alive (idle) counts as running — resuming
    // it would only start a copy — and a busy worker makes it busy.
    auto done   = *job;
    auto worker = parseInteractiveSession(
        R"({"pid":7,"sessionId":"S2","kind":"bg","status":"busy","entrypoint":"cli",)"
        R"("statusUpdatedAt":1790411125875})"
    );
    REQUIRE(worker);
    CHECK(worker->kind == SessionInfo::Kind::Background);
    applyWorker(done, *worker);
    CHECK(done.running);
    CHECK(statusIsBusy(done.status));
    CHECK(done.statusSinceMs == 1790411125875); // the worker's status, the worker's time

    // A job stuck on "working" after its turn ended: the idle worker wins.
    auto stale = *parseBackgroundJob(R"({"state":"working","sessionId":"S2"})");
    auto idle  = parseInteractiveSession(
        R"({"pid":7,"sessionId":"S2","kind":"bg","status":"idle","entrypoint":"cli"})"
    );
    REQUIRE(idle);
    applyWorker(stale, *idle);
    CHECK(stale.running);
    CHECK_FALSE(statusIsBusy(stale.status));
    // ...but a question waiting for the user stays visible.
    auto asking = *approval;
    applyWorker(asking, *idle);
    CHECK(statusNeedsUser(asking.status));

    CHECK_FALSE(parseInteractiveSession("{}"));
    CHECK_FALSE(parseBackgroundJob("garbage"));
    CHECK(statusIsBusy("busy"));
    CHECK(statusIsBusy("working"));
    CHECK_FALSE(statusIsBusy("idle"));
    // Idle with a background command running: nobody is typing.
    CHECK_FALSE(statusIsBusy("shell"));
    CHECK(statusHasShell("shell"));
}

// ── The backend against a fake ~/.claude ──────────────────────────────────────

namespace {

struct FakeClaudeHome {
    QTemporaryDir dir;
    QString       transcript;

    FakeClaudeHome() {
        qputenv("CLAUDE_CONFIG_DIR", dir.path().toUtf8());
        QDir(dir.path()).mkpath("sessions");
        QDir(dir.path()).mkpath("projects/-src-app");
        transcript = dir.path() + "/projects/-src-app/S1.jsonl";
        // A known-sessions file from an earlier test run would leak in.
        QFile::remove(
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
            "/claude-code/known-sessions.json"
        );
    }
    ~FakeClaudeHome() { qunsetenv("CLAUDE_CONFIG_DIR"); }

    void writeSession(const QString &status, const QString &name = "app-1") {
        QFile f(dir.path() + "/sessions/1.json");
        REQUIRE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QJsonDocument(
                    QJsonObject{
                        {"pid", QCoreApplication::applicationPid()}, // alive
                        {"sessionId", "S1"},
                        {"cwd", "/src/app"},
                        {"name", name},
                        {"status", status},
                        {"entrypoint", "cli"},
                    }
        )
                    .toJson());
    }
    void append(const QByteArray &bytes) {
        QFile f(transcript);
        REQUIRE(f.open(QIODevice::Append));
        f.write(bytes);
    }
};

void writeFile(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    REQUIRE(f.open(QIODevice::WriteOnly));
    f.write(bytes);
}

template <typename T>
std::vector<T> collect(rpl::producer<T> p) {
    std::vector<T> out;
    rpl::lifetime  lt;
    std::move(p) | rpl::on_next([&](T v) { out.push_back(std::move(v)); }, lt);
    return out;
}

} // namespace

TEST_CASE("claude rm says why it kept a session", "[claude][launcher]") {
    using claude_code::parseRemoveRefusal;
    CHECK(parseRemoveRefusal("removed abcdef11\n  worktree: /src/x/.claude/worktrees/w\n", 0)
              .isEmpty());
    CHECK(
        parseRemoveRefusal(
            u"kept abcdef11 — its worktree is still at /src/x/.claude/worktrees/w\n"
            u"  it has uncommitted changes\n"
            u"  claude rm abcdef11 --discard-unpushed 1a2b@3c4d\n"_s,
            1
        ) == "it has uncommitted changes"
    );
    CHECK(
        parseRemoveRefusal(u"kept abcdef11 — its worktree is still at /w\n"_s, 1) ==
        "its worktree is still at /w"
    );
    CHECK(
        parseRemoveRefusal(u"couldn't remove abcdef11 — its worker didn't stop\n"_s, 1) ==
        "its worker didn't stop"
    );
    CHECK(parseRemoveRefusal("error: unknown command 'rm'\n", 1) == "error: unknown command 'rm'");
    CHECK_FALSE(parseRemoveRefusal("", 1).isEmpty());
}

TEST_CASE("the worktrees a session used are found in its records", "[claude][worktrees]") {
    const auto job = worktreesOfJob(
        R"({"sessionId":"S","worktreePath":"/r/.claude/worktrees/a","worktreeBranch":"wt-a",)"
        R"("originCwd":"/r"})"
    );
    REQUIRE(job.size() == 1);
    CHECK(job[0] == WorktreeRef{"/r/.claude/worktrees/a", "wt-a", "/r"});
    CHECK(worktreesOfJob(R"({"sessionId":"S","worktreePath":null})").empty());

    QTemporaryDir dir;
    const QString t = dir.path() + "/S.jsonl";
    writeFile(
        t,
        R"({"type":"user","message":{"role":"user","content":"worktree-state"}})"
        "\n"
        R"({"type":"worktree-state","worktreeSession":{"worktreePath":"/r/w/b","worktreeBranch":"wt-b","originalCwd":"/r"}})"
        "\n"
        R"({"type":"worktree-state","worktreeSession":null})"
        "\n"
        R"({"type":"worktree-state","worktreeSession":{"worktreePath":"/r/w/b/","originalCwd":"/r"}})"
        "\n"
        R"({"type":"worktree-state","worktreeSession":{"worktreePath":"/r/w/c","worktreeBranch":"wt-c"}})"
    );
    writeFile(
        dir.path() + "/S/subagents/agent-x.meta.json",
        R"({"agentType":"x","worktreePath":"/r/w/d","worktreeBranch":"wt-d"})"
    );
    writeFile(dir.path() + "/S/subagents/agent-y.meta.json", R"({"agentType":"y"})");
    const auto seen = worktreesOfTranscript(t);
    REQUIRE(seen.size() == 3);
    CHECK(seen[0] == WorktreeRef{"/r/w/b", "wt-b", "/r"}); // named twice: once
    CHECK(seen[1] == WorktreeRef{"/r/w/c", "wt-c", ""});
    CHECK(seen[2] == WorktreeRef{"/r/w/d", "wt-d", ""});

    CHECK(pathWithin("/r/w/b/src", "/r/w/b"));
    CHECK(pathWithin("/r/w/b", "/r/w/b/"));
    CHECK_FALSE(pathWithin("/r/w/bc", "/r/w/b"));
    CHECK_FALSE(pathWithin("/r", "/r/w/b"));

    const auto list = parseWorktreeList(
        "worktree /r\nHEAD 1\nbranch refs/heads/master\n\n"
        "worktree /r/w/b\nHEAD 2\nbranch refs/heads/wt-b\n\n"
        "worktree /r/w/e\nHEAD 3\ndetached\nprunable gitdir file points to non-existent location\n"
    );
    REQUIRE(list.size() == 3);
    CHECK(list[0].path == "/r");
    CHECK(list[0].branch == "master");
    CHECK(list[1].branch == "wt-b");
    CHECK(list[2].branch.isEmpty());
}

TEST_CASE("a session title names the teammates it mentions", "[claude][backend]") {
    FakeClaudeHome home;
    home.writeSession("idle", "Ask @claude:role:engineer and @claude:agent");
    home.append(prompt("hi", "2026-09-25T10:00:00.000Z"));

    claude_code::Backend backend(Credentials{});
    backend.connectRealtime();
    const auto convs = collect(backend.loadConversations());
    REQUIRE(convs.size() == 1);
    REQUIRE(convs[0].size() == 1);
    CHECK(convs[0][0].name == "Ask @Engineer and @Generalist");
    const auto users = collect(backend.loadUsers());
    CHECK(std::any_of(users[0].begin(), users[0].end(), [](const User &u) {
        return u.id.value == "claude:S1" && u.name == "Ask @Engineer and @Generalist";
    }));
}

TEST_CASE(
    "backend lists a terminal session read-only and announces its answer", "[claude][backend]"
) {
    FakeClaudeHome home;
    home.writeSession("busy");
    home.append(
        prompt("hi", "2026-09-25T10:00:00.000Z") +
        assistantText("Hello!", "2026-09-25T10:00:01.000Z")
    );

    claude_code::Backend backend(Credentials{});
    std::vector<Event>   events;
    rpl::lifetime        lt;
    backend.events() | rpl::on_next([&](Event e) { events.push_back(std::move(e)); }, lt);
    backend.connectRealtime();

    const auto convs = collect(backend.loadConversations());
    REQUIRE(convs.size() == 1);
    REQUIRE(convs[0].size() == 1);
    const Conversation c = convs[0][0];
    // Working on the turn reads as the session typing (the terminal's spinner).
    CHECK(std::any_of(events.begin(), events.end(), [](const Event &e) {
        const auto *t = std::get_if<EvTyping>(&e);
        return t && t->conv.value == "S1" && t->user.value == "claude:agent";
    }));
    CHECK(c.name == "app-1");
    CHECK(c.kind == ConvKind::Im);
    CHECK(c.readOnlyReason.contains("terminal")); // a terminal drives it

    // While Claude works on the turn only the prompt shows.
    auto history = collect(backend.loadHistory(c.id, std::nullopt));
    REQUIRE(history.size() == 1);
    REQUIRE(history[0].messages.size() == 1);
    CHECK(history[0].messages[0].author == UserId{"me"});
    CHECK(history[0].fromStart); // the whole session: a cached row older than it is stale

    // The turn ends: the answer is announced (once), and the dot goes idle.
    home.append(turnEnd("2026-09-25T10:00:02.000Z"));
    home.writeSession("idle");
    const bool announced = QTest::qWaitFor(
        [&] {
            for (const auto &e : events)
                if (const auto *n = std::get_if<EvMessageNew>(&e))
                    return n->msg.text.text == "Hello!";
            return false;
        },
        5000
    );
    CHECK(announced);

    // The terminal session ends: without a claude CLI it stays read-only, but
    // for a different reason, and the change is pushed as a conversation update.
    events.clear();
    QFile::remove(home.dir.path() + "/sessions/1.json");
    const bool updated = QTest::qWaitFor(
        [&] {
            for (const auto &e : events)
                if (const auto *u = std::get_if<EvChannelCreated>(&e))
                    return u->conv.readOnlyReason.contains("Install");
            return false;
        },
        12000
    );
    CHECK(updated);
    // …and it is still listed: ended sessions stay while their transcript does.
    CHECK(collect(backend.loadConversations())[0].size() == 1);
}

TEST_CASE(
    "a session removed by an older msga stays away through an idle-worker retire",
    "[claude][backend]"
) {
    FakeClaudeHome home;
    home.writeSession("idle");
    home.append(
        prompt("hi", "2026-09-25T10:00:00.000Z") +
        assistantText("Hello!", "2026-09-25T10:00:01.000Z") + turnEnd("2026-09-25T10:00:02.000Z")
    );
    // Removed before hide entries kept the transcript's size (seen 2026-09-25:
    // the session came back an hour on, when Claude Code retired its worker).
    const qint64 removedAt = QDateTime::currentMSecsSinceEpoch();
    {
        const QString path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                             "/claude-code/known-sessions.json";
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(
                    QJsonObject{
                        {"sessions", QJsonArray{}},
                        {"hidden",
                         QJsonArray{QJsonObject{
                             {"sessionId", "S1"},
                             {"at", double(removedAt)},
                             {"transcript", home.transcript},
                         }}},
                    }
        )
                    .toJson());
    }
    QTest::qWait(20); // the transcript's next write must be newer than the removal

    claude_code::Backend backend(Credentials{});
    backend.connectRealtime();
    CHECK(collect(backend.loadConversations())[0].empty());

    home.append(
        "{\"type\":\"last-prompt\",\"lastPrompt\":\"hi\",\"sessionId\":\"S1\"}\n"
        "{\"type\":\"cost-state\",\"sessionId\":\"S1\"}\n"
    );
    home.writeSession("idle");
    QTest::qWait(1500);
    CHECK(collect(backend.loadConversations())[0].empty());

    // A turn after the removal brings it back.
    const QByteArray now = QDateTime::fromMSecsSinceEpoch(removedAt + 1000, Qt::UTC)
                               .toString(Qt::ISODateWithMs)
                               .toUtf8();
    home.append(prompt("more", now.constData()));
    home.writeSession("busy");
    CHECK(
        QTest::qWaitFor([&] { return collect(backend.loadConversations())[0].size() == 1; }, 12000)
    );
}

TEST_CASE(
    "a session removed from msga stays away until it gets new activity", "[claude][backend]"
) {
    FakeClaudeHome home;
    home.writeSession("idle");
    home.append(
        prompt("hi", "2026-09-25T10:00:00.000Z") +
        assistantText("Hello!", "2026-09-25T10:00:01.000Z") + turnEnd("2026-09-25T10:00:02.000Z")
    );
    const ConversationId conv{"S1"};
    {
        claude_code::Backend backend(Credentials{});
        backend.connectRealtime();
        REQUIRE(collect(backend.loadConversations())[0].size() == 1);
        backend.leaveConversation(conv);
        CHECK(collect(backend.loadConversations())[0].empty());
    }
    // Remembered across restarts; Claude Code's own files are left alone.
    CHECK(QFile::exists(home.transcript));
    CHECK(QFile::exists(home.dir.path() + "/sessions/1.json"));
    QTest::qWait(20); // the transcript's next write must be newer than the removal

    claude_code::Backend backend(Credentials{});
    std::vector<Event>   events;
    rpl::lifetime        lt;
    backend.events() | rpl::on_next([&](Event e) { events.push_back(std::move(e)); }, lt);
    backend.connectRealtime();
    CHECK(collect(backend.loadConversations())[0].empty());

    // Claude Code's daemon retiring the idle worker appends bookkeeping: no
    // activity, it stays away.
    home.append(
        "{\"type\":\"last-prompt\",\"lastPrompt\":\"hi\",\"sessionId\":\"S1\"}\n"
        "{\"type\":\"cost-state\",\"sessionId\":\"S1\"}\n"
    );
    home.writeSession("idle");
    QTest::qWait(1500);
    CHECK(collect(backend.loadConversations())[0].empty());

    // Someone continues it in the terminal: it's back.
    home.append(prompt("more", "2026-09-25T11:00:00.000Z"));
    home.writeSession("busy");
    CHECK(
        QTest::qWaitFor(
            [&] {
                for (const auto &e : events)
                    if (const auto *c = std::get_if<EvChannelCreated>(&e); c && c->conv.id == conv)
                        return true;
                return false;
            },
            12000
        )
    );
    CHECK(collect(backend.loadConversations())[0].size() == 1);
}

TEST_CASE(
    "a removed background job whose transcript Claude Code deleted stays removed",
    "[claude][backend]"
) {
    FakeClaudeHome home;
    QDir(home.dir.path()).mkpath("jobs/8d953db6");
    {
        QFile f(home.dir.path() + "/jobs/8d953db6/state.json");
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(
                    QJsonObject{
                        {"state", "done"},
                        {"sessionId", "8d953db6-f3be-4b02-8f0f-09aea0343b3e"},
                        {"cwd", "/src/app"},
                        {"name", "no transcript"},
                        {"linkScanPath", home.dir.path() + "/projects/-src-app/gone.jsonl"},
                    }
        )
                    .toJson());
    }
    const ConversationId conv{"8d953db6-f3be-4b02-8f0f-09aea0343b3e"};
    {
        claude_code::Backend backend(Credentials{});
        backend.connectRealtime();
        REQUIRE(collect(backend.loadConversations())[0].size() == 1);
        backend.leaveConversation(conv);
        // Any roster change triggers a rescan: it must not bring the job back.
        home.writeSession("idle");
        QTest::qWait(1500);
        const auto convs = collect(backend.loadConversations())[0];
        CHECK(std::none_of(convs.begin(), convs.end(), [&](const Conversation &c) {
            return c.id == conv;
        }));
    }
    claude_code::Backend backend(Credentials{});
    backend.connectRealtime();
    const auto convs = collect(backend.loadConversations())[0];
    CHECK(std::none_of(convs.begin(), convs.end(), [&](const Conversation &c) {
        return c.id == conv;
    }));
}

TEST_CASE("a background worker is paired with its job by job id", "[claude][roster]") {
    // A `/clear` sent to a job starts a new session in the same worker: its
    // sessions/<pid>.json names another session, and only its jobId the job
    // (seen with 2.1.282).
    FakeClaudeHome home;
    QDir(home.dir.path()).mkpath("jobs/ed076643");
    const QString sid = "ed076643-40f1-4f2f-aeb1-a0c39466257b";
    writeFile(
        home.dir.path() + "/jobs/ed076643/state.json",
        QJsonDocument(QJsonObject{{"state", "done"}, {"sessionId", sid}, {"cwd", "/src/app"}})
            .toJson()
    );
    writeFile(
        home.dir.path() + "/sessions/2.json",
        QJsonDocument(
            QJsonObject{
                {"pid", QCoreApplication::applicationPid()}, // alive
                {"sessionId", "b06f80b6-a8d7-45cc-a904-52553cf4e38e"},
                {"jobId", "ed076643"},
                {"kind", "bg"},
                {"status", "idle"},
            }
        )
            .toJson()
    );
    const Paths paths = Paths::detect();
    const auto  all   = scanSessions(paths);
    const auto  job   = std::find_if(all.begin(), all.end(), [&](const SessionInfo &s) {
        return s.sessionId == sid;
    });
    REQUIRE(job != all.end());
    CHECK(job->running);
    CHECK(job->status == "idle");
    CHECK(liveWorkerPids(paths, sid) == std::vector<qint64>{QCoreApplication::applicationPid()});
}

#if defined(Q_OS_LINUX)
TEST_CASE(
    "removing a session deletes it only when msga, or a session of msga's, started it",
    "[claude][backend][bg]"
) {
    FakeClaudeHome home;
    QTemporaryDir  work;
    QDir(home.dir.path()).mkpath("jobs");
    // A CLI whose `stop` the daemon doesn't act on: the worker lingers, and
    // msga has to end it itself.
    const QString cli = work.path() + "/claude";
    writeFile(cli, R"SH(#!/bin/sh
if [ "$1" = auth ]; then echo '{"loggedIn":true,"authMethod":"claude.ai"}'; exit 0; fi
printf '%s\n' "$*" >> "$CLAUDE_CONFIG_DIR/calls.log"
)SH");
    QFile(cli).setPermissions(QFile(cli).permissions() | QFileDevice::ExeOwner);
    const QString parent           = "aaaa1111-0000-4000-8000-000000000001";
    const QString child            = "bbbb2222-0000-4000-8000-000000000002";
    const QString other            = "cccc3333-0000-4000-8000-000000000003";
    const QString parentTranscript = home.dir.path() + "/projects/-src-app/" + parent + ".jsonl";
    // The session msga started ran `claude --bg`, which started `child`.
    writeFile(
        parentTranscript,
        prompt("start a helper", "2026-09-25T10:00:00.000Z") +
            "{\"type\":\"user\",\"timestamp\":\"2026-09-25T10:00:01.000Z\",\"message\":{\"role\":"
            "\"user\",\"content\":[{\"tool_use_id\":\"t1\",\"type\":\"tool_result\",\"content\":"
            "\"backgrounded \xc2\xb7 bbbb2222\\n  claude agents\"}]}}\n"
    );
    std::vector<qint64> workers;
    int                 n = 0;
    for (const QString &sid : {parent, child, other}) {
        const QString short8 = sid.left(8);
        QDir(home.dir.path()).mkpath("jobs/" + short8);
        writeFile(
            home.dir.path() + "/jobs/" + short8 + "/state.json",
            QJsonDocument(
                QJsonObject{
                    {"state", "done"},
                    {"sessionId", sid},
                    {"cwd", work.path()},
                    {"name", short8},
                    {"linkScanPath",
                     sid == parent ? parentTranscript
                                   : home.dir.path() + "/projects/-src-app/" + sid + ".jsonl"},
                }
            )
                .toJson()
        );
        qint64 pid = 0;
        REQUIRE(QProcess::startDetached("sleep", {"60"}, {}, &pid));
        workers.push_back(pid);
        writeFile(
            home.dir.path() + QStringLiteral("/sessions/w%1.json").arg(++n),
            QJsonDocument(
                QJsonObject{
                    {"pid", pid},
                    {"sessionId", sid},
                    {"jobId", short8},
                    {"kind", "bg"},
                    {"status", "idle"}
                }
            )
                .toJson()
        );
    }
    // msga started `parent` in an earlier run.
    const QString known = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                          "/claude-code/known-sessions.json";
    QDir().mkpath(QFileInfo(known).absolutePath());
    writeFile(
        known,
        QJsonDocument(
            QJsonObject{{"sessions", QJsonArray{}}, {"started", QJsonArray{parent}}}
        ).toJson()
    );
    auto calls = [&] {
        QFile f(home.dir.path() + "/calls.log");
        return f.open(QIODevice::ReadOnly)
                   ? QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts)
                   : QStringList{};
    };

    claude_code::Backend backend(Credentials{cli});
    backend.connectRealtime();
    REQUIRE(collect(backend.loadConversations())[0].size() == 3);

    // Started elsewhere: it only leaves the list.
    backend.leaveConversation(ConversationId{other});
    QTest::qWait(1500);
    CHECK(calls().filter("cccc3333").isEmpty());
    CHECK(isProcessAlive(workers[2]));

    // Started by msga's session: deleted (`claude rm`), its lingering worker
    // ended too.
    backend.leaveConversation(ConversationId{child});
    REQUIRE(QTest::qWaitFor([&] { return calls().contains("rm bbbb2222"); }, 5000));
    CHECK(QTest::qWaitFor([&] { return !isProcessAlive(workers[1]); }, 16000));
    CHECK(isProcessAlive(workers[0]));
    CHECK(isProcessAlive(workers[2]));
    CHECK(calls().filter("stop").isEmpty());
    CHECK(calls().filter(QRegularExpression("^rm ")).size() == 1);
    for (const qint64 pid : workers)
        signalProcess(pid, true);
}

TEST_CASE(
    "removing a session msga started deletes every worktree it used", "[claude][backend][bg]"
) {
    FakeClaudeHome home;
    QTemporaryDir  work;
    QDir(home.dir.path()).mkpath("jobs");
    // A real repository, with the worktrees a session can leave behind.
    const QString repo = QFileInfo(work.path()).canonicalFilePath() + "/repo";
    QDir().mkpath(repo);
    auto git = [&](const QStringList &args) {
        QProcess p;
        p.setWorkingDirectory(repo);
        p.start(
            "git",
            QStringList{"-c", "user.name=t", "-c", "user.email=t@t", "-c", "commit.gpgsign=false"} +
                args
        );
        REQUIRE(p.waitForFinished(20000));
        REQUIRE(p.exitCode() == 0);
        return QString::fromUtf8(p.readAllStandardOutput());
    };
    git({"init", "-q", "-b", "master"});
    writeFile(repo + "/a.txt", "a\n");
    git({"add", "a.txt"});
    git({"commit", "-q", "-m", "a"});
    const QString trees = repo + "/.claude/worktrees";
    auto          add   = [&](const QString &name) {
        git({"worktree", "add", "-q", "-b", "wt-" + name, trees + "/" + name});
        return trees + "/" + name;
    };
    const QString inJob     = add("job");     // the job's own, dirty
    const QString inRecords = add("records"); // only in the transcript, left, unpushed
    const QString inSub     = add("sub");     // a subagent's
    const QString inUse     = add("busy");    // entered, but a live session works in it
    const QString foreign   = add("foreign"); // another session's, not msga's
    writeFile(inJob + "/scratch.txt", "uncommitted\n");
    writeFile(inRecords + "/b.txt", "b\n");
    {
        QProcess p;
        p.setWorkingDirectory(inRecords);
        p.start(
            "git",
            {"-c",
             "user.name=t",
             "-c",
             "user.email=t@t",
             "-c",
             "commit.gpgsign=false",
             "commit",
             "-q",
             "-am",
             "x",
             "--allow-empty"}
        );
        REQUIRE(p.waitForFinished(20000));
    }
    writeFile(inRecords + "/a.txt", "changed\n");

    // Claude Code: `rm` keeps the dirty worktree (and the job), as it does.
    const QString cli = work.path() + "/claude";
    writeFile(cli, R"SH(#!/bin/sh
if [ "$1" = auth ]; then echo '{"loggedIn":true,"authMethod":"claude.ai"}'; exit 0; fi
printf '%s\n' "$*" >> "$CLAUDE_CONFIG_DIR/calls.log"
if [ "$1" = rm ]; then
  echo "kept $2 — its worktree is still at somewhere"
  echo "  it has uncommitted changes"
  exit 1
fi
)SH");
    QFile(cli).setPermissions(QFile(cli).permissions() | QFileDevice::ExeOwner);
    const QString mine       = "aaaa1111-0000-4000-8000-000000000001";
    const QString theirs     = "cccc3333-0000-4000-8000-000000000003";
    const QString busy       = "eeee5555-0000-4000-8000-000000000005";
    const QString transcript = home.dir.path() + "/projects/-src-app/" + mine + ".jsonl";
    auto          record     = [](const QString &path, const QString &branch, const QString &from) {
        return QJsonDocument(
                   QJsonObject{
                       {"type", "worktree-state"},
                       {"worktreeSession",
                        QJsonObject{
                            {"worktreePath", path},
                            {"worktreeBranch", branch},
                            {"originalCwd", from}
                        }}
                   }
               ).toJson(QJsonDocument::Compact) +
               "\n";
    };
    // Entered, then left (ExitWorktree(keep) records a null session); entered
    // one a live session works in; the main checkout named as if one.
    writeFile(
        transcript,
        prompt("work in a worktree", "2026-09-25T10:00:00.000Z") +
            record(inRecords, "wt-records", repo) +
            "{\"type\":\"worktree-state\",\"worktreeSession\":null}\n" +
            record(inUse, "wt-busy", repo) + record(repo, "master", repo)
    );
    writeFile(
        home.dir.path() + "/projects/-src-app/" + mine + "/subagents/agent-a1.meta.json",
        QJsonDocument(QJsonObject{{"worktreePath", inSub}, {"worktreeBranch", "wt-sub"}}).toJson()
    );
    std::vector<qint64> workers;
    int                 n = 0;
    for (const QString &sid : {mine, theirs}) {
        const QString short8 = sid.left(8);
        writeFile(
            home.dir.path() + "/jobs/" + short8 + "/state.json",
            QJsonDocument(
                QJsonObject{
                    {"state", "done"},
                    {"sessionId", sid},
                    {"cwd", sid == mine ? inJob : foreign},
                    {"name", short8},
                    {"linkScanPath",
                     sid == mine ? transcript
                                 : home.dir.path() + "/projects/-src-app/" + sid + ".jsonl"},
                    {"worktreePath", sid == mine ? inJob : foreign},
                    {"worktreeBranch", sid == mine ? "wt-job" : "wt-foreign"},
                    {"originCwd", repo},
                }
            )
                .toJson()
        );
        qint64 pid = 0;
        REQUIRE(QProcess::startDetached("sleep", {"60"}, {}, &pid));
        workers.push_back(pid);
        writeFile(
            home.dir.path() + QStringLiteral("/sessions/w%1.json").arg(++n),
            QJsonDocument(
                QJsonObject{
                    {"pid", pid},
                    {"sessionId", sid},
                    {"jobId", short8},
                    {"kind", "bg"},
                    {"status", "idle"},
                    {"cwd", sid == mine ? inJob : foreign}
                }
            ).toJson()
        );
    }
    // A terminal's session working in `inUse`.
    qint64 terminal = 0;
    REQUIRE(QProcess::startDetached("sleep", {"60"}, {}, &terminal));
    writeFile(
        home.dir.path() + "/sessions/t.json",
        QJsonDocument(
            QJsonObject{
                {"pid", terminal},
                {"sessionId", busy},
                {"cwd", inUse + "/src"},
                {"kind", "interactive"},
                {"entrypoint", "cli"},
                {"status", "idle"}
            }
        )
            .toJson()
    );
    const QString known = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                          "/claude-code/known-sessions.json";
    writeFile(
        known,
        QJsonDocument(
            QJsonObject{{"sessions", QJsonArray{}}, {"started", QJsonArray{mine}}}
        ).toJson()
    );
    auto calls = [&] {
        QFile f(home.dir.path() + "/calls.log");
        return f.open(QIODevice::ReadOnly)
                   ? QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts)
                   : QStringList{};
    };
    auto branches = [&] { return git({"branch", "--format=%(refname:short)"}).split('\n'); };

    claude_code::Backend backend(Credentials{cli});
    std::vector<Event>   events;
    rpl::lifetime        lt;
    backend.events() | rpl::on_next([&](Event e) { events.push_back(std::move(e)); }, lt);
    backend.connectRealtime();
    REQUIRE(collect(backend.loadConversations())[0].size() == 3);

    // Not msga's: it only leaves the list, worktree and all.
    backend.leaveConversation(ConversationId{theirs});
    QTest::qWait(1500);
    CHECK(calls().filter("cccc3333").isEmpty());
    CHECK(isProcessAlive(workers[1]));
    CHECK(QFileInfo::exists(foreign));

    // msga's: `claude rm` keeps the dirty worktree; msga deletes it once the
    // worker is gone — and the ones only the records name, and the subagent's.
    backend.leaveConversation(ConversationId{mine});
    REQUIRE(QTest::qWaitFor([&] { return calls().contains("rm aaaa1111"); }, 5000));
    CHECK(
        QTest::qWaitFor(
            [&] {
                return !QFileInfo::exists(inJob) && !QFileInfo::exists(inRecords) &&
                       !QFileInfo::exists(inSub);
            },
            30000
        )
    );
    CHECK(QTest::qWaitFor([&] { return !branches().contains("wt-sub"); }, 10000));
    QTest::qWait(1000); // the last git, gone from the list, may not have exited yet
    CHECK(!isProcessAlive(workers[0]));
    const QStringList left = branches();
    CHECK_FALSE(left.contains("wt-job"));
    CHECK_FALSE(left.contains("wt-records"));
    CHECK_FALSE(left.contains("wt-sub"));
    // Kept: the one in use, the main checkout and its branch, the other session's.
    CHECK(left.contains("wt-busy"));
    CHECK(left.contains("master"));
    CHECK(left.contains("wt-foreign"));
    CHECK(QFileInfo::exists(inUse));
    CHECK(QFileInfo::exists(repo + "/a.txt"));
    CHECK(QFileInfo::exists(foreign));
    const QString list = git({"worktree", "list", "--porcelain"});
    CHECK_FALSE(list.contains(inJob));
    CHECK_FALSE(list.contains(inRecords));
    CHECK_FALSE(list.contains(inSub));
    // The transcripts stay, the subagents' too.
    CHECK(QFileInfo::exists(transcript));
    CHECK(
        QFileInfo::exists(
            home.dir.path() + "/projects/-src-app/" + mine + "/subagents/agent-a1.meta.json"
        )
    );
    // Nothing failed, so nothing to tell.
    CHECK(std::none_of(events.begin(), events.end(), [](const Event &e) {
        return std::holds_alternative<EvNotice>(e);
    }));
    CHECK(calls().filter("stop").isEmpty());

    for (const qint64 pid : workers)
        signalProcess(pid, true);
    signalProcess(terminal, true);
}
#endif

TEST_CASE("files sent to a session go with its prompt", "[claude][backend]") {
    FakeClaudeHome home;
    QTemporaryDir  work;
    {
        QFile f(home.dir.path() + "/.claude.json");
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(
                    QJsonObject{
                        {"projects",
                         QJsonObject{
                             {QDir(work.path()).absolutePath(),
                              QJsonObject{{"hasTrustDialogAccepted", true}}}
                         }},
                    }
        )
                    .toJson());
    }
    // The CLI only notes what it was asked to do.
    const QString cli = work.path() + "/claude";
    {
        QFile f(cli);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(
            "#!/bin/sh\n[ \"$1\" = auth ] && exit 1\n" // "can't tell": a login isn't required
            "printf '%s\\n' \"$*\" >> \"$CLAUDE_CONFIG_DIR/calls.log\"\nexit 1\n"
        );
        f.setPermissions(f.permissions() | QFileDevice::ExeOwner);
    }
    QImage img(4, 4, QImage::Format_RGB32);
    img.fill(Qt::blue);
    REQUIRE(img.save(work.path() + "/pasted.png"));

    claude_code::Backend backend(Credentials{cli});
    std::vector<Event>   events;
    rpl::lifetime        lt;
    backend.events() | rpl::on_next([&](Event e) { events.push_back(std::move(e)); }, lt);
    ConversationId conv;
    backend.startAgentSession(work.path(), false, {}, [&](ConversationId id) { conv = id; }, {});
    REQUIRE(QTest::qWaitFor([&] { return !conv.value.isEmpty(); }, 5000));

    std::optional<bool> ok;
    backend.uploadFiles(
        conv, {work.path() + "/pasted.png"}, "look", std::nullopt, [&](bool s, QString) { ok = s; }
    );
    REQUIRE(ok == true);
    // msga's copy of it shows the picture, not the mention (and so takes the
    // place of the Session's file-message ghost).
    const auto copy = std::find_if(events.begin(), events.end(), [&](const Event &e) {
        const auto *n = std::get_if<EvMessageNew>(&e);
        return n && n->conv == conv;
    });
    REQUIRE(copy != events.end());
    const Message &m = std::get<EvMessageNew>(*copy).msg;
    CHECK(m.text.text == "look");
    REQUIRE(m.files.size() == 1);
    CHECK(m.files[0].name == "pasted.png");
    // Claude Code gets the cached copy's mention before the text.
    const QString log = home.dir.path() + "/calls.log";
    QString       call;
    // The whole line: the shell creates the file before it writes to it.
    REQUIRE(
        QTest::qWaitFor(
            [&] {
                QFile f(log);
                call = f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
                return call.endsWith('\n');
            },
            5000
        )
    );
    CHECK(call.contains("-- @" + uploadsDir() + '/'));
    CHECK(call.trimmed().endsWith("/pasted.png look"));

    // A file that can't be read fails the send, and only through `done`: the
    // Session says "Upload failed" itself.
    events.clear();
    std::optional<bool> failed;
    backend.uploadFiles(conv, {work.path() + "/gone.png"}, "x", std::nullopt, [&](bool s, QString) {
        failed = !s;
    });
    CHECK(failed == true);
    CHECK(events.empty());
    // Nor does a refused one announce itself.
    backend.uploadFiles(
        ConversationId{"nope"},
        {work.path() + "/pasted.png"},
        "",
        std::nullopt,
        [&](bool s, QString) { failed = !s; }
    );
    CHECK(failed == true);
    CHECK_FALSE(std::any_of(events.begin(), events.end(), [](const Event &e) {
        return std::holds_alternative<EvSendFailed>(e);
    }));
}

TEST_CASE("a session renamed in msga is titled by that name", "[claude][backend]") {
    FakeClaudeHome home;
    home.writeSession("idle");
    home.append(prompt("hi", "2026-09-25T10:00:00.000Z"));
    const ConversationId conv{"S1"};
    const UserId         peer{"claude:S1"};
    auto                 nameOf = [&](claude_code::Backend &b) {
        const auto users = collect(b.loadUsers());
        for (const auto &u : users[0])
            if (u.id == peer)
                return u.displayName;
        return QString();
    };
    {
        claude_code::Backend backend(Credentials{});
        std::vector<Event>   events;
        rpl::lifetime        lt;
        backend.events() | rpl::on_next([&](Event e) { events.push_back(std::move(e)); }, lt);
        backend.connectRealtime();
        REQUIRE(nameOf(backend) == "app-1");
        backend.setConversationLocalName(conv, " Refunds ");
        // The DM's title comes from its peer: pushed right away.
        CHECK(std::any_of(events.begin(), events.end(), [&](const Event &e) {
            const auto *u = std::get_if<EvUserChanged>(&e);
            return u && u->user.id == peer && u->user.displayName == "Refunds";
        }));
        const auto convs = collect(backend.loadConversations())[0];
        REQUIRE(convs.size() == 1);
        CHECK(convs[0].localName == "Refunds");
        CHECK(convs[0].name == "app-1"); // Claude Code's own name, the dialog's placeholder
    }
    claude_code::Backend backend(Credentials{});
    backend.connectRealtime();
    CHECK(nameOf(backend) == "Refunds"); // kept across restarts
    backend.setConversationLocalName(conv, "");
    CHECK(nameOf(backend) == "app-1");
}

TEST_CASE("your name and picture are kept by msga", "[claude][backend][profile]") {
    FakeClaudeHome home;
    const QString  appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QFile::remove(appData + "/claude-code/profile.json");
    QTemporaryDir pics;
    const QString pic = pics.path() + "/me.png";
    {
        QFile f(pic);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("not really a png");
    }
    auto meOf = [](claude_code::Backend &b) {
        const auto users = collect(b.loadUsers());
        return users[0][0]; // you come first
    };
    {
        claude_code::Backend backend(Credentials{});
        CHECK_FALSE(backend.capabilities().profileContact);
        const User before = meOf(backend);
        CHECK(before.avatarUrl.isEmpty());

        bool ok = false;
        backend.updateProfile({{"display_name", "Robin"}}, [&](bool o, QString) { ok = o; });
        CHECK(ok);
        QString url;
        backend.setPhoto(pic, [&](bool o, QString, QString u) {
            ok  = o;
            url = u;
        });
        CHECK(ok);
        CHECK(url.startsWith("file:"));
        CHECK(QFile::exists(QUrl(url).toLocalFile()));
    }
    claude_code::Backend backend(Credentials{});
    const User           after = meOf(backend);
    CHECK(after.displayName == "Robin");
    CHECK(!after.avatarUrl.isEmpty());
    MyProfile loaded;
    backend.loadMyProfile([&](MyProfile p) { loaded = p; });
    CHECK(loaded.displayName == "Robin");
    CHECK(loaded.avatarUrl == after.avatarUrl);
    QFile::remove(appData + "/claude-code/profile.json");
    QFile::remove(QUrl(after.avatarUrl).toLocalFile());
}

TEST_CASE("an idle session with a background command running is not typing", "[claude][backend]") {
    FakeClaudeHome home;
    home.writeSession("shell");
    home.append(
        prompt("watch it", "2026-09-25T10:00:00.000Z") +
        assistantText("Watching.", "2026-09-25T10:00:01.000Z") + turnEnd("2026-09-25T10:00:02.000Z")
    );
    claude_code::Backend backend(Credentials{});
    std::vector<Event>   events;
    rpl::lifetime        lt;
    backend.events() | rpl::on_next([&](Event e) { events.push_back(std::move(e)); }, lt);
    backend.connectRealtime();
    const auto users = collect(backend.loadUsers());
    const auto peer  = std::find_if(users[0].begin(), users[0].end(), [](const User &u) {
        return u.id == UserId{"claude:S1"};
    });
    REQUIRE(peer != users[0].end());
    CHECK_FALSE(peer->isActive); // no "working" dot…
    CHECK(peer->unavailable);    // …but the yellow one: a terminal holds it
    CHECK(peer->statusText == "Running a background command");
    // Its teammate (the generalist) is yellow too: nothing of it works.
    const auto mate = std::find_if(users[0].begin(), users[0].end(), [](const User &u) {
        return u.id == UserId{"claude:agent"};
    });
    REQUIRE(mate != users[0].end());
    CHECK_FALSE(mate->isActive);
    CHECK(mate->unavailable);
    CHECK_FALSE(collect(backend.loadPresence(UserId{"claude:S1"}))[0]);
    QTest::qWait(3500); // the typing pump runs every 3 s while anything is busy
    CHECK(std::none_of(events.begin(), events.end(), [](const Event &e) {
        return std::holds_alternative<EvTyping>(e);
    }));
}

TEST_CASE(
    "a background job stuck on \"working\" after its turn ended is not typing",
    "[claude][backend][bg]"
) {
    // Seen live 2026-09-25: a subagent's notification left the job reading
    // "working" (inFlight.queued 1) while its worker sat idle for good.
    FakeClaudeHome home;
    home.append(
        prompt("look for duplicates", "2026-09-25T10:00:00.000Z") +
        assistantText("Done.", "2026-09-25T10:00:01.000Z") + turnEnd("2026-09-25T10:00:02.000Z")
    );
    QDir(home.dir.path()).mkpath("jobs/S1");
    {
        QFile f(home.dir.path() + "/jobs/S1/state.json");
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(
                    QJsonObject{
                        {"state", "working"},
                        {"sessionId", "S1"},
                        {"cwd", "/src/app"},
                        {"name", "duplicates"},
                        {"linkScanPath", home.transcript},
                    }
        )
                    .toJson());
    }
    {
        QFile f(home.dir.path() + "/sessions/1.json");
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(
                    QJsonObject{
                        {"pid", QCoreApplication::applicationPid()}, // alive
                        {"sessionId", "S1"},
                        {"cwd", "/src/app"},
                        {"kind", "bg"},
                        {"status", "idle"},
                        {"entrypoint", "cli"},
                    }
        )
                    .toJson());
    }
    claude_code::Backend backend(Credentials{});
    std::vector<Event>   events;
    rpl::lifetime        lt;
    backend.events() | rpl::on_next([&](Event e) { events.push_back(std::move(e)); }, lt);
    backend.connectRealtime();
    const auto users = collect(backend.loadUsers());
    const auto peer  = std::find_if(users[0].begin(), users[0].end(), [](const User &u) {
        return u.id == UserId{"claude:S1"};
    });
    REQUIRE(peer != users[0].end());
    CHECK_FALSE(peer->isActive); // no "working" dot
    QTest::qWait(3500);          // the typing pump runs every 3 s while anything is busy
    CHECK(std::none_of(events.begin(), events.end(), [](const Event &e) {
        return std::holds_alternative<EvTyping>(e);
    }));
}

TEST_CASE(
    "a reply to a background job's question works though the job still reads \"blocked\"",
    "[claude][backend][bg]"
) {
    // Seen live 2026-09-26: a turn ending on a question left the job "blocked",
    // and the reply's turn never flipped it back to "working" — no dot, no
    // "thinking" for two minutes of work. The worker read "busy" throughout
    // (a subagent of its was running).
    FakeClaudeHome home;
    home.append(
        prompt("look for duplicates", "2026-09-25T10:00:00.000Z") +
        assistantText("Copy it to master?", "2026-09-25T10:00:01.000Z") +
        turnEnd("2026-09-25T10:00:02.000Z")
    );
    QDir(home.dir.path()).mkpath("jobs/S1");
    {
        QFile f(home.dir.path() + "/jobs/S1/state.json");
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(
                    QJsonObject{
                        {"state", "blocked"},
                        {"detail", "Copy it to master?"},
                        {"updatedAt", "2026-09-25T10:00:02.500Z"},
                        {"sessionId", "S1"},
                        {"cwd", "/src/app"},
                        {"name", "duplicates"},
                        {"linkScanPath", home.transcript},
                    }
        )
                    .toJson());
    }
    const auto writeWorker = [&](const QString &status) {
        QFile f(home.dir.path() + "/sessions/1.json");
        REQUIRE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QJsonDocument(
                    QJsonObject{
                        {"pid", QCoreApplication::applicationPid()}, // alive
                        {"sessionId", "S1"},
                        {"cwd", "/src/app"},
                        {"kind", "bg"},
                        {"status", status},
                        {"entrypoint", "cli"},
                    }
        )
                    .toJson());
    };
    writeWorker("busy");
    claude_code::Backend backend(Credentials{});
    std::vector<Event>   events;
    rpl::lifetime        lt;
    backend.events() | rpl::on_next([&](Event e) { events.push_back(std::move(e)); }, lt);
    backend.connectRealtime();
    const auto peer = [&] {
        const auto users = collect(backend.loadUsers());
        const auto it    = std::find_if(users[0].begin(), users[0].end(), [](const User &u) {
            return u.id == UserId{"claude:S1"};
        });
        REQUIRE(it != users[0].end());
        return *it;
    };
    const auto active = [&]() -> bool {
        return collect(backend.loadPresence(UserId{"claude:S1"}))[0];
    };
    const auto typing = [&]() -> std::optional<EvTyping> {
        for (auto it = events.rbegin(); it != events.rend(); ++it)
            if (const auto *t = std::get_if<EvTyping>(&*it); t && !t->threadRoot)
                return *t;
        return std::nullopt;
    };

    // The question waits: no dot, whatever the busy worker says.
    CHECK_FALSE(peer().isActive);
    CHECK(peer().statusText == "Waiting for you");

    // The reply's turn is under way: working, thinking since its prompt.
    home.append(
        prompt("copy changes to master", "2026-09-25T10:05:00.000Z") +
        toolUse("b1", "Bash", {{"command", "git apply"}}, "2026-09-25T10:05:01.000Z")
    );
    REQUIRE(QTest::qWaitFor(active, 5000));
    CHECK(peer().statusText == "Working");
    REQUIRE(QTest::qWaitFor([&] { return typing().has_value(); }, 5000));
    CHECK(typing()->thinkingSinceMs == 1790330700000);

    // A worker gone idle with the turn left open (interrupted): not working.
    writeWorker("idle");
    REQUIRE(QTest::qWaitFor([&] { return !active(); }, 5000));
    writeWorker("busy");
    REQUIRE(QTest::qWaitFor(active, 5000));

    // The turn ends: back to waiting.
    home.append(
        assistantText("Copied.", "2026-09-25T10:06:00.000Z") + turnEnd("2026-09-25T10:06:01.000Z")
    );
    REQUIRE(QTest::qWaitFor([&] { return !active(); }, 5000));
}

TEST_CASE("a background job's suggested reply is offered only while it asks", "[claude][roster]") {
    // Claude Code writes it on a turn that ends on a question, and its own
    // list offers it only while the job reads "blocked" (verified in 2.1.283).
    const auto asking = parseBackgroundJob(
        R"({"state":"blocked","tempo":"blocked","sessionId":"S1","needs":"confirm the copy",
            "suggestedReply":"copy changes to 'master'"})"
    );
    REQUIRE(asking);
    CHECK(asking->suggestedReply == "copy changes to 'master'");

    // A stopped job keeps a stale one (seen live).
    const auto stopped = parseBackgroundJob(
        R"({"state":"stopped","tempo":"idle","sessionId":"S2","suggestedReply":"finish the rest"})"
    );
    REQUIRE(stopped);
    CHECK(stopped->suggestedReply.isEmpty());

    // Multiple-choice questions are answered by picking, not typing.
    const auto choosing = parseBackgroundJob(
        R"({"state":"blocked","tempo":"blocked","sessionId":"S3","suggestedReply":"yes",
            "block":{"questions":[{"question":"Which?","options":[]}]}})"
    );
    REQUIRE(choosing);
    CHECK(choosing->suggestedReply.isEmpty());
}

TEST_CASE(
    "the suggested reply rides on the conversation until the reply is under way",
    "[claude][backend][bg]"
) {
    FakeClaudeHome home;
    home.append(
        prompt("look for duplicates", "2026-09-25T10:00:00.000Z") +
        assistantText("Copy it to master?", "2026-09-25T10:00:01.000Z") +
        turnEnd("2026-09-25T10:00:02.000Z")
    );
    QDir(home.dir.path()).mkpath("jobs/S1");
    {
        QFile f(home.dir.path() + "/jobs/S1/state.json");
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(
                    QJsonObject{
                        {"state", "blocked"},
                        {"tempo", "blocked"},
                        {"needs", "confirm the copy"},
                        {"suggestedReply", "copy changes to 'master'"},
                        {"updatedAt", "2026-09-25T10:00:02.500Z"},
                        {"sessionId", "S1"},
                        {"cwd", "/src/app"},
                        {"name", "duplicates"},
                        {"linkScanPath", home.transcript},
                    }
        )
                    .toJson());
    }
    {
        QFile f(home.dir.path() + "/sessions/1.json");
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(
                    QJsonObject{
                        {"pid", QCoreApplication::applicationPid()}, // alive
                        {"sessionId", "S1"},
                        {"cwd", "/src/app"},
                        {"kind", "bg"},
                        {"status", "busy"},
                        {"entrypoint", "cli"},
                    }
        )
                    .toJson());
    }
    Credentials creds;
    creds.claudePath = QStringLiteral("claude"); // else nothing is writable from here
    claude_code::Backend backend(creds);
    std::vector<Event>   events;
    rpl::lifetime        lt;
    backend.events() | rpl::on_next([&](Event e) { events.push_back(std::move(e)); }, lt);
    backend.connectRealtime();

    const auto convs = collect(backend.loadConversations());
    REQUIRE(convs.size() == 1);
    REQUIRE(convs[0].size() == 1);
    CHECK(convs[0][0].readOnlyReason.isEmpty());
    CHECK(convs[0][0].suggestedReply == "copy changes to 'master'");

    // The reply, typed in a terminal, is under way: nothing left to suggest.
    const auto lastSuggestion = [&]() -> std::optional<QString> {
        for (auto it = events.rbegin(); it != events.rend(); ++it)
            if (const auto *c = std::get_if<EvChannelCreated>(&*it); c && c->conv.id.value == "S1")
                return c->conv.suggestedReply;
        return std::nullopt;
    };
    home.append(
        prompt("copy changes to master", "2026-09-25T10:05:00.000Z") +
        toolUse("b1", "Bash", {{"command", "ls"}}, "2026-09-25T10:05:01.000Z")
    );
    REQUIRE(QTest::qWaitFor([&] { return lastSuggestion() == QString(); }, 5000));
}

#if !defined(Q_OS_WIN)
// ── Team roles ────────────────────────────────────────────────────────────────

TEST_CASE("the team: a generalist first, then the specialists", "[claude][roles]") {
    const auto &all = builtInRoles();
    REQUIRE(all.size() == 5);
    CHECK(all[0].id == "generalist");
    CHECK(appendedPrompt(all[0]).isEmpty()); // plain Claude Code
    for (size_t i = 1; i < all.size(); ++i)
        CHECK(appendedPrompt(all[i]).startsWith(
            "# Your role: " + all[i].promptName + " (msga: " + all[i].id + ")\n"
        ));
}

TEST_CASE("a session's role is read back from its recorded system prompt", "[claude][roles]") {
    const Role      &engineer = builtInRoles()[1];
    const QJsonArray withRole{"You are an interactive agent…", appendedPrompt(engineer)};
    CHECK(roleInSystemPrompt(withRole).id == "engineer");
    CHECK(roleInSystemPrompt(QJsonArray{"You are an interactive agent…"}).id.isEmpty());
    // Before ids were written: a built-in's English name alone.
    CHECK(roleInSystemPrompt(QJsonArray{"# Your role: Engineer\nYou are…"}).id == "engineer");
    CHECK(roleInSystemPrompt(QJsonArray{"# Your role: Astronaut\nFly."}).id.isEmpty());
    // An added teammate, with spaces in its name.
    const RoleMark added =
        roleInSystemPrompt(QJsonArray{"# Your role: Data analyst (msga: data-analyst)\nDig."});
    CHECK(added.id == "data-analyst");
    CHECK(added.name == "Data analyst");

    // In a transcript: the prompt_snapshot attachment Claude Code records.
    const QByteArray snapshot = line(
        {{"type", "attachment"},
         {"timestamp", "2026-09-25T10:00:02.000Z"},
         {"attachment", QJsonObject{{"type", "prompt_snapshot"}, {"systemPrompt", withRole}}}}
    );
    TranscriptParser p;
    p.feed(prompt("hello", "2026-09-25T10:00:01.000Z") + snapshot);
    CHECK(p.role() == "engineer");
    TranscriptParser plain;
    plain.feed(prompt("hello", "2026-09-25T10:00:01.000Z"));
    CHECK(plain.role().isEmpty());

    // Raw bytes, as "Find a session" reads a transcript's ends — even cut off
    // right after the header line.
    CHECK(roleInTranscriptBytes(snapshot).id == "engineer");
    const qsizetype at = snapshot.indexOf("engineer)\\n");
    CHECK(roleInTranscriptBytes(snapshot.left(at + 11)).id == "engineer");
    CHECK(roleInTranscriptBytes(snapshot.left(at + 5)).id.isEmpty()); // the line isn't whole
    CHECK(roleInTranscriptBytes(prompt("# Your role: Engineer", "2026-09-25T10:00:01.000Z"))
              .id.isEmpty()); // typed, not a part of the system prompt

    CatalogEntry e;
    REQUIRE(catalogEntryFrom(prompt("hello", "2026-09-25T10:00:01.000Z") + snapshot, "\n", e));
    CHECK(e.role == "engineer");
}

TEST_CASE("teammates are added, edited, restored and removed", "[claude][roles]") {
    QTemporaryDir dir;
    Team          team(dir.path());
    REQUIRE(team.listed().size() == 5);

    Role copy;
    copy.name        = "Copy writer!";
    copy.description = "Writes copy.";
    copy.glyph       = "pen-tool";
    copy.color       = QColor("#0e8c9a");
    copy.prompt      = "You write copy.";
    QString       error;
    const QString id = team.save(copy, &error);
    REQUIRE(id == "copy-writer");
    CHECK(team.find(id)->avatarUrl.endsWith("/avatars/pen-tool-0e8c9a.svg"));
    CHECK(QFileInfo::exists(QUrl(team.find(id)->avatarUrl).toLocalFile()));
    CHECK(
        appendedPrompt(*team.find(id)) ==
        "# Your role: Copy writer! (msga: copy-writer)\nYou write copy."
    );
    // A second one by the same name gets an id of its own.
    CHECK(team.save(copy, &error) == "copy-writer-2");
    CHECK(team.save(Role{}, &error).isEmpty()); // no name
    CHECK_FALSE(error.isEmpty());

    // Renaming keeps the id — the one its sessions carry.
    Role renamed = *team.find(id);
    renamed.name = "Writer";
    CHECK(team.save(renamed, &error) == id);
    CHECK(appendedPrompt(*team.find(id)).startsWith("# Your role: Writer (msga: copy-writer)"));

    // A built-in, edited then restored.
    Role engineer   = *team.find("engineer");
    engineer.prompt = "Only Rust.";
    REQUIRE(team.save(engineer, &error) == "engineer");
    CHECK(team.find("engineer")->edited);
    CHECK(team.find("engineer")->avatarUrl == "qrc:/roles/engineer.svg"); // same picture
    CHECK(
        appendedPrompt(*team.find("engineer")) ==
        "# Your role: Engineer (msga: engineer)\nOnly Rust."
    );
    CHECK_FALSE(team.remove("engineer")); // built-ins stay
    CHECK(team.restore("engineer"));
    CHECK_FALSE(team.find("engineer")->edited);
    CHECK(team.find("engineer")->prompt == builtInRoles()[1].prompt);

    // The team as subagent types: every role with a prompt, under its id, with
    // that prompt — but none shadowing one of Claude Code's own types.
    Role plan;
    plan.name   = "Plan";
    plan.prompt = "You plan.";
    REQUIRE(team.save(plan, &error) == "plan");
    const QJsonObject agents =
        QJsonDocument::fromJson(subagentsJson(team.listed()).toUtf8()).object();
    CHECK(
        agents.keys() ==
        QStringList{
            "copy-writer", "copy-writer-2", "designer", "engineer", "marketer", "researcher"
        }
    );
    CHECK(agents["engineer"]["prompt"].toString() == appendedPrompt(*team.find("engineer")));
    CHECK(
        agents["copy-writer"]["description"].toString() ==
        "Writer, a teammate (mentioned as @claude:role:copy-writer). Writes copy."
    );
    CHECK(subagentsJson({builtInRoles()[0]}).isEmpty()); // the Generalist adds nothing
    team.remove("plan");

    // Removed: off the list, still known to its sessions.
    CHECK(team.remove(id));
    CHECK(team.listed().size() == 6);
    REQUIRE(team.find(id));
    CHECK(team.find(id)->removed);
    CHECK(team.resolve(id).name == "Writer");

    // All of it read back from disk.
    Team again(dir.path());
    CHECK(again.find(id)->removed);
    CHECK(again.find(id)->name == "Writer");
    CHECK(again.find("copy-writer-2")->prompt == "You write copy.");
    CHECK_FALSE(again.find("engineer")->edited);
    // Added teammates come after the built-ins, in the order they were added.
    CHECK(again.roles()[5].id == id);

    // A role nobody here knows: a former teammate, named by its sessions.
    CHECK(again.noteFormer("astronaut", "Astronaut"));
    CHECK_FALSE(again.noteFormer("astronaut", "Astronaut"));
    const Role former = again.resolve("astronaut");
    CHECK(former.former);
    CHECK(former.name == "Astronaut");
    CHECK_FALSE(former.avatarUrl.isEmpty());
    // …and its id is never given to a new teammate.
    Role astro;
    astro.name = "Astronaut";
    CHECK(again.save(astro, &error) == "astronaut-2");
}

TEST_CASE(
    "sending runs background sessions: start, then stop + resume per turn, queued",
    "[claude][backend][bg]"
) {
    FakeClaudeHome home;
    QTemporaryDir  work, untrusted;
    // No teammates left over from an earlier run.
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/claude-code/team")
        .removeRecursively();
    QDir(home.dir.path()).mkpath("jobs");
    QDir(home.dir.path()).mkpath("projects/-fake");
    // Claude Code trusts `work` (a parent folder would do too), not `untrusted`.
    {
        QFile f(home.dir.path() + "/.claude.json");
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(
                    QJsonObject{
                        {"projects",
                         QJsonObject{
                             {QDir(work.path()).absolutePath(),
                              QJsonObject{{"hasTrustDialogAccepted", true}}}
                         }},
                    }
        )
                    .toJson());
    }
    // Stand-in for the CLI, following what Claude Code 2.1.282 was seen doing:
    // `--bg … -- <prompt>` creates a job + worker (a real process: a stop waits
    // for it to exit) and answers, printing "backgrounded · <short>"; `stop
    // <short>` ends the worker; `--bg --resume <id> -- <prompt>` continues —
    // or, with a copy-next file present, starts a copy of the session (records
    // repeated, uuids and all) and continues there. Every call is logged to
    // calls.log.
    const QString cli = work.path() + "/claude";
    {
        QFile f(cli);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(R"SH(#!/bin/sh
H="$CLAUDE_CONFIG_DIR"
if [ "$1" = auth ]; then echo '{"loggedIn":true,"authMethod":"claude.ai"}'; exit 0; fi
if [ "$1" = attach ]; then # typing into a live worker: see the terminal UI test
  printf '%s\n' "$*" >> "$H/attach.log"
  [ -n "$FAKE_ATTACH" ] && exec "$FAKE_ATTACH" "$2"
  echo "no such session"; exit 1
fi
printf '%s\n' "$*" >> "$H/calls.log" # echo would expand \n
if [ "$1" = stop ]; then
  rm -f "$H/sessions/w$2.json"
  kill $(cat "$H/wpid-$2" 2>/dev/null) 2>/dev/null
  # No `sed -i`: BSD sed (macOS) reads its next argument as a backup suffix.
  sed 's/"state":"[a-z]*"/"state":"stopped"/' "$H/jobs/$2/state.json" > "$H/state.tmp" &&
    mv "$H/state.tmp" "$H/jobs/$2/state.json"
  echo "stopped $2"; exit 0
fi
sid=""; prompt=""; copied=""
while [ $# -gt 0 ]; do
  case "$1" in
    --resume) sid="$2"; shift ;;
    --) prompt="$2"; shift ;;
  esac; shift
done
if [ -n "$sid" ] && [ -f "$H/copy-next" ]; then
  rm -f "$H/copy-next"; copied="$H/projects/-fake/$sid.jsonl"; sid=""
fi
if [ -z "$sid" ]; then
  n=$(cat "$H/counter" 2>/dev/null || echo 0); n=$((n+1)); echo $n > "$H/counter"
  sid="abcdef1$n-0000-4000-8000-00000000000$n"
fi
short=$(echo "$sid" | cut -c1-8)
T="$H/projects/-fake/$sid.jsonl"
[ -n "$copied" ] && cp "$copied" "$T"
# Millisecond ISO time via perl, not `date +%3N`: BSD date (macOS) prints a literal "3N".
ts=$(perl -MTime::HiRes=time -MPOSIX=strftime -e '$t = time; printf "%s.%03dZ", strftime("%Y-%m-%dT%H:%M:%S", gmtime $t), ($t - int $t) * 1000')
# A fresh id per turn from /dev/urandom — there is no /proc on macOS, and ids that
# repeat across turns make the backend drop later answers as duplicates.
u=$(od -An -N16 -tx1 /dev/urandom | tr -d ' \n')
mkdir -p "$H/jobs/$short"
echo "{\"state\":\"done\",\"sessionId\":\"$sid\",\"cwd\":\"$PWD\",\"name\":\"fake-$short\",\"linkScanPath\":\"$T\"}" > "$H/jobs/$short/state.json"
kill $(cat "$H/wpid-$short" 2>/dev/null) 2>/dev/null
sleep 60 </dev/null >/dev/null 2>&1 &
echo $! > "$H/wpid-$short"
echo "{\"pid\":$!,\"sessionId\":\"$sid\",\"kind\":\"bg\",\"status\":\"idle\"}" > "$H/sessions/w$short.json"
echo "{\"type\":\"user\",\"uuid\":\"$u-1\",\"timestamp\":\"$ts\",\"origin\":{\"kind\":\"human\"},\"message\":{\"content\":\"$prompt\"}}" >> "$T"
echo "{\"type\":\"assistant\",\"uuid\":\"$u-2\",\"timestamp\":\"$ts\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"echo $prompt\"}]}}" >> "$T"
echo "{\"type\":\"system\",\"subtype\":\"turn_duration\",\"uuid\":\"$u-3\",\"timestamp\":\"$ts\"}" >> "$T"
[ -n "$copied" ] && echo "Worker still running; started a copy of the session."
echo "backgrounded · $short"
)SH");
        f.setPermissions(f.permissions() | QFileDevice::ExeOwner);
    }

    claude_code::Backend backend(Credentials{cli});
    std::vector<Event>   events;
    rpl::lifetime        lt;
    backend.events() | rpl::on_next([&](Event e) { events.push_back(std::move(e)); }, lt);
    backend.connectRealtime();

    // Background sessions refuse untrusted folders: said up front.
    QString error;
    backend.startAgentSession(untrusted.path(), false, {}, {}, [&](QString e) { error = e; });
    CHECK(error.contains("trust"));

    ConversationId conv;
    backend.startAgentSession(work.path(), false, {}, [&](ConversationId id) { conv = id; }, {});
    REQUIRE(QTest::qWaitFor([&] { return !conv.value.isEmpty(); }, 5000));
    REQUIRE(conv.value.startsWith("new-")); // Claude Code picks the id on the first message
    const auto listed = collect(backend.loadConversations());
    CHECK(std::any_of(listed[0].begin(), listed[0].end(), [&](const Conversation &c) {
        return c.id == conv && c.readOnlyReason.isEmpty();
    }));

    auto sendText = [&](const char *text, bool *ok) {
        OutgoingMessage out;
        out.text = {text, {}};
        backend.sendMessage(conv, out, [ok](bool success, QString) { *ok = success; });
    };
    auto answered = [&](const QString &text) {
        return QTest::qWaitFor(
            [&] {
                for (const auto &e : events)
                    if (const auto *n = std::get_if<EvMessageNew>(&e);
                        n && n->conv == conv && n->msg.text.text == text)
                        return true;
                return false;
            },
            8000
        );
    };

    bool ok1 = false, ok2 = false, ok3 = false;
    sendText("first", &ok1);
    REQUIRE(answered("echo first"));
    CHECK(ok1);
    // Two more at once: the second waits for its turn, one prompt per turn.
    sendText("second", &ok2);
    sendText("third", &ok3);
    // The waiting one is a message of its own at once, and stays one across a
    // reload of the chat — the Session's optimistic copy wouldn't.
    CHECK(ok3);
    const auto ownTexts = [&] {
        QStringList texts;
        const auto  pages = collect(backend.loadHistory(conv, std::nullopt));
        for (const auto &m : pages[0].messages)
            if (m.author.value == "me")
                texts << m.text.text;
        return texts;
    };
    CHECK(ownTexts() == QStringList{"first", "second", "third"});
    REQUIRE(answered("echo second"));
    REQUIRE(answered("echo third"));
    CHECK(ok2);
    // …and gives way to its prompt in the transcript: no doubles.
    CHECK(ownTexts() == QStringList{"first", "second", "third"});

    QFile log(home.dir.path() + "/calls.log");
    REQUIRE(log.open(QIODevice::ReadOnly));
    const QStringList calls = QString::fromUtf8(log.readAll()).split('\n', Qt::SkipEmptyParts);
    const QString     sid   = "abcdef11-0000-4000-8000-000000000001";
    REQUIRE(calls.size() == 5);
    // A new session gets the team as subagent types, so "use @Engineer" works.
    CHECK(
        calls[0] == "--bg --disallowedTools AskUserQuestion --agents " +
                        subagentsJson(builtInRoles()) + " -- first"
    );
    CHECK(calls[1] == "stop abcdef11");                       // the worker idles on after a turn
    CHECK(calls[2] == "--bg --resume " + sid + " -- second"); // no flags: keeps its options
    CHECK(calls[3] == "stop abcdef11");
    CHECK(calls[4] == "--bg --resume " + sid + " -- third");
    log.close();

    // A message still waiting for its turn can be taken back.
    bool ok4 = false, ok5 = false;
    sendText("fourth", &ok4);
    sendText("fifth", &ok5);
    Ts fifth;
    for (const auto &e : events)
        if (const auto *n = std::get_if<EvMessageNew>(&e); n && n->msg.text.text == "fifth")
            fifth = n->msg.ts;
    REQUIRE_FALSE(fifth.isEmpty());
    REQUIRE(backend.canDeleteMessage(conv, fifth));
    backend.deleteMessage(conv, fifth);
    CHECK(std::any_of(events.begin(), events.end(), [&](const Event &e) {
        const auto *d = std::get_if<EvMessageDeleted>(&e);
        return d && d->ts == fifth;
    }));
    REQUIRE(answered("echo fourth"));
    QTest::qWait(500); // a turn for fifth would have started by now
    REQUIRE(log.open(QIODevice::ReadOnly));
    CHECK_FALSE(log.readAll().contains("fifth"));
    log.close();
    CHECK_FALSE(ownTexts().contains("fifth"));

    // One conversation for it, under its "+" id; the name comes from Claude Code.
    const auto after = collect(backend.loadConversations());
    CHECK(std::count_if(after[0].begin(), after[0].end(), [&](const Conversation &c) {
              return c.name == "fake-abcdef11";
          }) == 1);

    // Claude Code started a copy after all (a resume racing the old worker's
    // exit, seen 2026-09-25): the chat goes on in it — no failure, no history
    // twice, no second session or thread — and the next message resumes the copy.
    {
        QFile f(home.dir.path() + "/copy-next");
        REQUIRE(f.open(QIODevice::WriteOnly));
    }
    bool ok6 = false, ok7 = false;
    sendText("sixth", &ok6);
    REQUIRE(answered("echo sixth"));
    CHECK(ok6);
    CHECK_FALSE(std::any_of(events.begin(), events.end(), [&](const Event &e) {
        return std::holds_alternative<EvSendFailed>(e);
    }));
    CHECK(ownTexts() == QStringList{"first", "second", "third", "fourth", "sixth"});
    const auto copied = collect(backend.loadConversations());
    CHECK(std::count_if(copied[0].begin(), copied[0].end(), [&](const Conversation &c) {
              return c.name.startsWith("fake-abcdef1");
          }) == 1);
    sendText("seventh", &ok7);
    REQUIRE(answered("echo seventh"));
    REQUIRE(log.open(QIODevice::ReadOnly));
    const QString copyLog = QString::fromUtf8(log.readAll());
    log.close();
    CHECK(copyLog.contains("--bg --resume abcdef12-0000-4000-8000-000000000002 -- seventh"));
    CHECK(copyLog.contains("stop abcdef11\n")); // the original is stopped, not left idling

    // Skipping permission checks is a start option, saved with the session.
    ConversationId noChecks;
    backend.startAgentSession(work.path(), true, {}, [&](ConversationId id) { noChecks = id; }, {});
    REQUIRE(QTest::qWaitFor([&] { return !noChecks.value.isEmpty(); }, 5000));
    OutgoingMessage out;
    out.text = {"go", {}};
    backend.sendMessage(noChecks, out, {});
    REQUIRE(
        QTest::qWaitFor(
            [&] {
                QFile f(home.dir.path() + "/calls.log");
                return f.open(QIODevice::ReadOnly) &&
                       f.readAll().contains(
                           "--dangerously-skip-permissions --agents " +
                           subagentsJson(builtInRoles()).toUtf8() + " -- go"
                       );
            },
            8000
        )
    );
    // Let that launch finish before the backend goes away with it.
    CHECK(
        QTest::qWaitFor(
            [&] {
                for (const auto &e : events)
                    if (const auto *n = std::get_if<EvMessageNew>(&e);
                        n && n->conv == noChecks && n->msg.text.text == "echo go")
                        return true;
                return false;
            },
            8000
        )
    );
    // Claude gets the text as typed, not the parsed copy the fences and
    // backticks were stripped from.
    OutgoingMessage fenced;
    fenced.text         = {"quotes test code", {}};
    fenced.rawText      = "quotes ```test``` `code`";
    fenced.composerText = "quotes ```test``` `code`";
    backend.sendMessage(noChecks, fenced, {});
    CHECK(
        QTest::qWaitFor(
            [&] {
                QFile f(home.dir.path() + "/calls.log");
                return f.open(QIODevice::ReadOnly) &&
                       f.readAll().contains("quotes ```test``` `code`");
            },
            8000
        )
    );

    // A teammate: its few lines go after Claude Code's own prompt, its session
    // carries its role and picture, and Claude answers as the teammate.
    ConversationId engineer;
    backend.startAgentSession(
        work.path(), false, "engineer", [&](ConversationId id) { engineer = id; }, {}
    );
    REQUIRE(QTest::qWaitFor([&] { return !engineer.value.isEmpty(); }, 5000));
    const auto convs = collect(backend.loadConversations());
    CHECK(std::any_of(convs[0].begin(), convs[0].end(), [&](const Conversation &c) {
        return c.id == engineer && c.agentRole == "engineer";
    }));
    const auto users = collect(backend.loadUsers());
    CHECK(std::any_of(users[0].begin(), users[0].end(), [&](const User &u) {
        return u.id.value == "claude:" + engineer.value && u.avatarUrl == "qrc:/roles/engineer.svg";
    }));
    CHECK(std::any_of(users[0].begin(), users[0].end(), [](const User &u) {
        return u.id.value == "claude:role:engineer" && u.name == "Engineer";
    }));
    out.text = {"hi", {}};
    backend.sendMessage(engineer, out, {});
    CHECK(
        QTest::qWaitFor(
            [&] {
                for (const auto &e : events)
                    if (const auto *n = std::get_if<EvMessageNew>(&e);
                        n && n->conv == engineer && n->msg.text.text == "echo hi")
                        return n->msg.author == UserId{"claude:role:engineer"};
                return false;
            },
            8000
        )
    );
    QFile calls2(home.dir.path() + "/calls.log");
    REQUIRE(calls2.open(QIODevice::ReadOnly));
    CHECK(
        calls2.readAll().contains("--append-system-prompt # Your role: Engineer (msga: engineer)\n")
    );
    calls2.close();

    // An added teammate. Editing its instructions reaches new sessions only —
    // one already started goes on without flags, keeping what it began with —
    // and removing it leaves its session its name and picture.
    auto waitFor = [&](const ConversationId &c, const QString &text) {
        return QTest::qWaitFor(
            [&] {
                for (const auto &e : events)
                    if (const auto *n = std::get_if<EvMessageNew>(&e);
                        n && n->conv == c && n->msg.text.text == text)
                        return true;
                return false;
            },
            8000
        );
    };
    AgentRole copy;
    copy.name   = "Copywriter";
    copy.glyph  = "pen-tool";
    copy.color  = "#0e8c9a";
    copy.prompt = "Write copy v1.";
    QString err;
    REQUIRE(backend.saveAgentRole(copy, &err) == "copywriter");
    ConversationId cw;
    backend.startAgentSession(
        work.path(), false, "copywriter", [&](ConversationId id) { cw = id; }, {}
    );
    REQUIRE(QTest::qWaitFor([&] { return !cw.value.isEmpty(); }, 5000));
    out.text = {"tagline", {}};
    backend.sendMessage(cw, out, {});
    REQUIRE(waitFor(cw, "echo tagline"));
    for (const AgentRole &r : backend.agentRoles())
        if (r.id == "copywriter")
            copy = r;
    copy.prompt = "Write copy v2.";
    REQUIRE(backend.saveAgentRole(copy, &err) == "copywriter");
    out.text = {"again", {}};
    backend.sendMessage(cw, out, {});
    REQUIRE(waitFor(cw, "echo again"));
    ConversationId cw2;
    backend.startAgentSession(
        work.path(), false, "copywriter", [&](ConversationId id) { cw2 = id; }, {}
    );
    REQUIRE(QTest::qWaitFor([&] { return !cw2.value.isEmpty(); }, 5000));
    out.text = {"fresh", {}};
    backend.sendMessage(cw2, out, {});
    REQUIRE(waitFor(cw2, "echo fresh"));
    QFile calls3(home.dir.path() + "/calls.log");
    REQUIRE(calls3.open(QIODevice::ReadOnly));
    const QString     calls3Text = QString::fromUtf8(calls3.readAll());
    // Both its prompt and its subagent type carry the text: count launches.
    // (A call's log entry runs over lines where its prompt does.)
    const QStringList calls3List = calls3Text.split("\n--bg");
    auto              launches   = [&](const QString &text) {
        return std::count_if(calls3List.begin(), calls3List.end(), [&](const QString &call) {
            return call.contains(text);
        });
    };
    CHECK(launches("Write copy v1.") == 1);
    CHECK(launches("Write copy v2.") == 1);
    CHECK(calls3Text.contains(R"("copywriter":{"description":"Copywriter, a teammate)"));
    CHECK(
        calls3Text.indexOf("Write copy v2.") > calls3Text.indexOf(" -- again")
    ); // only the new session
    for (const QString &l : calls3Text.split('\n'))
        if (l.endsWith(" -- again"))
            CHECK(l.startsWith("--bg --resume ")); // no flags: its own prompt stays

    backend.removeAgentRole("copywriter");
    for (const AgentRole &r : backend.agentRoles())
        CHECK(r.id != "copywriter");
    const auto afterRemove = collect(backend.loadConversations());
    CHECK(std::any_of(afterRemove[0].begin(), afterRemove[0].end(), [&](const Conversation &c) {
        return c.id == cw && c.agentRole == "copywriter";
    }));
    const auto usersAfter = collect(backend.loadUsers());
    CHECK(std::any_of(usersAfter[0].begin(), usersAfter[0].end(), [&](const User &u) {
        return u.id.value == "claude:" + cw.value && u.avatarUrl.endsWith("pen-tool-0e8c9a.svg");
    }));
    CHECK(std::any_of(usersAfter[0].begin(), usersAfter[0].end(), [](const User &u) {
        return u.id.value == "claude:role:copywriter" && u.name == "Copywriter";
    }));
}

TEST_CASE("Stop cuts a session's turn short and drops what waits", "[claude][backend][bg]") {
    FakeClaudeHome home;
    home.writeSession("busy"); // S1: a terminal's session, working
    QTemporaryDir work;
    QDir(home.dir.path()).mkpath("jobs");
    QDir(home.dir.path()).mkpath("projects/-fake");
    {
        QFile f(home.dir.path() + "/.claude.json");
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(
                    QJsonObject{
                        {"projects",
                         QJsonObject{
                             {QDir(work.path()).absolutePath(),
                              QJsonObject{{"hasTrustDialogAccepted", true}}}
                         }},
                    }
        )
                    .toJson());
    }
    // Every turn this CLI starts goes on until it is stopped. Its worker is a
    // real process: a stop waits for it to exit.
    const QString cli = work.path() + "/claude";
    {
        QFile f(cli);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(R"SH(#!/bin/sh
H="$CLAUDE_CONFIG_DIR"
if [ "$1" = auth ]; then echo '{"loggedIn":true,"authMethod":"claude.ai"}'; exit 0; fi
if [ "$1" = attach ]; then # typing into a live worker: see the terminal UI test
  printf '%s\n' "$*" >> "$H/attach.log"
  [ -n "$FAKE_ATTACH" ] && exec "$FAKE_ATTACH" "$2"
  echo "no such session"; exit 1
fi
printf '%s\n' "$*" >> "$H/calls.log" # echo would expand \n
if [ "$1" = stop ] || [ "$1" = rm ]; then
  # The worker writes its last records as it exits.
  T=$(sed -n 's/.*"linkScanPath":"\([^"]*\)".*/\1/p' "$H/jobs/$2/state.json")
  echo '{"type":"last-prompt","lastPrompt":"x"}' >> "$T"
  rm -f "$H/sessions/w$2.json"
  kill $(cat "$H/wpid-$2" 2>/dev/null) 2>/dev/null
  if [ "$1" = rm ]; then rm -rf "$H/jobs/$2"; echo "removed $2"; exit 0; fi
  # No `sed -i`: BSD sed (macOS) reads its next argument as a backup suffix.
  sed 's/"state":"[a-z]*"/"state":"stopped"/' "$H/jobs/$2/state.json" > "$H/state.tmp" &&
    mv "$H/state.tmp" "$H/jobs/$2/state.json"
  echo "stopped $2"; exit 0
fi
sid=""; prompt=""
while [ $# -gt 0 ]; do
  case "$1" in
    --resume) sid="$2"; shift ;;
    --) prompt="$2"; shift ;;
  esac; shift
done
[ -z "$sid" ] && sid="abcdef11-0000-4000-8000-000000000001"
short=$(echo "$sid" | cut -c1-8)
T="$H/projects/-fake/$sid.jsonl"
# Millisecond ISO time via perl, not `date +%3N`: BSD date (macOS) prints a literal "3N".
ts=$(perl -MTime::HiRes=time -MPOSIX=strftime -e '$t = time; printf "%s.%03dZ", strftime("%Y-%m-%dT%H:%M:%S", gmtime $t), ($t - int $t) * 1000')
mkdir -p "$H/jobs/$short"
echo "{\"state\":\"working\",\"sessionId\":\"$sid\",\"cwd\":\"$PWD\",\"name\":\"fake-$short\",\"linkScanPath\":\"$T\"}" > "$H/jobs/$short/state.json"
kill $(cat "$H/wpid-$short" 2>/dev/null) 2>/dev/null
sleep 60 </dev/null >/dev/null 2>&1 &
echo $! > "$H/wpid-$short"
echo "{\"pid\":$!,\"sessionId\":\"$sid\",\"kind\":\"bg\",\"status\":\"busy\"}" > "$H/sessions/w$short.json"
echo "{\"type\":\"user\",\"timestamp\":\"$ts\",\"origin\":{\"kind\":\"human\"},\"message\":{\"content\":\"$prompt\"}}" >> "$T"
echo "backgrounded · $short"
)SH");
        f.setPermissions(f.permissions() | QFileDevice::ExeOwner);
    }

    claude_code::Backend backend(Credentials{cli});
    std::vector<Event>   events;
    rpl::lifetime        lt;
    backend.events() | rpl::on_next([&](Event e) { events.push_back(std::move(e)); }, lt);
    backend.connectRealtime();

    // A terminal's session is the terminal's to stop.
    CHECK_FALSE(backend.canStopAgentSession(ConversationId{"S1"}));

    ConversationId conv;
    backend.startAgentSession(work.path(), false, {}, [&](ConversationId id) { conv = id; }, {});
    REQUIRE(QTest::qWaitFor([&] { return !conv.value.isEmpty(); }, 5000));
    CHECK_FALSE(backend.canStopAgentSession(conv)); // nothing sent yet

    const QString sid   = "abcdef11-0000-4000-8000-000000000001";
    auto          calls = [&] {
        QFile f(home.dir.path() + "/calls.log");
        return f.open(QIODevice::ReadOnly)
                   ? QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts)
                   : QStringList{};
    };
    auto send = [&](const char *text) {
        OutgoingMessage out;
        out.text = {text, {}};
        backend.sendMessage(conv, out, {});
    };
    auto ownTexts = [&] {
        QStringList texts;
        const auto  pages = collect(backend.loadHistory(conv, std::nullopt));
        for (const auto &m : pages[0].messages)
            if (m.author.value == "me")
                texts << m.text.text;
        return texts;
    };
    const UserId assistant{"claude:" + conv.value};
    auto         working = [&] {
        const auto presence = collect(backend.loadPresence(assistant)); // vector<bool>: bind it
        return bool(presence[0]);
    };

    send("first");
    // Claude is on it: the prompt is in and the worker reads busy.
    REQUIRE(QTest::qWaitFor([&] { return ownTexts() == QStringList{"first"}; }, 8000));
    QTest::qWait(300); // the launcher has reported back
    send("second");    // waits for the turn to end
    CHECK(ownTexts() == QStringList{"first", "second"});
    CHECK(working());
    REQUIRE(backend.canStopAgentSession(conv));

    backend.stopAgentSession(conv);
    CHECK_FALSE(working()); // at once
    CHECK_FALSE(backend.canStopAgentSession(conv));
    CHECK(ownTexts() == QStringList{"first"}); // the waiting one is dropped
    REQUIRE(QTest::qWaitFor([&] { return calls().size() == 2; }, 8000));
    CHECK(calls()[1] == "stop abcdef11");
    QTest::qWait(1000); // stopped, and nothing more goes out
    CHECK(calls().size() == 2);
    CHECK_FALSE(working());
    CHECK(std::none_of(events.begin(), events.end(), [](const Event &e) {
        return std::holds_alternative<EvSendFailed>(e);
    }));

    // The next message continues it: the worker is gone, so no stop first.
    send("third");
    REQUIRE(QTest::qWaitFor([&] { return calls().size() == 3; }, 8000));
    CHECK(calls()[2] == "--bg --resume " + sid + " -- third");
    // Stopped while the CLI is still starting the turn: stopped once it has.
    backend.stopAgentSession(conv);
    REQUIRE(QTest::qWaitFor([&] { return calls().size() == 4; }, 8000));
    CHECK(calls()[3] == "stop abcdef11");
    QTest::qWait(1000);
    CHECK_FALSE(working());
    CHECK_FALSE(backend.canStopAgentSession(conv));

    // An idle worker can be stopped too: a subagent's result or a scheduled
    // prompt would wake it without anyone sending a thing.
    send("fourth");
    REQUIRE(QTest::qWaitFor([&] { return calls().size() == 5; }, 8000));
    const QString workerFile = home.dir.path() + "/sessions/wabcdef11.json";
    REQUIRE(QTest::qWaitFor([&] { return QFile::exists(workerFile); }, 8000));
    QTest::qWait(300); // the launcher has reported back
    {
        QFile w(home.dir.path() + "/wpid-abcdef11"); // the fake's worker process
        REQUIRE(w.open(QIODevice::ReadOnly));
        QFile f(workerFile);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(QString(R"({"pid":%1,"sessionId":"%2","kind":"bg","status":"idle"})")
                    .arg(w.readAll().trimmed().toLongLong())
                    .arg(sid)
                    .toUtf8());
        // …and the turn is over.
        QFile job(home.dir.path() + "/jobs/abcdef11/state.json");
        REQUIRE(job.open(QIODevice::ReadOnly));
        const QByteArray state = job.readAll().replace("\"working\"", "\"done\"");
        job.close();
        REQUIRE(job.open(QIODevice::WriteOnly | QIODevice::Truncate));
        job.write(state);
        QFile t(home.dir.path() + "/projects/-fake/" + sid + ".jsonl");
        REQUIRE(t.open(QIODevice::Append));
        t.write("{\"type\":\"system\",\"subtype\":\"turn_duration\"}\n");
    }
    REQUIRE(QTest::qWaitFor([&] { return !working(); }, 8000));
    CHECK(backend.canStopAgentSession(conv));

#if defined(Q_OS_LINUX)
    // What the session left running outside its worker is ended with it —
    // only that: a process that merely inherited the session id is no leftover.
    auto spawn = [&](bool inJob, qint64 *pid) {
        QProcess            p;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("CLAUDE_CODE_SESSION_ID", sid);
        if (inJob)
            env.insert("CLAUDE_JOB_DIR", home.dir.path() + "/jobs/abcdef11");
        p.setProcessEnvironment(env);
        p.setProgram("sleep");
        p.setArguments({"30"});
        return p.startDetached(pid); // not msga's child: those are spared
    };
    qint64 leftover = 0, bystander = 0;
    REQUIRE(spawn(true, &leftover));
    REQUIRE(spawn(false, &bystander));
    REQUIRE(
        QTest::qWaitFor(
            [&] { return leftoverProcesses(sid, "abcdef11") == std::vector<qint64>{leftover}; },
            3000
        )
    );
#endif
    backend.stopAgentSession(conv);
    REQUIRE(QTest::qWaitFor([&] { return calls().size() == 6; }, 8000));
    CHECK(calls()[5] == "stop abcdef11");
#if defined(Q_OS_LINUX)
    CHECK(QTest::qWaitFor([&] { return !isProcessAlive(leftover); }, 5000));
    CHECK(isProcessAlive(bystander));
    signalProcess(bystander, true);
#endif

    // "Remove from msga" deletes it (`claude rm`), live worker and all, and the
    // session stays away though the worker still writes to its transcript as
    // it goes.
    send("fifth");
    REQUIRE(QTest::qWaitFor([&] { return calls().size() == 7; }, 8000));
    REQUIRE(QTest::qWaitFor([&] { return working(); }, 8000));
    QTest::qWait(300); // the launcher has reported back
    backend.leaveConversation(conv);
    REQUIRE(QTest::qWaitFor([&] { return calls().size() == 8; }, 8000));
    CHECK(calls()[7] == "rm abcdef11");
    QTest::qWait(1500);
    const auto convs = collect(backend.loadConversations());
    CHECK(std::none_of(convs[0].begin(), convs[0].end(), [&](const Conversation &c) {
        return c.id == conv;
    }));
    // Nothing resumes it. A removal that lands while the turn is still being
    // launched (a slow machine: the 300 ms above isn't a guarantee) stops the
    // worker once more when the launch reports back — needed, the first rm may
    // have beaten the new worker — so a second "rm" is fine, anything else not.
    const QStringList after = calls();
    REQUIRE(after.size() >= 8);
    for (qsizetype i = 8; i < after.size(); ++i)
        CHECK(after[i] == "rm abcdef11");
}
#endif

namespace {

QByteArray rule(int n, const QByteArray &title = {}) {
    QByteArray r;
    for (int i = 0; i < n; ++i)
        r += "─";
    return title.isEmpty() ? r : r + " " + title + " ─";
}

// A frame drawn the way Claude Code 2.1.282 draws its idle screen: absolute
// rows, the prompt box near the bottom, the cursor parked in it.
QByteArray idleFrame(const QByteArray &input = {}, bool cursorShown = true) {
    QByteArray f = "\x1b[?1049h\x1b[H\x1b[2J\x1b[?25l";
    f += "\x1b[2;1H\x1b[38;5;174m ▐▛███▛█\x1b[39m   Claude Code v2.1.282";
    f += "\x1b[6;1H❯ Launch ONE subagent\x1b[8;1H● STARTED";
    f += "\x1b[10;1H✻ Waiting for 1 background agent to finish";
    f += "\x1b[44;1H" + rule(100, "fix the build");
    f += "\x1b[45;1H❯ " + input;
    f += "\x1b[46;1H" + rule(120);
    f += "\x1b[47;1H  ⏸ manual mode on · ← 1 agent · ↓ to manage";
    f += "\x1b[45;" + QByteArray::number(3 + input.size()) + "H";
    if (cursorShown)
        f += "\x1b[?25h";
    return f;
}

} // namespace

TEST_CASE("the terminal screen shows when Claude Code's prompt takes typing", "[claude][attach]") {
    {
        VtScreen s(50, 160);
        s.feed(idleFrame());
        CHECK(s.row(43).startsWith("────"));
        CHECK(s.row(43).endsWith("fix the build ─"));
        CHECK(s.row(44) == "❯");
        REQUIRE(findPromptBox(s));
        CHECK(findPromptBox(s)->empty);
        CHECK(readyForInput(s));
    }
    {
        // Mid-draw (cursor hidden), or a draft someone left in the box: no.
        VtScreen hidden(50, 160);
        hidden.feed(idleFrame({}, false));
        CHECK_FALSE(readyForInput(hidden));
        VtScreen draft(50, 160);
        draft.feed(idleFrame("half a thought"));
        CHECK_FALSE(readyForInput(draft));
        REQUIRE(findPromptBox(draft));
        CHECK(findPromptBox(draft)->lines == QStringList{"half a thought"});
    }
    {
        // A permission question (here a subagent's) takes the box's place.
        VtScreen s(50, 160);
        s.feed(
            "\x1b[H\x1b[2J\x1b[6;1H● STARTED\x1b[16;1H" + rule(120) +
            "\x1b[17;1H Bash command · from the general-purpose agent"
            "\x1b[22;1H Do you want to proceed?\x1b[23;1H ❯ 1. Yes\x1b[24;1H   2. No"
            "\x1b[27;1H Esc to cancel · Tab to amend\x1b[23;2H\x1b[?25h"
        );
        CHECK_FALSE(findPromptBox(s));
        CHECK_FALSE(readyForInput(s));
        const auto q = findPermissionQuestion(s);
        REQUIRE(q);
        REQUIRE(q->options.size() == 2);
        CHECK(q->options[0].label == "Yes");
        CHECK(q->options[1].label == "No");
        CHECK(q->selected == 1);
        CHECK(q->text == "Bash command · from the general-purpose agent Do you want to proceed?");
    }
    {
        // As Claude Code 2.1.283 drew one for a background session (seen through
        // `claude attach`, 2026-09-26): the command framed and wrapped, a note,
        // the auto-deny countdown — and "❯" moved down to the second option.
        VtScreen s(50, 120);
        s.feed(
            "\x1b[H\x1b[2J\x1b[25;1H● Running 1 shell command…\x1b[31;1H" + rule(120) +
            "\x1b[32;1H Bash command"
            "\x1b[34;1H   │ rm -rf /home/robin/.claude/jobs/b7b46dde/tmp/ccfg"
            "\x1b[35;1H   │ /home/robin/src/msga/build-tests; git -C"
            "\x1b[36;1H   │ /home/robin/src/msga status --short"
            "\x1b[37;1H   Run shell command"
            "\x1b[39;1H │ Dangerous rm operation on working directory or its ancestor:"
            "\x1b[41;1H ⚠ Claude Code will automatically deny this request in 0:14"
            "\x1b[44;1H Do you want to proceed?\x1b[45;1H   1. Yes\x1b[46;1H ❯ 2. No"
            "\x1b[48;1H Esc to cancel · Tab to amend\x1b[46;2H"
        );
        const auto q = findPermissionQuestion(s);
        REQUIRE(q);
        REQUIRE(q->options.size() == 2);
        CHECK(q->selected == 2);
        CHECK(
            q->text.startsWith("Bash command │ rm -rf /home/robin/.claude/jobs/b7b46dde/tmp/ccfg")
        );
    }
    {
        // The prompt box, or a numbered list in an answer: no question.
        VtScreen box(50, 160);
        box.feed(idleFrame());
        CHECK_FALSE(findPermissionQuestion(box));
        VtScreen list(50, 160);
        list.feed(
            "\x1b[H\x1b[2J\x1b[3;1H" + rule(120) + "\x1b[4;1H Steps:\x1b[5;1H 1. build" +
            "\x1b[6;1H 2. test"
        );
        CHECK_FALSE(findPermissionQuestion(list)); // no "❯" on any of them
    }
    {
        // A panel (/cost) has the keyboard: no box either.
        VtScreen s(50, 160);
        s.feed("\x1b[H\x1b[2J\x1b[3;1H  Current session\x1b[45;1H   Esc to cancel\x1b[3;31H");
        CHECK_FALSE(readyForInput(s));
    }
    {
        // Relative moves, erasing, a wide character, UTF-8 split across reads.
        VtScreen         s(5, 20);
        const QByteArray beer = "🍺";
        s.feed("ab\x1b[3Gc" + beer.left(2));
        s.feed(beer.mid(2) + "d\x1b[2;4Hx\x1b[1A\x1b[2Cy");
        CHECK(s.row(0) == "abc🍺dy");
        CHECK(s.row(1) == "   x");
        s.feed("\x1b[1;3H\x1b[K");
        CHECK(s.row(0) == "ab");
        CHECK(s.cursorRow() == 0);
        CHECK(s.cursorCol() == 2);
        // Writing past the last row scrolls up.
        s.feed("\x1b[5;1Hlast\r\nnext");
        CHECK(s.row(3) == "last");
        CHECK(s.row(4) == "next");
    }
}

TEST_CASE("the question on screen is matched to the job's needs", "[claude][attach]") {
    const auto question = [](const QString &text) {
        PermissionQuestion q;
        q.text    = text;
        q.options = {{1, "Yes"}, {2, "No"}};
        return q;
    };
    // A command, wrapped and framed on screen.
    const auto rm =
        question("Bash command │ rm -rf /home/robin/src/msga/ │ build Do you want to proceed?");
    CHECK(questionIsFor("approve Bash: rm -rf /home/robin/src/msga/build", rm));
    CHECK_FALSE(questionIsFor("approve Bash: git push", rm));
    // A label alone, which the question words its own way (EnterWorktree with
    // a path, Claude Code 2.1.283): "approve Entering worktree" asks this.
    const auto worktree = question(
        "Enter the worktree at \"/home/robin/src/citycity/monorepo\"? This moves the session's "
        "working directory and write access there, and loads project configuration (CLAUDE.md, "
        "settings) from that location."
    );
    CHECK(questionIsFor("approve Entering worktree", worktree));
    CHECK(questionIsFor("approve Web fetch", question("Web fetch https://example.com")));
    CHECK_FALSE(questionIsFor("approve Entering worktree", rm));
    CHECK_FALSE(questionIsFor("approve ", worktree));
}

TEST_CASE("a message is typed line by line, never as one big paste", "[claude][attach]") {
    const auto paste = [](const QString &t) {
        return QByteArray("\x1b[200~") + t.toUtf8() + "\x1b[201~";
    };
    CHECK(
        AttachInput::keystrokes("first line\nsecond \"quoted\"") ==
        QList<QByteArray>{paste("first line"), "\n", paste("second \"quoted\"")}
    );
    // Blank lines stay; a trailing newline and CRLFs don't make extra ones.
    CHECK(
        AttachInput::keystrokes("a\r\n\r\nb\n") ==
        QList<QByteArray>{paste("a"), "\n", "\n", paste("b")}
    );
    // "!" would run a shell command, "/" a slash command: a space keeps them text.
    CHECK(AttachInput::keystrokes("!rm -rf /") == QList<QByteArray>{paste(" !rm -rf /")});
    CHECK(AttachInput::keystrokes("/cost") == QList<QByteArray>{paste(" /cost")});
    // Control characters would be keys (Esc, Ctrl+C): dropped.
    CHECK(AttachInput::keystrokes("x\x1b[31my\x03") == QList<QByteArray>{paste("x[31my")});
    // A long line goes in pieces; a character is never cut in two.
    const QString long1000 = QString(399, 'a') + QString::fromUtf8("🍺") + QString(600, 'b');
    const auto    keys     = AttachInput::keystrokes(long1000);
    REQUIRE(keys.size() == 3);
    QString joined;
    for (const auto &k : keys) {
        CHECK(k.startsWith("\x1b[200~"));
        joined += QString::fromUtf8(k.mid(6, k.size() - 12));
    }
    CHECK(joined == long1000);
    CHECK(QString::fromUtf8(keys[0].mid(6, keys[0].size() - 12)).size() == 399);
}

// Against the real CLI, by hand, after a Claude Code upgrade: point it at a
// live background session of a throwaway CLAUDE_CONFIG_DIR —
//   MSGA_CC_LIVE_SHORT=<short> MSGA_CC_LIVE_TEXT="What is 2+2?" \
//   [MSGA_CC_LIVE_EXPECT=NotReady] test_claude_code "[.live]"
// (claude on PATH). Sent means the prompt box took it; the transcript shows
// whether Claude got it as typed.
TEST_CASE("typing into a real background session", "[.live][attach]") {
    const QString shortId = qEnvironmentVariable("MSGA_CC_LIVE_SHORT");
    const QString claude  = QStandardPaths::findExecutable("claude");
    if (shortId.isEmpty() || claude.isEmpty())
        SKIP("MSGA_CC_LIVE_SHORT and claude on PATH needed");
    const QString text =
        qEnvironmentVariable("MSGA_CC_LIVE_TEXT", "Reply with only the word PONG.");
    const QString expect = qEnvironmentVariable("MSGA_CC_LIVE_EXPECT", "Sent");
    std::optional<AttachInput::Outcome> outcome;
    QString                             detail;
    AttachInput::send(
        claude,
        {"attach", shortId},
        QDir::currentPath(),
        text,
        [&](AttachInput::Outcome o, QString d) {
            outcome = o;
            detail  = d;
        },
        nullptr
    );
    REQUIRE(QTest::qWaitFor([&] { return outcome.has_value(); }, 60'000));
    INFO(detail.toStdString());
    const char *names[] = {"Sent", "NotReady", "Failed", "Unconfirmed"};
    CHECK(QString(names[int(*outcome)]) == expect);
}

// A reply in a real subagent's thread, relayed by its session: run in a
// throwaway CLAUDE_CONFIG_DIR, with MSGA_CC_LIVE_SESSION = a background
// session (claude --bg) whose last turn launched a subagent, and claude on PATH.
TEST_CASE("replying to a real subagent", "[.live][thread]") {
    const QString sessionId = qEnvironmentVariable("MSGA_CC_LIVE_SESSION");
    const QString claude    = QStandardPaths::findExecutable("claude");
    if (sessionId.isEmpty() || claude.isEmpty())
        SKIP("MSGA_CC_LIVE_SESSION and claude on PATH needed");
    const QString reply =
        qEnvironmentVariable("MSGA_CC_LIVE_TEXT", "What codeword were you given? Only the word.");
    Credentials creds;
    creds.claudePath = claude;
    claude_code::Backend backend(creds);
    backend.connectRealtime();
    const ConversationId conv{sessionId};
    std::optional<Ts>    root;
    REQUIRE(
        QTest::qWaitFor(
            [&] {
                for (const auto &page : collect(backend.loadHistory(conv, std::nullopt)))
                    for (const auto &m : page.messages)
                        if (m.replyCount > 0 && backend.threadAcceptsReplies(conv, m.ts))
                            root = m.ts;
                return root.has_value();
            },
            30'000
        )
    );
    OutgoingMessage out;
    out.composerText = reply;
    out.threadRoot   = root;
    std::optional<bool> sent;
    QString             error;
    backend.sendMessage(conv, out, [&](bool ok, QString err) {
        sent  = ok;
        error = err;
    });
    REQUIRE(QTest::qWaitFor([&] { return sent.has_value(); }, 10'000));
    INFO(error.toStdString());
    REQUIRE(*sent);
    // The reply lands in the thread as typed; the subagent answers after it.
    std::vector<Message> thread;
    const bool           answered = QTest::qWaitFor(
        [&] {
            thread          = collect(backend.loadThread(conv, *root, std::nullopt))[0].messages;
            const auto mine = std::find_if(thread.begin(), thread.end(), [&](const Message &m) {
                return m.author == UserId{"me"} && m.text.text == reply;
            });
            return mine != thread.end() && mine + 1 != thread.end();
        },
        240'000
    );
    for (const auto &m : thread)
        UNSCOPED_INFO(m.author.value.toStdString() << ": " << m.text.text.left(120).toStdString());
    CHECK(answered);
    for (const auto &page : collect(backend.loadHistory(conv, std::nullopt)))
        for (const auto &m : page.messages)
            CHECK_FALSE(m.text.text.contains(reply)); // not in the chat itself
}

#ifdef CC_FAKE_ATTACH
TEST_CASE(
    "a live background session is typed to, not stopped, even while it works",
    "[claude][backend][bg][attach]"
) {
    FakeClaudeHome home;
    QTemporaryDir  work;
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/claude-code/team")
        .removeRecursively();
    QDir(home.dir.path()).mkpath("jobs");
    QDir(home.dir.path()).mkpath("projects/-fake");
    {
        QFile f(home.dir.path() + "/.claude.json");
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(
                    QJsonObject{
                        {"projects",
                         QJsonObject{
                             {QDir(work.path()).absolutePath(),
                              QJsonObject{{"hasTrustDialogAccepted", true}}}
                         }},
                    }
        )
                    .toJson());
    }
    // `--bg` starts a session whose worker (a real process) idles on after the
    // turn; `attach` is the fake terminal UI (cc_fake_attach).
    qputenv("FAKE_ATTACH", CC_FAKE_ATTACH);
    AttachInput::setAttachTimeoutMs(1500);
    const QString cli = work.path() + "/claude";
    {
        QFile f(cli);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(R"SH(#!/bin/sh
H="$CLAUDE_CONFIG_DIR"
if [ "$1" = auth ]; then echo '{"loggedIn":true,"authMethod":"claude.ai"}'; exit 0; fi
if [ "$1" = attach ]; then
  printf '%s\n' "$*" >> "$H/attach.log"
  exec "$FAKE_ATTACH" "$2"
fi
printf '%s\n' "$*" >> "$H/calls.log"
if [ "$1" = stop ]; then
  rm -f "$H/sessions/w$2.json"; kill $(cat "$H/wpid-$2") 2>/dev/null; exit 0
fi
prompt=""
while [ $# -gt 0 ]; do [ "$1" = -- ] && prompt="$2"; shift; done
sid="abcdef11-0000-4000-8000-000000000001"; short=abcdef11
T="$H/projects/-fake/$sid.jsonl"
mkdir -p "$H/jobs/$short"
echo "{\"state\":\"done\",\"sessionId\":\"$sid\",\"cwd\":\"$PWD\",\"name\":\"fake\",\"linkScanPath\":\"$T\"}" > "$H/jobs/$short/state.json"
sleep 60 </dev/null >/dev/null 2>&1 &
echo $! > "$H/wpid-$short"
echo "{\"pid\":$!,\"sessionId\":\"$sid\",\"kind\":\"bg\",\"status\":\"idle\"}" > "$H/sessions/w$short.json"
echo "{\"type\":\"user\",\"uuid\":\"s-$$-1\",\"origin\":{\"kind\":\"human\"},\"message\":{\"content\":\"$prompt\"}}" >> "$T"
echo "{\"type\":\"assistant\",\"uuid\":\"s-$$-2\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"echo $prompt\"}]}}" >> "$T"
echo "{\"type\":\"system\",\"subtype\":\"turn_duration\",\"uuid\":\"s-$$-3\"}" >> "$T"
echo "backgrounded · $short"
)SH");
        f.setPermissions(f.permissions() | QFileDevice::ExeOwner);
    }
    const QString H        = home.dir.path();
    auto          readText = [&](const QString &name) {
        QFile f(H + "/" + name);
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
    };
    // A `stop` call (the first call's --agents JSON can say "stop" too).
    auto stopped = [&] {
        const QStringList calls = readText("calls.log").split('\n');
        return std::any_of(calls.begin(), calls.end(), [](const QString &l) {
            return l.startsWith("stop ");
        });
    };
    auto writeText = [&](const QString &name, const QByteArray &text) {
        QFile f(H + "/" + name);
        REQUIRE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(text);
    };

    claude_code::Backend backend(Credentials{cli});
    std::vector<Event>   events;
    rpl::lifetime        lt;
    backend.events() | rpl::on_next([&](Event e) { events.push_back(std::move(e)); }, lt);
    backend.connectRealtime();
    ConversationId conv;
    backend.startAgentSession(work.path(), false, {}, [&](ConversationId id) { conv = id; }, {});
    REQUIRE(QTest::qWaitFor([&] { return !conv.value.isEmpty(); }, 5000));
    auto send = [&](const QString &text) {
        OutgoingMessage out;
        out.text = {text, {}};
        backend.sendMessage(conv, out, {});
    };
    auto answered = [&](const QString &text, int ms = 8000) {
        return QTest::qWaitFor(
            [&] {
                return std::any_of(events.begin(), events.end(), [&](const Event &e) {
                    const auto *n = std::get_if<EvMessageNew>(&e);
                    return n && n->conv == conv && n->msg.text.text == text;
                });
            },
            ms
        );
    };

    send("first"); // a new session: started the usual way
    REQUIRE(answered("echo first"));
    const QString worker = readText("wpid-abcdef11").trimmed();
    REQUIRE_FALSE(worker.isEmpty());

    // A subagent runs: the worker reads busy though Claude waits for input.
    QTest::qWait(300);
    writeText(
        "sessions/wabcdef11.json",
        QString(
            R"({"pid":%1,"sessionId":"abcdef11-0000-4000-8000-000000000001","kind":"bg","status":"busy"})"
        )
            .arg(worker)
            .toUtf8()
    );
    QTest::qWait(500); // the roster has seen it
    send("second");
    REQUIRE(answered("echo second"));
    CHECK(readText("typed.log") == "second\n");
    CHECK(readText("attach.log").contains("attach abcdef11"));
    CHECK_FALSE(stopped()); // the worker (and its subagent) live on
    CHECK(QProcess::execute("kill", {"-0", worker}) == 0);

    // Several lines, one starting with "!": typed as they are, as plain text.
    send("!make it\nwork");
    REQUIRE(answered("echo  !make it\nwork"));
    CHECK(readText("typed.log").endsWith(" !make it\\nwork\n"));

    // A permission question has the keyboard: nothing is typed, the message
    // waits — no stop, no resume — and goes once the prompt is back.
    writeText("attach-mode", "question");
    send("third");
    QTest::qWait(2500);
    CHECK_FALSE(readText("typed.log").contains("third"));
    CHECK_FALSE(QFile::exists(H + "/question-keys.log"));
    CHECK_FALSE(stopped());
    writeText("attach-mode", "");
    REQUIRE(answered("echo third", 10000));
    CHECK_FALSE(stopped());
    CHECK_FALSE(std::any_of(events.begin(), events.end(), [](const Event &e) {
        return std::holds_alternative<EvSendFailed>(e);
    }));

    // Waiting on the permission question its job names: the question's
    // options, read off the screen, are buttons on "Waiting for your
    // approval", and a press picks that option there. A message sent
    // meanwhile waits for the answer instead of being refused.
    const QString jobPath  = "jobs/abcdef11/state.json";
    QJsonObject   job      = QJsonDocument::fromJson(readText(jobPath).toUtf8()).object();
    const auto    writeJob = [&] {
        writeText(jobPath, QJsonDocument(job).toJson(QJsonDocument::Compact));
    };
    writeText("attach-mode", "question");
    job["state"] = "working";
    job["needs"] = "approve Bash: rm -rf build";
    writeJob();
    std::optional<Message> waiting;
    REQUIRE(
        QTest::qWaitFor(
            [&] {
                for (const auto &e : events) {
                    const Message *m = nullptr;
                    if (const auto *n = std::get_if<EvMessageNew>(&e); n && n->conv == conv)
                        m = &n->msg;
                    if (const auto *c = std::get_if<EvMessageChanged>(&e); c && c->conv == conv)
                        m = &c->msg;
                    if (m && m->blocks.size() == 2)
                        waiting = *m;
                }
                return waiting.has_value();
            },
            10000
        )
    );
    CHECK(backend.capabilities().botButtons);
    CHECK_FALSE(waiting->botId.isEmpty());
    CHECK(waiting->blocks[0].text.text == "Waiting for your approval: Bash: rm -rf build");
    const auto &buttons = waiting->blocks[1].buttons;
    REQUIRE(buttons.size() == 3);
    CHECK(buttons[0].text == "Yes");
    CHECK(buttons[0].style == "primary");
    CHECK(buttons[1].text == "Yes, and don't ask again for rm commands");
    CHECK(buttons[2].text == "No");
    CHECK(buttons[2].style == "danger");
    CHECK_FALSE(QFile::exists(H + "/answered.log")); // reading it pressed nothing
    CHECK_FALSE(readText("question-keys.log").contains('\r'));
    send("fifth");
    bool pressed = false, ok = false;
    backend.pressBotButton(
        conv, waiting->ts, std::nullopt, waiting->botId, buttons[2], [&](bool o, QString) {
            pressed = true;
            ok      = o;
        }
    );
    REQUIRE(QTest::qWaitFor([&] { return pressed; }, 10000));
    CHECK(ok);
    CHECK(readText("answered.log") == "3\n"); // "No", moved to with ↓↓ before Enter
    CHECK_FALSE(readText("typed.log").contains("fifth"));
    job["state"] = "done";
    job.remove("needs");
    writeJob();
    REQUIRE(answered("echo fifth", 10000));
    CHECK_FALSE(stopped());
    CHECK_FALSE(std::any_of(events.begin(), events.end(), [](const Event &e) {
        return std::holds_alternative<EvSendFailed>(e);
    }));

    // Mid-turn Claude Code queues the prompt and may draw the box as it likes:
    // a hint in it, or the prompt seemingly still there. Delivered either way
    // (the transcript has it) — never "didn't take the message".
    AttachInput::setSubmitTimeoutMs(1000);
    writeText("attach-mode", "hint");
    send("queued one");
    REQUIRE(answered("echo queued one"));
    writeText("attach-mode", "sticky");
    send("queued two");
    REQUIRE(answered("echo queued two"));
    QTest::qWait(1500); // past the box's deadline
    writeText("attach-mode", "");
    send("queued three"); // and the next one still goes live
    REQUIRE(answered("echo queued three", 10000));
    CHECK(readText("typed.log").endsWith("queued one\nqueued two\nqueued three\n"));
    CHECK_FALSE(stopped());
    CHECK_FALSE(std::any_of(events.begin(), events.end(), [](const Event &e) {
        return std::holds_alternative<EvSendFailed>(e);
    }));
    AttachInput::setSubmitTimeoutMs(10'000);

    // Stopped by hand, the session takes the old way again.
    backend.stopAgentSession(conv);
    REQUIRE(QTest::qWaitFor(stopped, 8000));
    QTest::qWait(500);
    send("fourth");
    REQUIRE(answered("echo fourth"));
    CHECK(readText("calls.log").contains("-- fourth"));

    AttachInput::setAttachTimeoutMs(15'000);
    qunsetenv("FAKE_ATTACH");
    QProcess::execute("kill", {readText("wpid-abcdef11").trimmed()});
}
#endif

TEST_CASE("a branched-off session is a thread in its parent", "[claude][backend][thread]") {
    FakeClaudeHome home;
    home.writeSession("idle");
    const QByteArray shared = prompt("hi", "2026-09-25T10:00:00.000Z") +
                              assistantText("Hello!", "2026-09-25T10:00:01.000Z") +
                              turnEnd("2026-09-25T10:00:01.500Z");
    home.append(shared);
    QTest::qWait(20); // the fork's file is the younger one (creation times in ms)
    // The fork: a copy of the parent's records so far, then its own question.
    {
        QFile f(home.dir.path() + "/projects/-src-app/S2.jsonl");
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(
            shared + prompt("side question?", "2026-09-25T10:00:03.000Z") +
            assistantText("Side answer.", "2026-09-25T10:00:04.000Z") +
            turnEnd("2026-09-25T10:00:04.500Z")
        );
        QFile s(home.dir.path() + "/sessions/2.json");
        REQUIRE(s.open(QIODevice::WriteOnly));
        s.write(QJsonDocument(
                    QJsonObject{
                        {"pid", QCoreApplication::applicationPid()},
                        {"sessionId", "S2"},
                        {"cwd", "/src/app"},
                        {"status", "idle"},
                        {"entrypoint", "cli"},
                    }
        )
                    .toJson());
    }
    // The parent went on after the branch.
    home.append(
        prompt("more", "2026-09-25T10:00:05.000Z") +
        assistantText("Sure.", "2026-09-25T10:00:06.000Z") + turnEnd("2026-09-25T10:00:06.500Z")
    );

    claude_code::Backend backend(Credentials{});
    backend.connectRealtime();
    const auto convs = collect(backend.loadConversations());
    REQUIRE(convs.size() == 1);
    REQUIRE(convs[0][0].id.value == "S1");
    CHECK(convs[0].size() == 1); // the branch isn't a session in the list

    const ConversationId conv{"S1"};
    const auto           page = collect(backend.loadHistory(conv, std::nullopt));
    REQUIRE(page.size() == 1);
    const auto &msgs = page[0].messages;
    REQUIRE(msgs.size() == 5);
    CHECK(msgs[2].text.text == "side question?"); // in time order, between the turns
    CHECK(msgs[2].author == UserId{"me"});
    CHECK(msgs[2].replyCount == 1);
    CHECK(msgs[3].text.text == "more");
    const Ts root = msgs[2].ts;
    CHECK(backend.threadAcceptsReplies(conv, root));
    CHECK_FALSE(backend.threadAcceptsReplies(conv, msgs[0].ts));

    const auto thread = collect(backend.loadThread(conv, root, std::nullopt));
    REQUIRE(thread.size() == 1);
    REQUIRE(thread[0].messages.size() == 2);
    CHECK(thread[0].messages[0].ts == root);
    CHECK(thread[0].messages[1].text.text == "Side answer.");
    CHECK(thread[0].messages[1].threadRoot == root);
    CHECK(thread[0].messages[1].parentUserId == UserId{"me"}); // notifies as a reply to you

    // "Open as session": the branch joins the list, its root leaves the parent.
    CHECK(backend.openThreadAsSession(conv, root).value == "S2");
    const auto after = collect(backend.loadConversations());
    REQUIRE(after.size() == 1);
    CHECK(after[0].size() == 2);
    const auto parent = collect(backend.loadHistory(conv, std::nullopt));
    REQUIRE(parent.size() == 1);
    CHECK(parent[0].messages.size() == 4);
}

TEST_CASE("/btw is offered with Claude Code's commands", "[claude][commands]") {
    FakeClaudeHome home;
    home.writeSession("idle");
    home.append(prompt("hi", "2026-09-25T10:00:00.000Z"));
    claude_code::Backend backend(Credentials{});
    backend.connectRealtime();
    const auto cmds = backend.conversationCommands(ConversationId{"S1"});
    REQUIRE_FALSE(cmds.empty());
    CHECK(cmds[0].name == "btw");
    const auto local = [&](const QString &name) {
        return std::any_of(cmds.begin(), cmds.end(), [&](const SlashCommand &c) {
            return c.name == name && c.local;
        });
    };
    CHECK(local("status")); // run by msga itself, never sent to Claude
    CHECK(local("clear"));

    const ConversationId conv{"S1"};
    const auto           status = backend.runLocalCommand(conv, "status", {});
    REQUIRE_FALSE(status.status.empty());
    CHECK(std::any_of(status.status.begin(), status.status.end(), [](const auto &row) {
        return row.second == "S1";
    }));
    // /clear needs the claude tool (none in this test): it says so, opens nothing.
    const auto clear = backend.runLocalCommand(conv, "clear", {});
    CHECK_FALSE(clear.error.isEmpty());
    CHECK(clear.open.value.isEmpty());
}

TEST_CASE(
    "a subagent run is a thread holding the subagent's transcript", "[claude][backend][thread]"
) {
    FakeClaudeHome home;
    home.writeSession("idle");
    home.append(
        prompt("research it", "2026-09-25T10:00:00.000Z") +
        toolUse("a1", "Agent", {{"description", "Read the docs"}}, "2026-09-25T10:00:01.000Z") +
        toolResult("a1", "2026-09-25T10:00:09.000Z", false, "agent42") +
        assistantText("Done.", "2026-09-25T10:00:10.000Z") + turnEnd("2026-09-25T10:00:11.000Z")
    );
    QDir().mkpath(home.dir.path() + "/projects/-src-app/S1/subagents");
    {
        QFile f(home.dir.path() + "/projects/-src-app/S1/subagents/agent-agent42.jsonl");
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(
            prompt("Read the docs and report", "2026-09-25T10:00:02.000Z", false) +
            assistantText("The docs say X.", "2026-09-25T10:00:08.000Z") +
            turnEnd("2026-09-25T10:00:08.500Z")
        );
    }

    claude_code::Backend backend(Credentials{});
    backend.connectRealtime();
    const ConversationId conv{"S1"};
    const auto           page = collect(backend.loadHistory(conv, std::nullopt));
    REQUIRE(page.size() == 1);
    const auto root =
        std::find_if(page[0].messages.begin(), page[0].messages.end(), [](const Message &m) {
            return m.replyCount > 0;
        });
    REQUIRE(root != page[0].messages.end());
    CHECK(root->replyCount == 2);
    CHECK(root->text.text == "Subagent: Read the docs");

    const auto thread = collect(backend.loadThread(conv, root->ts, std::nullopt));
    REQUIRE(thread.size() == 1);
    REQUIRE(thread[0].messages.size() == 3); // root + prompt + answer
    CHECK(thread[0].messages[0].ts == root->ts);
    CHECK(thread[0].messages[2].text.text == "The docs say X.");
    CHECK(thread[0].messages[2].threadRoot == root->ts);
    // A reply goes on to the subagent (relayed by the session); it isn't a
    // session of its own.
    CHECK(backend.threadAcceptsReplies(conv, root->ts));
    CHECK_FALSE(backend.threadOpensAsSession(conv, root->ts));
}

TEST_CASE("a background subagent at work thinks in its thread", "[claude][backend][thread]") {
    FakeClaudeHome home;
    home.writeSession("busy");
    home.append(
        prompt("research it", "2026-09-25T10:00:00.000Z") +
        toolUse(
            "a1",
            "Agent",
            {{"description", "Read the docs"}, {"subagent_type", "designer"}},
            "2026-09-25T10:00:01.000Z"
        ) +
        toolResult("a1", "2026-09-25T10:00:01.500Z", false, "agent42", "async_launched")
    );
    const QString sub = home.dir.path() + "/projects/-src-app/S1/subagents/agent-agent42.jsonl";
    QDir().mkpath(QFileInfo(sub).path());
    const auto subAppend = [&](const QByteArray &bytes) {
        QFile f(sub);
        REQUIRE(f.open(QIODevice::Append));
        f.write(bytes);
    };
    subAppend(
        prompt("Read the docs and report", "2026-09-25T10:00:02.000Z", false) +
        toolUse("t1", "Read", {{"file_path", "/docs"}}, "2026-09-25T10:00:03.000Z")
    );

    claude_code::Backend backend(Credentials{});
    std::vector<Event>   events;
    rpl::lifetime        lt;
    backend.events() | rpl::on_next([&](Event e) { events.push_back(std::move(e)); }, lt);
    const auto threadTyping = [&]() -> std::optional<EvTyping> {
        for (auto it = events.rbegin(); it != events.rend(); ++it)
            if (const auto *t = std::get_if<EvTyping>(&*it); t && t->threadRoot)
                return *t;
        return std::nullopt;
    };
    backend.connectRealtime();
    const ConversationId conv{"S1"};
    const auto           page = collect(backend.loadHistory(conv, std::nullopt));
    REQUIRE(page.size() == 1);
    const auto root =
        std::find_if(page[0].messages.begin(), page[0].messages.end(), [](const Message &m) {
            return m.replyCount > 0;
        });
    REQUIRE(root != page[0].messages.end());

    // Running: thinking in its thread, as the teammate, since its first record.
    auto typing = threadTyping();
    REQUIRE(typing);
    CHECK(typing->conv == conv);
    CHECK(*typing->threadRoot == root->ts);
    CHECK(typing->user == UserId{"claude:role:designer"});
    CHECK(typing->thinkingSinceMs == 1790330402000);
    // The session thinks in the chat as ever.
    CHECK(std::any_of(events.begin(), events.end(), [](const Event &e) {
        const auto *t = std::get_if<EvTyping>(&e);
        return t && !t->threadRoot && t->user.value == "claude:agent";
    }));

    // It stops: the session is notified, a moment after its last record.
    subAppend(assistantText("The docs say X.", "2026-09-25T10:00:08.000Z"));
    home.append(taskStopped("agent42", "2026-09-25T10:00:08.050Z"));
    QTest::qWait(500); // the transcript's change is picked up
    events.clear();
    QTest::qWait(3500); // the typing pump runs every 3 s while anything is busy
    CHECK_FALSE(threadTyping());

    // A reply relayed to it starts it again — its clock from then.
    subAppend(line({
        {"type", "user"},
        {"isMeta", true},
        {"timestamp", "2026-09-25T10:05:00.000Z"},
        {"origin", QJsonObject{{"kind", "coordinator"}}},
        {"message", QJsonObject{{"role", "user"}, {"content", "Which page?"}}},
    }));
    REQUIRE(QTest::qWaitFor([&] { return threadTyping().has_value(); }, 5000));
    typing = threadTyping();
    CHECK(typing->thinkingSinceMs == 1790330700000);

    // A session that's gone runs no subagents, whatever its files say.
    QFile::remove(home.dir.path() + "/sessions/1.json");
    QTest::qWait(500);
    events.clear();
    QTest::qWait(3500);
    CHECK_FALSE(threadTyping());
}

TEST_CASE("a subagent started as a teammate speaks as that teammate", "[claude][backend][thread]") {
    // Seen live 2026-09-26: "start a @Designer subagent" ran subagent_type
    // "designer", yet its thread read as the session's Generalist.
    FakeClaudeHome home;
    home.writeSession("idle");
    home.append(
        prompt("draw it", "2026-09-25T10:00:00.000Z") +
        toolUse(
            "a1",
            "Agent",
            {{"description", "Draw a test image"}, {"subagent_type", "designer"}},
            "2026-09-25T10:00:01.000Z"
        ) +
        toolResult("a1", "2026-09-25T10:00:09.000Z", false, "agent42") +
        assistantText("Done.", "2026-09-25T10:00:10.000Z") + turnEnd("2026-09-25T10:00:11.000Z")
    );
    QDir().mkpath(home.dir.path() + "/projects/-src-app/S1/subagents");
    {
        QFile f(home.dir.path() + "/projects/-src-app/S1/subagents/agent-agent42.jsonl");
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(
            prompt("Draw a test image", "2026-09-25T10:00:02.000Z", false) +
            assistantText("Drew it.", "2026-09-25T10:00:08.000Z") +
            turnEnd("2026-09-25T10:00:08.500Z")
        );
    }

    claude_code::Backend backend(Credentials{});
    backend.connectRealtime();
    const ConversationId conv{"S1"};
    const UserId         designer{"claude:role:designer"};
    const UserId         generalist{"claude:agent"};
    const auto           page = collect(backend.loadHistory(conv, std::nullopt));
    REQUIRE(page.size() == 1);
    const auto &msgs = page[0].messages;
    const auto  root =
        std::find_if(msgs.begin(), msgs.end(), [](const Message &m) { return m.replyCount > 0; });
    REQUIRE(root != msgs.end());
    CHECK(root->author == designer);
    CHECK(msgs.back().author == generalist); // the session's own answer

    const auto thread = collect(backend.loadThread(conv, root->ts, std::nullopt));
    REQUIRE(thread.size() == 1);
    REQUIRE(thread[0].messages.size() == 3);
    CHECK(thread[0].messages[1].author == generalist); // the prompt: the session wrote it
    CHECK(thread[0].messages[2].author == designer);
    CHECK(thread[0].messages[2].parentUserId == designer);
}

TEST_CASE("a plain subagent isn't taken for its session's teammate", "[claude][backend][thread]") {
    // Seen live 2026-09-26: a Researcher session with no teammate types asked
    // for two @Engineers spawned subagent_type "claude" with a self-written
    // "Role: engineer." prompt — and both threads read as the Researcher.
    FakeClaudeHome home;
    home.writeSession("idle");
    const QByteArray snapshot = line(
        {{"type", "attachment"},
         {"timestamp", "2026-09-25T10:00:00.500Z"},
         {"attachment",
          QJsonObject{
              {"type", "prompt_snapshot"},
              {"systemPrompt", QJsonArray{"# Your role: Researcher (msga: researcher)\nDig."}}
          }}}
    );
    home.append(
        prompt("ask engineers", "2026-09-25T10:00:00.000Z") + snapshot +
        toolUse(
            "a1",
            "Agent",
            {{"description", "Fix it"},
             {"subagent_type", "claude"},
             {"prompt", "Role: engineer. Fix it."}},
            "2026-09-25T10:00:01.000Z"
        ) +
        toolResult("a1", "2026-09-25T10:00:02.000Z", false, "agent1") +
        toolUse(
            "a2",
            "Agent",
            {{"description", "Look around"}, {"subagent_type", "Explore"}, {"prompt", "Look."}},
            "2026-09-25T10:00:03.000Z"
        ) +
        toolResult("a2", "2026-09-25T10:00:04.000Z", false, "agent2") +
        assistantText("Started both.", "2026-09-25T10:00:05.000Z") +
        turnEnd("2026-09-25T10:00:06.000Z")
    );

    claude_code::Backend backend(Credentials{});
    backend.connectRealtime();
    const auto page = collect(backend.loadHistory(ConversationId{"S1"}, std::nullopt));
    REQUIRE(page.size() == 1);
    QHash<QString, UserId> byText;
    for (const auto &m : page[0].messages)
        byText.insert(m.text.text, m.author);
    CHECK(byText.value("Started both.") == UserId{"claude:role:researcher"});
    CHECK(byText.value("Subagent: Fix it") == UserId{"claude:role:engineer"});
    CHECK(byText.value("Subagent: Look around") == UserId{"claude:agent"});
}

TEST_CASE(
    "a teammate at work as a subagent in another's session shows as working",
    "[claude][backend][thread][presence]"
) {
    // Seen live 2026-09-28: an Engineer a Researcher session started was
    // "thinking (2m 47s)…" in its thread, its dot in the team list clear.
    FakeClaudeHome home;
    home.writeSession("idle"); // the Researcher itself waits
    const QByteArray snapshot = line(
        {{"type", "attachment"},
         {"timestamp", "2026-09-25T10:00:00.500Z"},
         {"attachment",
          QJsonObject{
              {"type", "prompt_snapshot"},
              {"systemPrompt", QJsonArray{"# Your role: Researcher (msga: researcher)\nDig."}}
          }}}
    );
    home.append(
        prompt("ask an engineer", "2026-09-25T10:00:00.000Z") + snapshot +
        toolUse(
            "a1",
            "Agent",
            {{"description", "Fix it"}, {"subagent_type", "engineer"}, {"prompt", "Fix it."}},
            "2026-09-25T10:00:01.000Z"
        ) +
        toolResult("a1", "2026-09-25T10:00:01.500Z", false, "eng1", "async_launched") +
        toolUse(
            "a2",
            "Agent",
            {{"description", "Look around"}, {"subagent_type", "Explore"}, {"prompt", "Look."}},
            "2026-09-25T10:00:02.000Z"
        ) +
        toolResult("a2", "2026-09-25T10:00:02.500Z", false, "exp1", "async_launched") +
        // A foreground Engineer, done long ago: its result was its stop.
        toolUse(
            "a3",
            "Agent",
            {{"description", "Check it"},
             {"subagent_type", "engineer"},
             {"prompt", "Check it."},
             {"run_in_background", false}},
            "2026-09-25T09:00:00.000Z"
        ) +
        toolResult("a3", "2026-09-25T09:05:00.000Z", false, "eng0", "completed") +
        assistantText("Started them.", "2026-09-25T10:00:03.000Z") +
        turnEnd("2026-09-25T10:00:04.000Z")
    );
    const QString subDir = home.dir.path() + "/projects/-src-app/S1/subagents/";
    QDir().mkpath(subDir);
    const auto subAppend = [&](const QString &agentId, const QByteArray &bytes) {
        QFile f(subDir + "agent-" + agentId + ".jsonl");
        REQUIRE(f.open(QIODevice::Append));
        f.write(bytes);
    };
    subAppend("eng1", prompt("Fix it.", "2026-09-25T10:00:02.000Z", false));
    subAppend("exp1", prompt("Look.", "2026-09-25T10:00:03.000Z", false));
    subAppend(
        "eng0",
        prompt("Check it.", "2026-09-25T09:00:01.000Z", false) +
            assistantText("Checked.", "2026-09-25T09:04:59.000Z")
    );

    claude_code::Backend backend(Credentials{});
    std::vector<Event>   events;
    rpl::lifetime        lt;
    backend.events() | rpl::on_next([&](Event e) { events.push_back(std::move(e)); }, lt);
    const UserId engineer{"claude:role:engineer"};
    const UserId researcher{"claude:role:researcher"};
    const UserId generalist{"claude:agent"};
    const auto   present = [&](const UserId &id) {
        const auto v = collect(backend.loadPresence(id));
        return !v.empty() && v.front();
    };
    const auto announced = [&](const UserId &id) -> std::optional<bool> {
        for (auto it = events.rbegin(); it != events.rend(); ++it)
            if (const auto *p = std::get_if<EvPresenceChanged>(&*it); p && p->user == id)
                return p->active;
        return std::nullopt;
    };
    const auto card = [&](const UserId &id) {
        const auto users = collect(backend.loadUsers());
        for (const User &u : users.front())
            if (u.id == id)
                return u;
        return User{};
    };
    backend.connectRealtime();

    // Its subagent runs: the Engineer is green, dot and card alike — its own
    // session idle, the Researcher's too. The Explore one lights no one.
    CHECK(present(engineer));
    CHECK(card(engineer).isActive);
    CHECK_FALSE(card(engineer).unavailable);
    CHECK_FALSE(present(researcher));
    CHECK_FALSE(present(generalist));

    // It stops: the dot clears, told to the list.
    subAppend("eng1", assistantText("Fixed.", "2026-09-25T10:01:00.000Z"));
    home.append(taskStopped("eng1", "2026-09-25T10:01:00.050Z"));
    REQUIRE(QTest::qWaitFor([&] { return announced(engineer) == false; }, 5000));
    CHECK_FALSE(present(engineer));
    CHECK_FALSE(card(engineer).isActive);

    // A reply relayed to it starts it again. Nothing watches a subagent's own
    // file: the typing pump, ticking for the Explore one, notices.
    events.clear();
    subAppend(
        "eng1",
        line({
            {"type", "user"},
            {"isMeta", true},
            {"timestamp", "2026-09-25T10:05:00.000Z"},
            {"origin", QJsonObject{{"kind", "coordinator"}}},
            {"message", QJsonObject{{"role", "user"}, {"content", "And the tests?"}}},
        })
    );
    REQUIRE(QTest::qWaitFor([&] { return announced(engineer) == true; }, 5000));
    CHECK(present(engineer));

    // Both stop: the pump's last tick, the one that finds nothing at work,
    // still clears the dot.
    events.clear();
    subAppend("eng1", assistantText("Tested.", "2026-09-25T10:06:00.000Z"));
    subAppend("exp1", assistantText("Looked.", "2026-09-25T10:06:00.000Z"));
    home.append(
        taskStopped("eng1", "2026-09-25T10:06:00.050Z") +
        taskStopped("exp1", "2026-09-25T10:06:00.050Z")
    );
    REQUIRE(QTest::qWaitFor([&] { return announced(engineer) == false; }, 5000));
    CHECK_FALSE(present(engineer));
    CHECK_FALSE(present(generalist));
    // No subagent thinks any more — the finished foreground one neither.
    events.clear();
    QTest::qWait(3500);
    CHECK_FALSE(std::any_of(events.begin(), events.end(), [](const Event &e) {
        const auto *t = std::get_if<EvTyping>(&e);
        return t && t->threadRoot;
    }));
}

TEST_CASE(
    "a reply relayed to a subagent is in its thread, not the chat", "[claude][backend][thread]"
) {
    FakeClaudeHome home;
    home.writeSession("idle");
    // As Claude Code 2.1.282 records it: the relay is the session's prompt; the
    // subagent gets it as the coordinator's (isMeta, hidden) and answers.
    home.append(
        prompt("research it", "2026-09-25T10:00:00.000Z") +
        toolUse("a1", "Agent", {{"description", "Read the docs"}}, "2026-09-25T10:00:01.000Z") +
        toolResult("a1", "2026-09-25T10:00:02.000Z", false, "agent42") +
        assistantText("Started.", "2026-09-25T10:00:03.000Z") +
        turnEnd("2026-09-25T10:00:04.000Z") +
        prompt(subagentReplyPrompt("agent42", "Which page?"), "2026-09-25T10:01:00.000Z") +
        toolUse("s1", "SendMessage", {{"to", "agent42"}}, "2026-09-25T10:01:01.000Z") +
        toolResult("s1", "2026-09-25T10:01:02.000Z") +
        assistantText("Sent.", "2026-09-25T10:01:03.000Z") + turnEnd("2026-09-25T10:01:04.000Z")
    );
    QDir().mkpath(home.dir.path() + "/projects/-src-app/S1/subagents");
    {
        QFile f(home.dir.path() + "/projects/-src-app/S1/subagents/agent-agent42.jsonl");
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(
            prompt("Read the docs", "2026-09-25T10:00:01.500Z", false) +
            assistantText("Reading.", "2026-09-25T10:00:05.000Z") +
            line({
                {"type", "user"},
                {"isMeta", true},
                {"origin", QJsonObject{{"kind", "coordinator"}}},
                {"timestamp", "2026-09-25T10:01:02.000Z"},
                {"message",
                 QJsonObject{
                     {"content",
                      "The coordinator sent a message while you were working:\nWhich "
                      "page?\n\nAddress this before completing your current task."}
                 }},
            }) +
            assistantText("Page 3.", "2026-09-25T10:01:05.000Z")
        );
    }

    claude_code::Backend backend(Credentials{});
    backend.connectRealtime();
    const ConversationId conv{"S1"};
    const auto           page = collect(backend.loadHistory(conv, std::nullopt));
    REQUIRE(page.size() == 1);
    const auto &msgs = page[0].messages;
    CHECK(std::none_of(msgs.begin(), msgs.end(), [](const Message &m) {
        return m.text.text.contains("Which page?") || m.threadRoot.has_value();
    }));
    const auto root =
        std::find_if(msgs.begin(), msgs.end(), [](const Message &m) { return m.replyCount > 0; });
    REQUIRE(root != msgs.end());
    CHECK(root->replyCount == 4); // its prompt and 2 answers, and the reply

    const auto thread = collect(backend.loadThread(conv, root->ts, std::nullopt));
    REQUIRE(thread.size() == 1);
    const auto &t = thread[0].messages;
    REQUIRE(t.size() == 5);
    CHECK(t[2].text.text == "Reading.");
    CHECK(t[3].text.text == "Which page?"); // yours, in time order
    CHECK(t[3].author == UserId{"me"});
    CHECK(t[3].threadRoot == root->ts);
    CHECK(t[4].text.text == "Page 3.");
    CHECK(root->latestReply == t[4].ts);
}

// ── Deleting messages ─────────────────────────────────────────────────────────

namespace {

// A record as Claude Code links them: uuid, and the record before it.
QByteArray linked(QByteArray rec, const char *uuid, const char *parent) {
    QJsonObject o   = QJsonDocument::fromJson(rec).object();
    o["uuid"]       = uuid;
    o["parentUuid"] = parent ? QJsonValue(parent) : QJsonValue();
    return line(o);
}

// One block of an assistant message (Claude Code writes each as a record).
QByteArray assistantBlock(const QJsonObject &block, const char *msgId, const char *ts) {
    return line({
        {"type", "assistant"},
        {"timestamp", ts},
        {"message", QJsonObject{{"id", msgId}, {"content", QJsonArray{block}}}},
    });
}

QByteArray thinking(const char *msgId, const char *ts) {
    return assistantBlock({{"type", "thinking"}, {"thinking", ""}, {"signature", "x"}}, msgId, ts);
}

QByteArray answer(const QString &text, const char *msgId, const char *ts) {
    return assistantBlock({{"type", "text"}, {"text", text}}, msgId, ts);
}

// Two turns: the second prompt thinks, calls a tool, thinks again, answers.
QByteArray linkedTurns() {
    return linked(prompt("keep this", "2026-09-25T10:00:00.000Z"), "p1", nullptr) +
           linked(thinking("m1", "2026-09-25T10:00:01.000Z"), "t1", "p1") +
           linked(answer("Kept.", "m1", "2026-09-25T10:00:02.000Z"), "a1", "t1") +
           linked(turnEnd("2026-09-25T10:00:03.000Z"), "e1", "a1") +
           linked(prompt("the secret is banana", "2026-09-25T10:01:00.000Z"), "p2", "e1") +
           linked(thinking("m2", "2026-09-25T10:01:01.000Z"), "t2", "p2") +
           linked(
               toolUse("tu", "Bash", {{"command", "ls"}}, "2026-09-25T10:01:02.000Z"), "u2", "t2"
           ) +
           linked(toolResult("tu", "2026-09-25T10:01:03.000Z"), "r2", "u2") +
           linked(thinking("m3", "2026-09-25T10:01:04.000Z"), "t3", "r2") +
           linked(answer("Noted.", "m3", "2026-09-25T10:01:05.000Z"), "a3", "t3") +
           linked(turnEnd("2026-09-25T10:01:06.000Z"), "e2", "a3") +
           R"({"type":"last-prompt","leafUuid":"e2"})"
           "\n" +
           // Claude Code's bookkeeping copies of the second prompt.
           R"({"type":"queue-operation","operation":"enqueue","content":"the secret is banana"})"
           "\n"
           R"({"type":"last-prompt","lastPrompt":"the secret is banana","leafUuid":"e2"})"
           "\n";
}

// uuid → parentUuid of every record in the file ("-" for none), in file order.
QList<std::pair<QString, QString>> chain(const QString &path) {
    QFile f(path);
    REQUIRE(f.open(QIODevice::ReadOnly));
    QList<std::pair<QString, QString>> out;
    for (const QByteArray &l : f.readAll().split('\n')) {
        const QJsonObject o = QJsonDocument::fromJson(l).object();
        if (o.contains("uuid"))
            out.append({o["uuid"].toString(), o["parentUuid"].toString("-")});
    }
    return out;
}

} // namespace

TEST_CASE("a prompt leaves the transcript with its turn's thinking", "[claude][delete]") {
    QTemporaryDir dir;
    const QString path = dir.path() + "/s.jsonl";
    {
        QFile f(path);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(linkedTurns());
    }
    TranscriptParser before;
    {
        QFile f(path);
        REQUIRE(f.open(QIODevice::ReadOnly));
        before.feed(f.readAll());
    }
    REQUIRE(before.items().size() == 5); // prompt, answer, prompt, tool calls, answer
    CHECK(before.items()[2].uuid == "p2");
    CHECK(before.items()[3].uuid.isEmpty()); // tool calls can't go on their own
    CHECK(before.items()[4].uuid == "a3");

    REQUIRE(removeFromTranscript(path, "p2"));
    // The prompt and both thoughts of its turn are gone; the tool call and its
    // result stay, and what followed a removed record now follows its parent.
    CHECK(
        chain(path) == QList<std::pair<QString, QString>>{
                           {"p1", "-"},
                           {"t1", "p1"},
                           {"a1", "t1"},
                           {"e1", "a1"},
                           {"u2", "e1"},
                           {"r2", "u2"},
                           {"a3", "r2"},
                           {"e2", "a3"},
                       }
    );
    QFile f(path);
    REQUIRE(f.open(QIODevice::ReadOnly));
    const QByteArray after = f.readAll();
    CHECK_FALSE(after.contains("banana"));
    // Records that needed no new link are kept byte for byte.
    CHECK(after.startsWith(linked(prompt("keep this", "2026-09-25T10:00:00.000Z"), "p1", nullptr)));
    CHECK(after.endsWith(
        R"({"type":"last-prompt","leafUuid":"e2"})"
        "\n"
    )); // its copies are gone
    f.close();

    CHECK_FALSE(removeFromTranscript(path, "p2")); // no longer there
}

TEST_CASE("an answer leaves the transcript with its own thinking", "[claude][delete]") {
    QTemporaryDir dir;
    const QString path = dir.path() + "/s.jsonl";
    {
        QFile f(path);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(linkedTurns().replace(R"("leafUuid":"e2")", R"("leafUuid":"a1")"));
    }
    REQUIRE(removeFromTranscript(path, "a1"));
    const auto c = chain(path);
    REQUIRE(c.size() == 9);
    CHECK(c[0] == std::pair<QString, QString>{"p1", "-"});
    CHECK(c[1] == std::pair<QString, QString>{"e1", "p1"}); // t1 and a1 are gone
    CHECK(c[3] == std::pair<QString, QString>{"t2", "p2"}); // the next turn's thinking stays
    QFile f(path);
    REQUIRE(f.open(QIODevice::ReadOnly));
    CHECK(f.readAll().contains(R"("leafUuid":"p1")")); // pointers along the chain move too
}

TEST_CASE("deleting a message in a session", "[claude][backend][delete]") {
    FakeClaudeHome home;
    // A finished background session nothing is writing to…
    const QString  sid          = "8d953db6-f3be-4b02-8f0f-09aea0343b3e";
    const QString  bgTranscript = home.dir.path() + "/projects/-src-app/" + sid + ".jsonl";
    QDir(home.dir.path()).mkpath("jobs/8d953db6");
    {
        QFile f(home.dir.path() + "/jobs/8d953db6/state.json");
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(
                    QJsonObject{
                        {"state", "done"},
                        {"sessionId", sid},
                        {"cwd", "/src/app"},
                        {"name", "finished"},
                        {"linkScanPath", bgTranscript},
                    }
        )
                    .toJson());
        QFile t(bgTranscript);
        REQUIRE(t.open(QIODevice::WriteOnly));
        t.write(linkedTurns());
    }
    // …and one open in a terminal, which keeps what it has said.
    home.writeSession("idle");
    home.append(
        linked(prompt("hi", "2026-09-25T09:00:00.000Z"), "q1", nullptr) +
        linked(answer("Hello!", "m9", "2026-09-25T09:00:01.000Z"), "q2", "q1") +
        linked(turnEnd("2026-09-25T09:00:02.000Z"), "q3", "q2")
    );

    claude_code::Backend backend(Credentials{});
    std::vector<Event>   events;
    rpl::lifetime        lt;
    backend.events() | rpl::on_next([&](Event e) { events.push_back(std::move(e)); }, lt);
    backend.connectRealtime();
    REQUIRE(collect(backend.loadConversations())[0].size() == 2);
    CHECK(backend.capabilities().deleteMessage);
    CHECK(backend.capabilities().deleteAnyMessage); // Claude's answers too

    const ConversationId term{"S1"};
    const auto           termMsgs = collect(backend.loadHistory(term, std::nullopt))[0].messages;
    REQUIRE(termMsgs.size() == 2);
    CHECK_FALSE(backend.canDeleteMessage(term, termMsgs[0].ts));

    const ConversationId conv{sid};
    const auto           msgs = collect(backend.loadHistory(conv, std::nullopt))[0].messages;
    REQUIRE(msgs.size() == 5);
    CHECK(backend.canDeleteMessage(conv, msgs[2].ts));       // a prompt
    CHECK(backend.canDeleteMessage(conv, msgs[4].ts));       // an answer
    CHECK_FALSE(backend.canDeleteMessage(conv, msgs[3].ts)); // tool calls
    CHECK_FALSE(backend.canDeleteMessage(conv, "1.000001"));

    events.clear();
    backend.deleteMessage(conv, msgs[2].ts);
    CHECK(std::any_of(events.begin(), events.end(), [&](const Event &e) {
        const auto *d = std::get_if<EvMessageDeleted>(&e);
        return d && d->conv == conv && d->ts == msgs[2].ts;
    }));
    const auto left = collect(backend.loadHistory(conv, std::nullopt))[0].messages;
    REQUIRE(left.size() == 4);
    CHECK(left[1].text.text == "Kept.");
    CHECK(left[3].text.text == "Noted.");
    QFile f(bgTranscript);
    REQUIRE(f.open(QIODevice::ReadOnly));
    CHECK_FALSE(f.readAll().contains("banana"));
}

TEST_CASE("reactions stay in msga", "[claude][backend][reactions]") {
    FakeClaudeHome home;
    home.writeSession("idle");
    home.append(
        linked(prompt("hi", "2026-09-25T09:00:00.000Z"), "q1", nullptr) +
        linked(answer("Hello!", "m9", "2026-09-25T09:00:01.000Z"), "q2", "q1") +
        linked(turnEnd("2026-09-25T09:00:02.000Z"), "q3", "q2")
    );

    claude_code::Backend backend(Credentials{});
    std::vector<Event>   events;
    rpl::lifetime        lt;
    backend.events() | rpl::on_next([&](Event e) { events.push_back(std::move(e)); }, lt);
    backend.connectRealtime();
    REQUIRE(collect(backend.loadConversations())[0].size() == 1);
    CHECK(backend.capabilities().reactions);

    const ConversationId conv{"S1"};
    const auto           msgs = collect(backend.loadHistory(conv, std::nullopt))[0].messages;
    REQUIRE(msgs.size() == 2);
    const Ts answerTs = msgs[1].ts;

    events.clear();
    backend.addReaction(conv, answerTs, "tada");
    backend.addReaction(conv, answerTs, "tada"); // once is enough
    backend.addReaction(conv, answerTs, "heart");
    CHECK(std::count_if(events.begin(), events.end(), [&](const Event &e) {
              const auto *r = std::get_if<EvReactionAdded>(&e);
              return r && r->conv == conv && r->ts == answerTs;
          }) == 2);

    // Served again with the history (a chat reloaded after a switch)…
    auto again = collect(backend.loadHistory(conv, std::nullopt))[0].messages;
    REQUIRE(again.size() == 2);
    REQUIRE(again[1].reactions.size() == 2);
    CHECK(again[1].reactions[0].name == "tada");
    CHECK(again[1].reactions[0].count == 1);
    CHECK(again[1].reactions[0].users == std::vector<UserId>{UserId{"me"}});
    CHECK(again[0].reactions.empty());

    // …not news when the transcript grows, and never written to it.
    events.clear();
    home.append(linked(prompt("more", "2026-09-25T09:01:00.000Z"), "q4", "q3"));
    REQUIRE(QTest::qWaitFor([&] {
        return std::any_of(events.begin(), events.end(), [](const Event &e) {
            return std::holds_alternative<EvMessageNew>(e);
        });
    }));
    CHECK_FALSE(std::any_of(events.begin(), events.end(), [](const Event &e) {
        return std::holds_alternative<EvMessageChanged>(e);
    }));
    QFile transcript(home.transcript);
    REQUIRE(transcript.open(QIODevice::ReadOnly));
    CHECK_FALSE(transcript.readAll().contains("tada"));

    events.clear();
    backend.removeReaction(conv, answerTs, "tada");
    backend.removeReaction(conv, answerTs, "tada");
    CHECK(std::count_if(events.begin(), events.end(), [](const Event &e) {
              return std::holds_alternative<EvReactionRemoved>(e);
          }) == 1);
    again = collect(backend.loadHistory(conv, std::nullopt))[0].messages;
    REQUIRE(again[1].reactions.size() == 1);
    CHECK(again[1].reactions[0].name == "heart");
}

// ── Finding sessions ("Find a session") ───────────────────────────────────────

namespace {

QByteArray titled(const char *type, const char *key, const QString &value) {
    return line({{"type", type}, {key, value}});
}

} // namespace

TEST_CASE("the catalog reads a transcript's title and prompts", "[claude][catalog]") {
    const QByteArray head =
        line(
            {{"type", "user"},
             {"cwd", "/src/app"},
             {"timestamp", "2026-09-25T10:00:00.000Z"},
             {"message",
              QJsonObject{{"content", "<local-command-caveat>x</local-command-caveat>"}}}}
        ) +
        prompt("fix   the\nbuild", "2026-09-25T10:00:01.000Z") +
        titled("ai-title", "aiTitle", "Build fix");
    CatalogEntry e;
    REQUIRE(catalogEntryFrom(head, "\n" + head, e));
    CHECK(e.cwd == "/src/app");
    CHECK(e.firstPrompt == "fix the build"); // one line; the caveat isn't a prompt
    CHECK(e.lastPrompt == "fix the build");
    CHECK(e.title == "Build fix");

    // The tail: a cut first line, the latest title (a /rename wins) and prompt.
    const QByteArray tail = R"(t":"cut off"})"
                            "\n" +
                            titled("custom-title", "customTitle", "My name") +
                            titled("last-prompt", "lastPrompt", "and the tests") +
                            titled("ai-title", "aiTitle", "Later title");
    CatalogEntry     e2;
    REQUIRE(catalogEntryFrom(head, tail, e2));
    CHECK(e2.title == "My name");
    CHECK(e2.firstPrompt == "fix the build");
    CHECK(e2.lastPrompt == "and the tests");

    CatalogEntry none;
    CHECK_FALSE(catalogEntryFrom(titled("ai-title", "aiTitle", "x"), "\n", none)); // no prompt
}

TEST_CASE(
    "every session is found, and one can be added to the list", "[claude][catalog][backend]"
) {
    FakeClaudeHome home;
    home.writeSession("idle");
    home.append(prompt("listed one", "2026-09-25T09:00:00.000Z"));
    // An ended session msga has never seen, in another folder, and a big one
    // whose middle is never read.
    const QString gone =
        home.dir.path() + "/projects/-src-other/aaaaaaaa-0000-0000-0000-000000000001.jsonl";
    writeFile(
        gone,
        line(
            {{"type", "user"},
             {"cwd", "/src/other"},
             {"timestamp", "2026-09-25T08:00:00.000Z"},
             {"origin", QJsonObject{{"kind", "human"}}},
             {"message", QJsonObject{{"content", "old question"}}}}
        ) + assistantText("Old answer.", "2026-09-25T08:00:01.000Z") +
            turnEnd("2026-09-25T08:00:02.000Z") + titled("ai-title", "aiTitle", "Old work")
    );
    QByteArray big = prompt("big start", "2026-09-25T07:00:00.000Z");
    while (big.size() < 400 * 1024)
        big += assistantText(QString(500, 'x'), "2026-09-25T07:00:01.000Z");
    big += titled("last-prompt", "lastPrompt", "big end");
    writeFile(
        home.dir.path() + "/projects/-src-big/bbbbbbbb-0000-0000-0000-000000000002.jsonl", big
    );

    const auto catalog = scanCatalog(home.dir.path() + "/projects");
    REQUIRE(catalog.size() == 3);
    const auto bigEntry = std::find_if(catalog.begin(), catalog.end(), [](const CatalogEntry &e) {
        return e.sessionId.startsWith("bbbbbbbb");
    });
    REQUIRE(bigEntry != catalog.end());
    CHECK(bigEntry->firstPrompt == "big start");
    CHECK(bigEntry->lastPrompt == "big end");

    claude_code::Backend backend(Credentials{});
    std::vector<Event>   events;
    rpl::lifetime        lt;
    backend.events() | rpl::on_next([&](Event e) { events.push_back(std::move(e)); }, lt);
    backend.connectRealtime();
    REQUIRE(collect(backend.loadConversations())[0].size() == 1);

    std::vector<FoundSession> found;
    bool                      answered = false;
    backend.findAgentSessions([&](std::vector<FoundSession> f) {
        found    = std::move(f);
        answered = true;
    });
    REQUIRE(QTest::qWaitFor([&] { return answered; }, 5000));
    REQUIRE(found.size() == 3);
    const auto byId = [&](const QString &prefix) {
        return *std::find_if(found.begin(), found.end(), [&](const FoundSession &f) {
            return f.id.startsWith(prefix);
        });
    };
    CHECK(byId("S1").listed == ConversationId{"S1"});
    const FoundSession old = byId("aaaaaaaa");
    CHECK(old.listed.value.isEmpty());
    CHECK(old.title == "Old work");
    CHECK(old.folder == "/src/other");

    events.clear();
    const ConversationId conv = backend.addFoundSession(old.id);
    CHECK(conv.value == old.id);
    CHECK(std::any_of(events.begin(), events.end(), [&](const Event &e) {
        const auto *c = std::get_if<EvChannelCreated>(&e);
        return c && c->conv.id == conv && c->conv.name == "Old work";
    }));
    CHECK(std::none_of(events.begin(), events.end(), [](const Event &e) {
        return std::holds_alternative<EvMessageNew>(e); // its history isn't news
    }));
    CHECK(collect(backend.loadConversations())[0].size() == 2);
    const auto history = collect(backend.loadHistory(conv, std::nullopt))[0].messages;
    REQUIRE(history.size() == 2);
    CHECK(history[1].text.text == "Old answer.");
    CHECK(backend.addFoundSession(old.id) == conv); // already there
    CHECK(backend.addFoundSession("nope").value.isEmpty());
}

TEST_CASE(
    "adding the original of a listed copy keeps both sessions", "[claude][catalog][backend]"
) {
    FakeClaudeHome   home;
    // S1 (listed) is a copy Claude Code made when an ended session was resumed:
    // the original's records, then more. The original itself isn't listed.
    const QByteArray original = prompt("/clear", "2026-09-25T08:00:00.000Z") +
                                assistantText("Cleared.", "2026-09-25T08:00:01.000Z") +
                                turnEnd("2026-09-25T08:00:02.000Z");
    const QString    origPath =
        home.dir.path() + "/projects/-src-app/cccccccc-0000-0000-0000-000000000003.jsonl";
    writeFile(origPath, original);
    QTest::qWait(20); // the copy is the younger file
    home.writeSession("idle");
    home.append(
        original + prompt("go on", "2026-09-25T09:00:00.000Z") +
        assistantText("Going.", "2026-09-25T09:00:01.000Z") + turnEnd("2026-09-25T09:00:02.000Z")
    );

    claude_code::Backend backend(Credentials{});
    backend.connectRealtime();
    REQUIRE(collect(backend.loadConversations())[0].size() == 1);

    const ConversationId conv = backend.addFoundSession("cccccccc-0000-0000-0000-000000000003");
    CHECK(conv.value == "cccccccc-0000-0000-0000-000000000003");
    const auto convs = collect(backend.loadConversations())[0];
    CHECK(convs.size() == 2); // the copy didn't turn into a thread of the original
    CHECK(collect(backend.loadHistory(ConversationId{"S1"}, std::nullopt))[0].messages.size() == 4);
}

// ── Files the agent made (cc_outputs) ───────────────────────────────────────

namespace {

QByteArray pngBytes(int w, int h) {
    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(Qt::red);
    QByteArray bytes;
    QBuffer    buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return bytes;
}

const QByteArray kSvg = R"(<svg xmlns="http://www.w3.org/2000/svg" width="240" height="160">)"
                        R"(<rect x="10" y="10" width="60" height="60" fill="blue"/></svg>)";

QByteArray isoAt(qint64 msecs) {
    return QDateTime::fromMSecsSinceEpoch(msecs, QTimeZone::UTC)
        .toString(Qt::ISODateWithMs)
        .toUtf8();
}

} // namespace

TEST_CASE("an answer names its files by path, or by folder and name", "[claude][outputs]") {
    QTemporaryDir tmp;
    const QString d = tmp.path();
    writeFile(d + "/out/test-image.svg", kSvg);
    writeFile(d + "/out/test-image.png", pngBytes(4, 4));
    writeFile(d + "/out/notes.txt", "not shown");
    writeFile(d + "/proj/mock.pdf", "%PDF-1.4");
    writeFile(d + "/abs.png", pngBytes(2, 2));

    // Seen live 2026-09-26: a folder, then the bare names under it.
    const QString     text  = "Files are in `" + d +
                              "/out/`:\n- `test-image.svg`\n- `test-image.png`\n"
                              "- notes.txt\nAlso **" +
                              d +
                              "/abs.png**, docs/missing.png, mock.pdf and "
                              "https://example.com/x.png.";
    const QStringList files = mentionedFiles(text, d + "/proj");
    CHECK(
        files == QStringList{
                     d + "/abs.png",
                     d + "/out/test-image.svg",
                     d + "/out/test-image.png",
                     d + "/proj/mock.pdf"
                 }
    );
}

TEST_CASE("an answer's files are copied, and only the ones made in its turn", "[claude][outputs]") {
    QTemporaryDir tmp;
    const QString d   = tmp.path();
    const qint64  now = QDateTime::currentMSecsSinceEpoch();
    writeFile(d + "/new.svg", kSvg);
    writeFile(d + "/old.png", pngBytes(8, 6));
    {
        QFile old(d + "/old.png"); // made the day before: only referred to
        REQUIRE(old.open(QIODevice::ReadWrite));
        old.setFileTime(
            QDateTime::fromMSecsSinceEpoch(now - 86'400'000), QFileDevice::FileModificationTime
        );
    }
    OutputContext ctx;
    ctx.convId     = "outputs-test";
    ctx.messageKey = "u1";
    ctx.turnStart  = (now - 60'000) * 1000;
    ctx.date       = now * 1000;
    clearOutputs(ctx.convId);

    const QString text  = "Drew " + d + "/new.svg, next to " + d + "/old.png.";
    const auto    files = outputFiles(text, ctx);
    REQUIRE(files.size() == 1);
    const File &svg = files[0];
    CHECK(svg.name == "new.svg");
    CHECK(svg.isImage()); // shown by its rendered preview
    CHECK(svg.imageWidth == 240);
    CHECK(svg.imageHeight == 160);
    CHECK(svg.urlPrivateDownload.endsWith(".svg")); // the download is the SVG
    CHECK(svg.urlPrivate.endsWith(".png"));
    CHECK(QUrl(svg.urlPrivate).toLocalFile().startsWith(outputsDir(ctx.convId)));
    CHECK(QImage(QUrl(svg.urlPrivate).toLocalFile()).width() == 480);

    // The copy stays what the agent made: the file changing or going is no matter.
    QFile::remove(d + "/new.svg");
    const auto again = outputFiles(text, ctx);
    REQUIRE(again.size() == 1);
    CHECK(again[0] == svg);

    clearOutputs(ctx.convId);
    CHECK_FALSE(QFileInfo::exists(outputsDir(ctx.convId)));
    CHECK(outputFiles(text, ctx).empty()); // gone, and not there to copy again
}

TEST_CASE("a subagent's handback is its answer", "[claude][transcript]") {
    TranscriptParser p;
    p.feed(
        prompt("Draw it", "2026-09-25T10:00:00.000Z", false) +
        toolUse(
            "h1",
            "SubagentHandback",
            {{"message", "Drew it: /tmp/x.png"}},
            "2026-09-25T10:00:05.000Z"
        ) +
        toolResult("h1", "2026-09-25T10:00:06.000Z", false)
    );
    REQUIRE(p.items().size() == 2);
    CHECK(p.items()[1].kind == Kind::AssistantText);
    CHECK(p.items()[1].text == "Drew it: /tmp/x.png");
}

TEST_CASE(
    "a session's answers carry the files it made, until it's removed", "[claude][backend][outputs]"
) {
    FakeClaudeHome home;
    home.writeSession("idle");
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    writeFile(home.dir.path() + "/work/chart.png", pngBytes(30, 20));
    home.append(
        prompt("make a chart", isoAt(now - 30'000).constData()) +
        assistantText(
            "Saved it to `" + home.dir.path() + "/work/chart.png`.", isoAt(now).constData()
        ) +
        turnEnd(isoAt(now + 1).constData())
    );
    const ConversationId conv{"S1"};
    clearOutputs(conv.value);

    claude_code::Backend backend(Credentials{});
    backend.connectRealtime();
    const auto page = collect(backend.loadHistory(conv, std::nullopt));
    REQUIRE(page.size() == 1);
    const auto &answer = page[0].messages.back();
    REQUIRE(answer.files.size() == 1);
    CHECK(answer.files[0].name == "chart.png");
    CHECK(answer.files[0].isImage());
    CHECK(answer.files[0].imageWidth == 30);
    CHECK(QFileInfo::exists(outputsDir(conv.value)));

    backend.leaveConversation(conv); // "Remove from msga"
    CHECK_FALSE(QFileInfo::exists(outputsDir(conv.value)));
}

TEST_CASE("the CLI's login status", "[claude][login]") {
    // `claude auth status` (verified 2.1.283): JSON, exit code 1 when logged out.
    CHECK(parseLoginStatus(R"({"loggedIn": true, "authMethod": "claude.ai"})", 0) == Login::In);
    CHECK(parseLoginStatus(R"({"loggedIn": true, "authMethod": "api_key"})", 0) == Login::In);
    CHECK(parseLoginStatus(R"({"loggedIn": false, "authMethod": "none"})", 1) == Login::Out);
    CHECK(parseLoginStatus("a warning first\n{\"loggedIn\": false}\n", 1) == Login::Out);
    CHECK(
        parseLoginStatus("Not logged in. Run claude auth login to authenticate.\n", 1) == Login::Out
    );
    // A CLI without `auth status` can't tell: that never blocks anything.
    CHECK(parseLoginStatus("error: unknown command 'auth'\n", 1) == Login::Unknown);
    CHECK(parseLoginStatus("", 0) == Login::Unknown);
}

TEST_CASE("a turn that failed for want of a login says how to log in", "[claude][login]") {
    // As Claude Code 2.1.283 records it.
    TranscriptParser p;
    p.feed(prompt("say hi", "2026-09-26T21:21:17.200Z"));
    p.feed(line({
        {"type", "assistant"},
        {"timestamp", "2026-09-26T21:21:17.500Z"},
        {"isApiErrorMessage", true},
        {"error", "authentication_failed"},
        {"message",
         QJsonObject{
             {"model", "<synthetic>"},
             {"content",
              QJsonArray{
                  QJsonObject{{"type", "text"}, {"text", "Not logged in · Please run /login"}}
              }},
         }},
    }));
    p.feed(turnEnd("2026-09-26T21:21:17.600Z"));
    REQUIRE(p.items().size() == 2);
    const TranscriptItem &answer = p.items()[1];
    CHECK(answer.kind == Kind::AssistantText);
    CHECK(answer.loginError);
    CHECK(p.loginFailedAt() == answer.date);
    const Message m = toMessage(answer, UserId{"me"}, UserId{"claude"});
    CHECK(m.rawText == notLoggedInMessage());
    CHECK(m.text.text.contains("/login"));

    // Any other answer is Claude's own.
    TranscriptParser ok;
    ok.feed(assistantText("hi", "2026-09-26T21:21:17.500Z"));
    REQUIRE(ok.items().size() == 1);
    CHECK_FALSE(ok.items()[0].loginError);
    CHECK(ok.loginFailedAt() == 0);
}

namespace {

// A `claude` that only answers `auth status`, as logged in or not; anything
// else is logged to calls.log (nothing may get that far when logged out).
QString fakeLoginCli(const QString &dir, bool loggedIn) {
    const QString cli = dir + "/claude";
    QFile         f(cli);
    if (!f.open(QIODevice::WriteOnly))
        return {};
    f.write(
        QByteArray("#!/bin/sh\n") +
        (loggedIn ? "[ \"$1\" = auth ] && { echo '{\"loggedIn\":true}'; exit 0; }\n"
                  : "[ \"$1\" = auth ] && { echo '{\"loggedIn\":false}'; exit 1; }\n") +
        "printf '%s\\n' \"$*\" >> \"$CLAUDE_CONFIG_DIR/calls.log\"\nexit 1\n"
    );
    f.setPermissions(f.permissions() | QFileDevice::ExeOwner);
    return cli;
}

} // namespace

TEST_CASE("no session starts while Claude Code is logged out", "[claude][login][backend]") {
    FakeClaudeHome home;
    QTemporaryDir  work;
    {
        QFile f(home.dir.path() + "/.claude.json");
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(
                    QJsonObject{
                        {"projects",
                         QJsonObject{
                             {QDir(work.path()).absolutePath(),
                              QJsonObject{{"hasTrustDialogAccepted", true}}}
                         }},
                    }
        )
                    .toJson());
    }
    const QString cli = fakeLoginCli(work.path(), false);
    REQUIRE_FALSE(cli.isEmpty());
    claude_code::Backend backend(Credentials{cli});
    QString              error;
    ConversationId       conv;
    backend.startAgentSession(
        work.path(), false, {}, [&](ConversationId id) { conv = id; }, [&](QString e) { error = e; }
    );
    REQUIRE(QTest::qWaitFor([&] { return !error.isEmpty(); }, 5000));
    CHECK(error == notLoggedInMessage());
    CHECK(conv.value.isEmpty());
    CHECK_FALSE(QFileInfo::exists(home.dir.path() + "/calls.log"));
}

TEST_CASE("adding the workspace needs the CLI and its login", "[claude][login][auth]") {
    FakeClaudeHome   home;
    QTemporaryDir    bin, fakeHome;
    // Only `bin` is searched: PATH, and the installers' folders under HOME.
    const QByteArray path = qgetenv("PATH"), realHome = qgetenv("HOME");
    qputenv("PATH", bin.path().toUtf8());
    qputenv("HOME", fakeHome.path().toUtf8());
    struct Restore {
        QByteArray path, home;
        ~Restore() {
            qputenv("PATH", path);
            qputenv("HOME", home);
        }
    } restore{path, realHome};
    if (!findClaudeExecutable().isEmpty())
        SKIP("a claude outside PATH and HOME (/usr/local/bin, /opt/homebrew/bin)");

    struct Outcome {
        std::optional<QString> failed, claudePath;
    };
    auto add = [] {
        Outcome                   o;
        claude_code::AuthStrategy s;
        QObject::connect(&s, &auth::AuthStrategy::failed, [&](QString why) { o.failed = why; });
        QObject::connect(&s, &auth::AuthStrategy::succeeded, [&](TokenStore::WorkspaceRecord r) {
            o.claudePath = fromRecord(r).claudePath;
        });
        s.start();
        QTest::qWaitFor([&] { return o.failed || o.claudePath; }, 5000);
        return o;
    };

    CHECK(add().failed == notInstalledMessage());

    REQUIRE_FALSE(fakeLoginCli(bin.path(), false).isEmpty());
    CHECK(add().failed == notLoggedInMessage());

    const QString cli = fakeLoginCli(bin.path(), true);
    REQUIRE_FALSE(cli.isEmpty());
    const Outcome added = add();
    CHECK_FALSE(added.failed);
    CHECK(added.claudePath == cli);
}
