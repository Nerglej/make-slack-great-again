// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
#include <catch2/catch_test_macros.hpp>

#include "backend/teams/json_mappers.h"
#include "text/markdown_compose.h"
#include "text/mrkdwn_parser.h"
#include "text/portable_markdown.h"

using namespace Qt::StringLiterals;

namespace {

// What a source workspace knows: U7 is Alice, C1 is #general, S1 is @eng-oncall,
// :rocket: is built in. Anything else keeps its parsed text.
QString sourceWorkspace(const TextEntity &e) {
    if (e.type == EntityType::UserMention && e.data == "U7")
        return u"@Alice"_s;
    if (e.type == EntityType::ChannelMention && e.data == "C1")
        return u"#general"_s;
    if (e.type == EntityType::UsergroupMention && e.data == "S1")
        return u"@eng-oncall"_s;
    if (e.type == EntityType::Emoji && e.data == "rocket")
        return QString::fromUtf8("🚀");
    if (e.type == EntityType::MessageLink)
        return u"#general"_s;
    return {};
}

QString portable(const QString &mrkdwn) {
    return PortableMarkdown::fromText(MrkdwnParser::parse(mrkdwn), sourceWorkspace);
}

const QString kWordJoiner = QString(QChar(0x2060));

} // namespace

TEST_CASE("portable text names people, groups and channels", "[portable_markdown]") {
    CHECK(
        portable(
            "hi <@U7> and <@U8|bob> in <#C1> and <#C9|random>, cc <!subteam^S1> <!subteam^S2|@ops>"
        ) == "hi @Alice and @bob in #general and #random, cc @eng-oncall @ops"
    );
    // Unknown and unlabeled: the parsed text, still plain.
    CHECK(portable("ping <@U9>") == "ping @U9");
}

TEST_CASE("portable text never broadcasts", "[portable_markdown]") {
    const QString out = portable("<!here> <!channel> <!everyone> heads up");
    CHECK(out == "@here @channel @everyone heads up");
    // The target composer passes <!…> tokens through; there must be none.
    CHECK_FALSE(out.contains('<'));
    CHECK(MarkdownCompose::convert(out).mrkdwn == out);
}

TEST_CASE("portable text keeps literal tokens literal", "[portable_markdown]") {
    // Typed as text in the source ("&lt;!here&gt;" on the wire): shown as text,
    // and must not become a real token once re-sent.
    const QString out = portable("say &lt;!here&gt; or &lt;@U7&gt; or &lt;#C1&gt;, 1 &lt; 2");
    CHECK(
        out == "say <" + kWordJoiner + "!here> or <" + kWordJoiner + "@U7> or <" + kWordJoiner +
                   "#C1>, 1 < 2"
    );
    const auto sent = MrkdwnParser::parse(MarkdownCompose::convert(out).mrkdwn);
    for (const auto &e : sent.entities) {
        CHECK(e.type != EntityType::HereCommand);
        CHECK(e.type != EntityType::UserMention);
        CHECK(e.type != EntityType::ChannelMention);
    }
}

TEST_CASE("portable text keeps link targets", "[portable_markdown]") {
    CHECK(portable("<https://a.example/x>") == "https://a.example/x");
    CHECK(portable("<https://a.example/x|https://a.example/x>") == "https://a.example/x");
    CHECK(portable("<https://a.example/x|a.example/x>") == "https://a.example/x");
    // Slack's shortened paste label gives way to the full URL.
    CHECK(
        portable(
            QString::fromUtf8("<https://a.example/very/long/path/page|a.example/very/…/page>")
        ) == "https://a.example/very/long/path/page"
    );
    CHECK(portable("see <https://a.example/x|the docs>") == "see [the docs](https://a.example/x)");
    CHECK(
        portable("<https://a.example/x|[draft] notes>") == "[(draft) notes](https://a.example/x)"
    );
    CHECK(portable("<mailto:ann@a.example|ann@a.example>") == "ann@a.example");
    // What a Slack target's composer makes of it.
    CHECK(
        MarkdownCompose::convert(portable("see <https://a.example/x|the docs>")).mrkdwn ==
        "see <https://a.example/x|the docs>"
    );
}

TEST_CASE("portable text writes a message link as its label", "[portable_markdown]") {
    CHECK(
        portable("<https://t.slack.com/archives/C1/p1700000000000100>") ==
        "[#general](https://t.slack.com/archives/C1/p1700000000000100)"
    );
    // No label known: the permalink itself.
    CHECK(
        PortableMarkdown::fromText(
            MrkdwnParser::parse("<https://t.slack.com/archives/C1/p1700000000000100>")
        ) == "https://t.slack.com/archives/C1/p1700000000000100"
    );
}

TEST_CASE("portable text formats in CommonMark", "[portable_markdown]") {
    CHECK(
        portable("*bold* _it_ ~gone~ `x = 1` __under__") == "**bold** _it_ ~~gone~~ `x = 1` under"
    );
    CHECK(portable("*see <#C1>*") == "**see #general**");
    CHECK(portable("*<https://a.example|docs>*") == "**[docs](https://a.example)**");
    // Round trip through a Slack target's composer lands on Slack's own marks.
    CHECK(
        MarkdownCompose::convert(portable("*bold* _it_ ~gone~ `x`")).mrkdwn ==
        "*bold* _it_ ~gone~ `x`"
    );
}

TEST_CASE("portable text fences code blocks on their own lines", "[portable_markdown]") {
    CHECK(
        portable("look:\n```\nint a = 1; // <@U7>\n```\nafter") ==
        "look:\n```\nint a = 1; // <" + kWordJoiner + "@U7>\n```\nafter"
    );
    CHECK(portable("run ```make all``` now") == "run\n```\nmake all\n```\nnow");
    CHECK(portable("```js\nx()\n```") == "```\nx()\n```");
}

TEST_CASE("portable text quotes line by line", "[portable_markdown]") {
    CHECK(
        portable("intro\n> quoted *line*\n> by <@U7>\nafter") ==
        "intro\n> quoted **line**\n> by @Alice\nafter"
    );
}

TEST_CASE("portable text turns built-in emoji into glyphs", "[portable_markdown]") {
    CHECK(portable("ship :rocket: :partyparrot:") == QString::fromUtf8("ship 🚀 :partyparrot:"));
    // No resolver: codes as parsed.
    CHECK(PortableMarkdown::fromText(MrkdwnParser::parse(":rocket:")) == ":rocket:");
}

TEST_CASE("portable text from a Teams HTML body", "[portable_markdown][teams]") {
    const auto twe = teams::JsonMappers::htmlToText(
        u"<p>Hi <b>team</b>, see <a href=\"https://x.example/doc\">the doc</a></p>"
        u"<pre><code>make all</code></pre><p>&lt;!here&gt; <i>done</i></p>"_s
    );
    const QString out = PortableMarkdown::fromText(twe, sourceWorkspace);
    CHECK(
        out == "Hi **team**, see [the doc](https://x.example/doc)\n```\nmake all\n```\n<" +
                   kWordJoiner + "!here> _done_"
    );
    CHECK_FALSE(out.contains("<p>"));
}

TEST_CASE("portable text of plain text is the text", "[portable_markdown]") {
    TextWithEntities plain{u"Hello,\nsee you at 10:30:00"_s, {}};
    CHECK(PortableMarkdown::fromText(plain) == plain.text);
}
