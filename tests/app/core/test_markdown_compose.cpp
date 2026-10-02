// Composer text → mrkdwn + rich_text (mrkdwn::compose): ported from msga's
// tests/test_markdown_compose.cpp, the list / fence / quote block cases and
// richTextElements. Keys come out in the writer's order (type first), not
// sorted as Qt's JSON wrote them.
#include "app/mrkdwn/markdown.h"
#include "base/json.h"
#include "support/test.h"

namespace {

// The rich_text block's elements, written back as compact JSON; "" when no
// block was built.
std::string elementsJson(const mrkdwn::Composed &c) {
    if (c.blocks.empty())
        return {};
    json::Document d;
    if (!d.parse(std::string(c.blocks), nullptr))
        return "parse error";
    return json::write(d.root()[size_t(0)]["elements"]);
}

json::Document parsed(const mrkdwn::Composed &c) {
    json::Document d;
    d.parse(std::string(c.blocks), nullptr);
    return d;
}

} // namespace

TEST("compose: a bulleted list becomes a rich_text_list with a bullet fallback") {
    const auto c = mrkdwn::compose("- one\n- **two**\n* three");
    CHECK_STR(c.mrkdwn, "\xE2\x80\xA2 one\n\xE2\x80\xA2 *two*\n\xE2\x80\xA2 three");
    const json::Document d = parsed(c);
    REQUIRE(d.root().size() == 1);
    CHECK(d.root()[size_t(0)]["type"].str() == "rich_text");
    const json::Value list = d.root()[size_t(0)]["elements"][size_t(0)];
    CHECK(list["type"].str() == "rich_text_list");
    CHECK(list["style"].str() == "bullet");
    CHECK(!list.has("indent"));
    REQUIRE(list["elements"].size() == 3);
    CHECK(list["elements"][size_t(0)]["type"].str() == "rich_text_section");
    CHECK_STR(
        json::write(list["elements"][size_t(1)]["elements"]),
        R"([{"type":"text","text":"two","style":{"bold":true}}])"
    );
}

TEST("compose: an ordered list renumbers its fallback and carries the start as offset") {
    const auto c = mrkdwn::compose("3. a\n7. b");
    CHECK_STR(c.mrkdwn, "3. a\n4. b");
    const json::Document d    = parsed(c);
    const json::Value    list = d.root()[size_t(0)]["elements"][size_t(0)];
    CHECK(list["style"].str() == "ordered");
    CHECK(list["offset"].integer() == 2);

    const auto           from1 = mrkdwn::compose("1. a\n2. b\n3) c");
    const json::Document d1    = parsed(from1);
    CHECK_STR(from1.mrkdwn, "1. a\n2. b\n3. c");
    CHECK(!d1.root()[size_t(0)]["elements"][size_t(0)].has("offset"));
}

TEST("compose: nested items become sibling lists with an indent") {
    // Two-space and four-space nesting rank the same.
    for (const char *in : {"- a\n  - b\n  - c\n- d", "- a\n    - b\n    - c\n- d"}) {
        const auto           c   = mrkdwn::compose(in);
        const json::Document d   = parsed(c);
        const json::Value    els = d.root()[size_t(0)]["elements"];
        REQUIRE(els.size() == 3);
        CHECK(els[size_t(0)]["elements"].size() == 1);
        CHECK(els[size_t(1)]["indent"].integer() == 1);
        CHECK(els[size_t(1)]["elements"].size() == 2);
        CHECK(!els[size_t(2)].has("indent"));
        CHECK_STR(
            c.mrkdwn, "\xE2\x80\xA2 a\n    \xE2\x80\xA2 b\n    \xE2\x80\xA2 c\n\xE2\x80\xA2 d"
        );
    }
    // A style change at the same level also starts a new list element.
    const json::Document d = parsed(mrkdwn::compose("- a\n1. b"));
    CHECK(d.root()[size_t(0)]["elements"].size() == 2);
}

TEST("compose: text around a list lands in sections and the blank lines survive") {
    const auto c = mrkdwn::compose("intro **x**\n\n- a\n- b\n\noutro\n\nlast");
    CHECK_STR(c.mrkdwn, "intro *x*\n\n\xE2\x80\xA2 a\n\xE2\x80\xA2 b\n\noutro\n\nlast");
    const json::Document d   = parsed(c);
    const json::Value    els = d.root()[size_t(0)]["elements"];
    REQUIRE(els.size() == 3);
    CHECK(els[size_t(0)]["type"].str() == "rich_text_section");
    CHECK(els[size_t(1)]["type"].str() == "rich_text_list");
    // One section per paragraph run; the blank line inside it is kept as text.
    CHECK_STR(
        json::write(els[size_t(2)]["elements"]), R"([{"type":"text","text":"outro\n\nlast"}])"
    );
}

