#include "support/test.h"
#include "base/str.h"

TEST("str: simplified folds Unicode whitespace like QString::simplified") {
    CHECK_STR(str::simplified("  a \t\n b\r\n\nc  "), "a b c");
    CHECK_STR(str::simplified(""), "");
    CHECK_STR(str::simplified(" \t\n "), "");
    CHECK_STR(str::simplified("one"), "one");
    // \f, \v, NBSP, the em space and the ideographic space are whitespace too.
    CHECK_STR(
        str::simplified(
            "a\f\vb\xC2\xA0\xC2\xA0"
            "c\xE2\x80\x83"
            "d\xE3\x80\x80"
        ),
        "a b c d"
    );
    // Non-space multi-byte text passes through untouched.
    CHECK_STR(
        str::simplified(" r\xC3\xA4ksm\xC3\xB6rg\xC3\xA5s  \xF0\x9F\x8D\xA4 "),
        "r\xC3\xA4ksm\xC3\xB6rg\xC3\xA5s \xF0\x9F\x8D\xA4"
    );
}

TEST("str: decodeEntities named and numeric references") {
    CHECK_STR(
        str::decodeEntities("a &amp; b &lt;c&gt; &quot;d&quot; &apos;e&apos;"),
        "a & b <c> \"d\" 'e'"
    );
    CHECK_STR(str::decodeEntities("&AMP; &Lt; &NBSP;"), "& < \xC2\xA0");
    CHECK_STR(str::decodeEntities("&#65;&#x42;&#X43;&#x1F600;"), "ABC\xF0\x9F\x98\x80");
    // nbspAsSpace turns only the references into spaces, not a literal U+00A0.
    CHECK_STR(str::decodeEntities("a&nbsp;b&#160;c\xC2\xA0", true), "a b c\xC2\xA0");
    CHECK_STR(
        str::decodeEntities("a&nbsp;b", false),
        "a\xC2\xA0"
        "b"
    );
}

TEST("str: decodeEntities leaves malformed references literal") {
    CHECK_STR(str::decodeEntities("no refs here"), "no refs here");
    CHECK_STR(str::decodeEntities("AT&T"), "AT&T");
    CHECK_STR(str::decodeEntities("a & b; c"), "a & b; c");
    CHECK_STR(str::decodeEntities("&unknown;"), "&unknown;");
    CHECK_STR(str::decodeEntities("&#0; &#; &#x;"), "&#0; &#; &#x;");
    CHECK_STR(str::decodeEntities("&verylongname;"), "&verylongname;"); // ';' too far away
    CHECK_STR(str::decodeEntities("&amp"), "&amp");
    CHECK_STR(str::decodeEntities("&&amp;"), "&&");
    // Out of range or surrogate numbers decode as U+FFFD, never as invalid UTF-8.
    CHECK_STR(str::decodeEntities("&#xD800;"), "\xEF\xBF\xBD");
    CHECK_STR(str::decodeEntities("&#9999999;"), "\xEF\xBF\xBD");
    // Digits stop at the first non-digit, as the old readers did.
    CHECK_STR(str::decodeEntities("&#65x;"), "A");
}

TEST("str: byteSize styles and rounding") {
    using str::ByteSize;
    // Whole: everything floored (the old composer chip and Settings).
    CHECK_STR(str::byteSize(0), "0 B");
    CHECK_STR(str::byteSize(1023), "1023 B");
    CHECK_STR(str::byteSize(1024), "1 KB");
    CHECK_STR(str::byteSize(2047), "1 KB");
    CHECK_STR(str::byteSize(1024 * 1024 - 1), "1023 KB");
    CHECK_STR(str::byteSize(1024 * 1024), "1 MB");
    CHECK_STR(str::byteSize(int64_t(1024) * 1024 * 1024 * 3 / 2), "1536 MB");

    // File: "" when unknown, floored KB, one MB decimal below 10.
    CHECK_STR(str::byteSize(0, ByteSize::File), "");
    CHECK_STR(str::byteSize(-5, ByteSize::File), "");
    CHECK_STR(str::byteSize(512, ByteSize::File), "512 B");
    CHECK_STR(str::byteSize(81 * 1024 - 1, ByteSize::File), "80 KB");
    CHECK_STR(str::byteSize(int64_t(1.45 * 1024 * 1024), ByteSize::File), "1.4 MB");
    CHECK_STR(str::byteSize(int64_t(9.94 * 1024 * 1024), ByteSize::File), "9.9 MB");
    CHECK_STR(str::byteSize(int64_t(10.6 * 1024 * 1024), ByteSize::File), "11 MB");
    CHECK_STR(str::byteSize(int64_t(1024) * 1024 * 1024 * 3, ByteSize::File), "3072 MB");

    // Exact: rounded KB, one decimal MB and GB (the file browser).
    CHECK_STR(str::byteSize(812, ByteSize::Exact), "812 B");
    CHECK_STR(str::byteSize(1535, ByteSize::Exact), "1 KB");
    CHECK_STR(str::byteSize(1536, ByteSize::Exact), "2 KB");
    CHECK_STR(str::byteSize(int64_t(12.5 * 1024 * 1024), ByteSize::Exact), "12.5 MB");
    CHECK_STR(str::byteSize(int64_t(1024) * 1024 * 1024 * 2, ByteSize::Exact), "2.0 GB");
}

TEST("str: trim and case") {
    CHECK(str::trim(" \t a b \r\n") == "a b");
    CHECK(str::trim("   ").empty());
    CHECK_STR(str::asciiLower("AbC\xC3\x84"), "abc\xC3\x84");
}
