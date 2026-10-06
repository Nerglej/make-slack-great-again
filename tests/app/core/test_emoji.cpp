#include "app/mrkdwn/emoji.h"
#include "base/json.h"
#include "base/file.h"
#include "base/utf8.h"
#include "support/test.h"

TEST("emoji: shortcodes resolve") {
    CHECK_STR(emoji::toUnicode("rocket"), "🚀");
    CHECK_STR(emoji::toUnicode("+1"), "👍");
    CHECK_STR(emoji::toUnicode("palm_tree"), "🌴");
    CHECK_STR(emoji::toUnicode("airplane"), "✈️");
    CHECK(emoji::toUnicode("no_such_emoji").empty());
    CHECK(emoji::toUnicode("").empty());
    CHECK(emoji::isKnown("tada"));
    CHECK_FALSE(emoji::isKnown("tad"));
}

TEST("emoji: every entry of the source table round-trips through the compact table") {
    std::string src;
    REQUIRE(file::readAll(MSGA_SOURCE_DIR "/scripts/emoji_table.json", &src));
    json::Document d;
    REQUIRE(d.parse(std::move(src), nullptr));
    int n = 0;
    for (json::Value e : d.root()) {
        if (!CHECK_STR(emoji::toUnicode(e.key()), e.str()))
            break;
        ++n;
    }
    CHECK(n == emoji::count());
}

TEST("emoji: skin tones") {
    CHECK_STR(emoji::toUnicode("+1::skin-tone-3"), "👍🏼");
    CHECK_STR(emoji::toUnicode("wave::skin-tone-6"), "👋🏿");
    // VS16 after the base is replaced by the modifier.
    CHECK_STR(emoji::applySkinTone("\xE2\x98\x9D\xEF\xB8\x8F", 4), "\xE2\x98\x9D\xF0\x9F\x8F\xBD");
    // ZWJ sequence: the modifier follows the first code point.
    CHECK_STR(
        emoji::applySkinTone("\xF0\x9F\x99\x8B\xE2\x80\x8D\xE2\x99\x82\xEF\xB8\x8F", 2),
        "\xF0\x9F\x99\x8B\xF0\x9F\x8F\xBB\xE2\x80\x8D\xE2\x99\x82\xEF\xB8\x8F"
    );
    CHECK(emoji::toUnicode("nope::skin-tone-2").empty());
}

TEST("emoji: expand, complete, iterate") {
    CHECK_STR(
        emoji::expandShortcodes("Hello :palm_tree: world :nope: 14:30"),
        "Hello 🌴 world :nope: 14:30"
    );
    std::vector<std::string> out;
    emoji::complete("thu", 10, out);
    REQUIRE(out.size() >= 2);
    CHECK_STR(out[0], "thumbsdown");
    CHECK_STR(out[1], "thumbsup");
    out.clear();
    emoji::complete("zzzz", 5, out);
    CHECK(out.empty());
    out.clear();
    emoji::complete("", 3, out);
    CHECK(out.size() == 3);
    int         seen = 0;
    std::string prevOwned;
    bool        sorted = true;
    emoji::forEach([&](std::string_view name, const std::string &u) {
        if (seen && !(prevOwned < name))
            sorted = false;
        prevOwned = std::string(name);
        ++seen;
        return !u.empty();
    });
    CHECK(sorted);
    CHECK(seen == emoji::count());
    // Names only, the same ones, already case-folded (the picker's search
    // matches against them as they are).
    int  named  = 0;
    bool folded = true;
    emoji::forEachName([&](std::string_view name) {
        folded = folded && utf8::foldCase(name) == name;
        ++named;
        return true;
    });
    CHECK(folded);
    CHECK(named == emoji::count());
}
