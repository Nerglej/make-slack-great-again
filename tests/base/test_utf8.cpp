#include "support/test.h"
#include "base/utf8.h"

TEST("utf8: decode and append round trip") {
    const std::string s = "a\xC3\xA5\xE2\x82\xAC\xF0\x9F\x9A\x80"; // a å € 🚀
    size_t            i = 0;
    CHECK(utf8::decode(s, i) == 'a');
    CHECK(utf8::decode(s, i) == 0xE5);
    CHECK(utf8::decode(s, i) == 0x20AC);
    CHECK(utf8::decode(s, i) == 0x1F680);
    CHECK(i == s.size());
    std::string out;
    for (uint32_t cp : {0x61u, 0xE5u, 0x20ACu, 0x1F680u})
        utf8::append(out, cp);
    CHECK(out == s);
    CHECK(utf8::countCodePoints(s) == 4);
}

TEST("utf8: invalid sequences decode as U+FFFD one byte at a time") {
    struct {
        const char *bytes;
        size_t      len;
    } bad[] = {
        {"\x80", 1},             // stray continuation
        {"\xC0\xAF", 2},         // overlong '/'
        {"\xE0\x80\xAF", 3},     // overlong
        {"\xED\xA0\x80", 3},     // UTF-16 surrogate
        {"\xF4\x90\x80\x80", 4}, // above U+10FFFF
        {"\xE2\x82", 2},         // truncated
        {"\xFF", 1},
    };
    for (const auto &b : bad) {
        const std::string_view s(b.bytes, b.len);
        CHECK_FALSE(utf8::isValid(s));
        size_t i = 0;
        CHECK(utf8::decode(s, i) == utf8::kReplacement);
        CHECK(i == 1);
    }
    CHECK(utf8::isValid("plain \xEF\xBF\xBD literal replacement is fine"));
    const char kBad[] = {'a', char(0xFF), 'b'};
    CHECK_STR(
        utf8::sanitize(std::string_view(kBad, 3)),
        "a\xEF\xBF\xBD"
        "b"
    );
    CHECK(utf8::sanitize("ok") == "ok");
    std::string out;
    utf8::append(out, 0xD800);
    CHECK(out == "\xEF\xBF\xBD");
}

TEST("utf8: boundaries never split a sequence") {
    const std::string s = "x\u20ACy"; // x € y
    CHECK(utf8::nextBoundary(s, 0) == 1);
    CHECK(utf8::nextBoundary(s, 1) == 4);
    CHECK(utf8::prevBoundary(s, 4) == 1);
    CHECK(utf8::prevBoundary(s, 1) == 0);
    CHECK(utf8::prevBoundary(s, 5) == 4);
    CHECK(utf8::truncateAt(s, 3) == 1);
    CHECK(utf8::truncateAt(s, 4) == 4);
    CHECK(utf8::truncateAt(s, 99) == s.size());
}

TEST("utf8: case folding for search") {
    CHECK_STR(utf8::foldCase("Hello ÅÄÖ Straße ΣΑΣ Привет"), "hello åäö straße σασ привет");
    CHECK(utf8::containsFolded("Palette v3 is out", "PALETTE"));
    CHECK(utf8::containsFolded("Lena Sørensen", "SØR"));
    CHECK(utf8::containsFolded("Tomás Ferreira", "TOMÁS"));
    CHECK_FALSE(utf8::containsFolded("abc", "abcd"));
    CHECK_FALSE(utf8::containsFolded("", "a"));
    CHECK(utf8::containsFolded("anything", ""));
    CHECK(utf8::containsFolded("xxab", "AB"));
    CHECK(utf8::foldCase(0x178) == 0xFF);
    CHECK(utf8::foldCase(0x141) == 0x142); // Ł → ł
}

TEST("utf8: word characters") {
    CHECK(utf8::isWordChar('a'));
    CHECK(utf8::isWordChar('7'));
    CHECK_FALSE(utf8::isWordChar('_'));
    CHECK_FALSE(utf8::isWordChar('.'));
    CHECK(utf8::isWordChar(0xE4));   // ä
    CHECK(utf8::isWordChar(0x4E2D)); // 中
    CHECK_FALSE(utf8::isWordChar(0x1F680));
    CHECK_FALSE(utf8::isWordChar(0x2014)); // em dash
    CHECK(utf8::isSpace(0xA0));
    CHECK_FALSE(utf8::isSpace('x'));
}