TEST("compose: a list ends at an unindented line, continues past indented ones and blanks") {
    const auto c = mrkdwn::compose("- a\n  more a\n- b\n\n- c\nafter");
    CHECK_STR(c.mrkdwn, "\xE2\x80\xA2 a\nmore a\n\xE2\x80\xA2 b\n\xE2\x80\xA2 c\nafter");
    const json::Document d   = parsed(c);
    const json::Value    els = d.root()[size_t(0)]["elements"];
    REQUIRE(els.size() == 2);
    const json::Value items = els[size_t(0)]["elements"];
    REQUIRE(items.size() == 3);
    CHECK_STR(json::write(items[size_t(0)]["elements"]), R"([{"type":"text","text":"a\nmore a"}])");
}

TEST("compose: what is not a list marker") {
    for (const char *s : {"-5 degrees", "-item", "1.5 hours", "**bold** start", "- ", "a - b"})
        CHECK(mrkdwn::compose(s).blocks.empty());
}

TEST("compose: list items resolve Slack tokens and emoji into typed elements") {
    const auto c = mrkdwn::compose(
        "- hi <@U1> in <#C1|general> <!here> :+1::skin-tone-3: "
        "[d](https://x.io) <!subteam^S1|@team> <!everyone>"
    );
    const json::Document d     = parsed(c);
    const json::Value    items = d.root()[size_t(0)]["elements"][size_t(0)]["elements"];
    REQUIRE(items.size() == 1);
    CHECK_STR(
        json::write(items[size_t(0)]["elements"]),
        R"([{"type":"text","text":"hi "},{"type":"user","user_id":"U1"},{"type":"text","text":" in "},)"
        R"({"type":"channel","channel_id":"C1"},{"type":"text","text":" "},{"type":"broadcast","range":"here"},)"
        R"({"type":"text","text":" "},{"type":"emoji","name":"+1","skin_tone":3},{"type":"text","text":" "},)"
        R"({"type":"link","text":"d","url":"https://x.io"},{"type":"text","text":" "},)"
        R"({"type":"usergroup","usergroup_id":"S1"},{"type":"text","text":" "},{"type":"broadcast","range":"everyone"}])"
    );
}

TEST("compose: fences and quotes take their block shapes when a list is present") {
    const auto c = mrkdwn::compose("- a\n```py\nprint(1)\n```\n> **q**\n> &lt;two&gt;");
    CHECK_STR(
        elementsJson(c),
        R"([{"type":"rich_text_list","style":"bullet","elements":[{"type":"rich_text_section","elements":[{"type":"text","text":"a"}]}]},)"
        R"j({"type":"rich_text_preformatted","elements":[{"type":"text","text":"print(1)"}]},)j"
        R"({"type":"rich_text_quote","elements":[{"type":"text","text":"q","style":{"bold":true}},{"type":"text","text":"\n<two>"}]}])"
    );
    CHECK_STR(c.mrkdwn, "\xE2\x80\xA2 a\n```\nprint(1)\n```\n> *q*\n> &lt;two&gt;");
}

TEST("compose: without a list no block is built, even for code or quotes") {
    CHECK(mrkdwn::compose("> q\n```\nc\n```\n**b**").blocks.empty());
    CHECK(mrkdwn::compose("").mrkdwn.empty());
    CHECK(mrkdwn::compose("").blocks.empty());
    CHECK_STR(mrkdwn::compose("a\r\nb").mrkdwn, "a\nb");
}

TEST("compose: nested marks compose their styles and merge equal runs") {
    CHECK_STR(
        mrkdwn::richTextElements("*a _b_ c* d"),
        R"({"elements":[{"type":"text","text":"a ","style":{"bold":true}},{"type":"text","text":"b","style":{"bold":true,"italic":true}},)"
        R"({"type":"text","text":" c","style":{"bold":true}},{"type":"text","text":" d"}]})"
    );
    CHECK_STR(
        mrkdwn::richTextElements("`x` ~y~"),
        R"({"elements":[{"type":"text","text":"x","style":{"code":true}},{"type":"text","text":" "},{"type":"text","text":"y","style":{"strike":true}}]})"
    );
}

TEST("compose: a styled mention keeps bold; a code span keeps tokens literal") {
    CHECK_STR(
        mrkdwn::richTextElements("*<@U1>* `<https://x.io|l>`"),
        R"({"elements":[{"type":"user","user_id":"U1","style":{"bold":true}},{"type":"text","text":" "},)"
        R"({"type":"text","text":"<https://x.io|l>","style":{"code":true}}]})"
    );
    CHECK_STR(
        mrkdwn::richTextElements("`<@U1>`"),
        R"({"elements":[{"type":"text","text":"<@U1>","style":{"code":true}}]})"
    );
}

TEST("compose: a bare <word> is given back as text, a bare URL as a link") {
    CHECK_STR(
        mrkdwn::richTextElements("<word>"), R"({"elements":[{"type":"text","text":"<word>"}]})"
    );
    CHECK_STR(
        mrkdwn::richTextElements("<https://x.io>"),
        R"({"elements":[{"type":"link","url":"https://x.io"}]})"
    );
    CHECK_STR(mrkdwn::richTextElements("__u__"), R"({"elements":[{"type":"text","text":"u"}]})");
}
