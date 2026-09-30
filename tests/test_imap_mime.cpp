// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Unit tests for the IMAP MIME / RFC 5322 parser (imap-backend-plan Phase 1).
#include <catch2/catch_test_macros.hpp>

#include "backend/imap/mime_parser.h"

using namespace imap;
using namespace Qt::StringLiterals;

namespace {
// Build a raw message with CRLF line endings (as IMAP delivers).
QByteArray msg(const QByteArray &s) {
    QByteArray out = s;
    out.replace("\n", "\r\n");
    return out;
}
} // namespace

// ── Encoded-words (RFC 2047) ─────────────────────────────────────────────────

TEST_CASE("decodeEncodedWords: B and Q encodings", "[imap][mime]") {
    CHECK(Mime::decodeEncodedWords("=?UTF-8?B?SGVsbG8gV29ybGQ=?=") == "Hello World");
    CHECK(Mime::decodeEncodedWords("=?UTF-8?Q?Hello_World?=") == "Hello World");
    // adjacent encoded words: the separating whitespace is dropped
    CHECK(Mime::decodeEncodedWords("=?UTF-8?B?SGVsbG8=?= =?UTF-8?B?V29ybGQ=?=") == "HelloWorld");
    // mixed plain + encoded
    CHECK(Mime::decodeEncodedWords("New skill: =?UTF-8?Q?Puzzle?=") == "New skill: Puzzle");
    // plain text passes through untouched
    CHECK(Mime::decodeEncodedWords("Access Token") == "Access Token");
}

TEST_CASE("decodeEncodedWords: non-ASCII (Cyrillic) round-trips", "[imap][mime]") {
    // "Михаил" base64 in UTF-8
    const QString got = Mime::decodeEncodedWords("=?UTF-8?B?0JzQuNGF0LDQuNC7?=");
    CHECK(got == QString::fromUtf8("\xD0\x9C\xD0\xB8\xD1\x85\xD0\xB0\xD0\xB8\xD0\xBB"));
}

// ── Transfer-encoding decoders ───────────────────────────────────────────────

TEST_CASE("quoted-printable: hex + soft line breaks", "[imap][mime]") {
    CHECK(Mime::decodeQuotedPrintable("Hello=20World") == "Hello World");
    CHECK(Mime::decodeQuotedPrintable("a=\r\nb") == "ab"); // soft break CRLF
    CHECK(Mime::decodeQuotedPrintable("a=\nb") == "ab");   // soft break LF
    CHECK(Mime::decodeQuotedPrintable("caf=C3=A9") == QByteArray("caf\xC3\xA9"));
}

TEST_CASE("base64: whitespace-tolerant", "[imap][mime]") {
    CHECK(Mime::decodeBase64("SGVsbG8=") == "Hello");
    CHECK(Mime::decodeBase64("SGVs\r\nbG8=") == "Hello");
}

// ── Address parsing (RFC 5322 common forms) ──────────────────────────────────

TEST_CASE("parseAddressList: name/quoted/bare + encoded-word names", "[imap][mime]") {
    auto a =
        Mime::parseAddressList("Kent <kent@worklingua.com>, \"Doe, John\" <john@x.com>, c@z.io");
    REQUIRE(a.size() == 3);
    CHECK(a[0].name == "Kent");
    CHECK(a[0].email == "kent@worklingua.com");
    CHECK(a[1].name == "Doe, John"); // comma inside quotes not a separator
    CHECK(a[1].email == "john@x.com");
    CHECK(a[2].name.isEmpty());
    CHECK(a[2].email == "c@z.io");

    auto b = Mime::parseAddressList("=?UTF-8?Q?Puzzle?= <p@x.com>");
    REQUIRE(b.size() == 1);
    CHECK(b[0].name == "Puzzle");
}

// ── Full messages ────────────────────────────────────────────────────────────

TEST_CASE("parse: simple text/plain message", "[imap][mime]") {
    const auto m = Mime::parse(
        msg("From: Alice <alice@example.com>\n"
            "To: Bob <bob@example.com>\n"
            "Subject: Lunch?\n"
            "Message-ID: <abc123@example.com>\n"
            "Date: Mon, 22 Jun 2026 18:37:57 +0000\n"
            "Content-Type: text/plain; charset=utf-8\n"
            "\n"
            "Are you free tomorrow?\n")
    );
    CHECK(m.subject == "Lunch?");
    CHECK(m.messageId == "abc123@example.com");
    REQUIRE(m.from.size() == 1);
    CHECK(m.from[0].email == "alice@example.com");
    CHECK(m.from[0].name == "Alice");
    CHECK(m.textPlain.trimmed() == "Are you free tomorrow?");
    CHECK(m.date.isValid());
}

TEST_CASE("parse: References + In-Reply-To threading headers", "[imap][mime]") {
    const auto m = Mime::parse(
        msg("Subject: Re: Lunch?\n"
            "In-Reply-To: <abc123@example.com>\n"
            "References: <root@x.com> <abc123@example.com>\n"
            "\n"
            "Sure!\n")
    );
    CHECK(m.inReplyTo == "abc123@example.com");
    REQUIRE(m.references.size() == 2);
    CHECK(m.references[0] == "root@x.com");
    CHECK(m.references[1] == "abc123@example.com");
}

TEST_CASE("parse: multipart/alternative keeps plain and html", "[imap][mime]") {
    const auto m = Mime::parse(
        msg("Subject: Hi\n"
            "Content-Type: multipart/alternative; boundary=\"BB\"\n"
            "\n"
            "preamble ignored\n"
            "--BB\n"
            "Content-Type: text/plain; charset=utf-8\n"
            "\n"
            "plain body\n"
            "--BB\n"
            "Content-Type: text/html; charset=utf-8\n"
            "\n"
            "<p>html body</p>\n"
            "--BB--\n")
    );
    CHECK(m.textPlain.contains("plain body"));
    CHECK(m.textHtml.contains("<p>html body</p>"));
}

TEST_CASE("parse: multipart/mixed with a base64 attachment", "[imap][mime]") {
    const auto m = Mime::parse(
        msg("Subject: report\n"
            "Content-Type: multipart/mixed; boundary=\"MM\"\n"
            "\n"
            "--MM\n"
            "Content-Type: text/plain; charset=utf-8\n"
            "\n"
            "see attached\n"
            "--MM\n"
            "Content-Type: application/pdf; name=\"r.pdf\"\n"
            "Content-Disposition: attachment; filename=\"r.pdf\"\n"
            "Content-Transfer-Encoding: base64\n"
            "\n"
            "SGVsbG8=\n"
            "--MM--\n")
    );
    CHECK(m.textPlain.contains("see attached"));
    REQUIRE(m.attachments.size() == 1);
    CHECK(m.attachments[0].filename == "r.pdf");
    CHECK(m.attachments[0].mimeType == "application/pdf");
    CHECK(m.attachments[0].content == "Hello");
}

TEST_CASE("parse: RFC 2047 encoded-word attachment filename is decoded", "[imap][mime]") {
    // Non-ASCII filenames arrive as encoded-words in the filename=/name= params,
    // exactly like subjects and address display-names. They must be decoded, not
    // shown raw ("=?utf-8?B?…?=").
    const auto m = Mime::parse(
        msg("Subject: report\n"
            "Content-Type: multipart/mixed; boundary=\"MM\"\n"
            "\n"
            "--MM\n"
            "Content-Type: application/pdf; name=\"=?utf-8?B?UHJvdG9rb2xsX8OFcnNtw7Z0ZS5wZGY=?=\"\n"
            "Content-Disposition: attachment; "
            "filename=\"=?utf-8?B?UHJvdG9rb2xsX8OFcnNtw7Z0ZS5wZGY=?=\"\n"
            "Content-Transfer-Encoding: base64\n"
            "\n"
            "SGVsbG8=\n"
            "--MM--\n")
    );
    REQUIRE(m.attachments.size() == 1);
    CHECK(m.attachments[0].filename == QString::fromUtf8("Protokoll_Årsmöte.pdf"));
}

TEST_CASE("parse: quoted-printable body decoded with charset", "[imap][mime]") {
    const auto m = Mime::parse(
        msg("Subject: =?UTF-8?Q?Caf=C3=A9?=\n"
            "Content-Type: text/plain; charset=utf-8\n"
            "Content-Transfer-Encoding: quoted-printable\n"
            "\n"
            "Caf=C3=A9 time\n")
    );
    CHECK(m.subject == QString::fromUtf8("Caf\xC3\xA9"));
    CHECK(m.textPlain.trimmed() == QString::fromUtf8("Caf\xC3\xA9 time"));
}

TEST_CASE("parse: List-Id extracted; participants dedup", "[imap][mime]") {
    const auto m = Mime::parse(
        msg("From: Sender <s@list.com>\n"
            "To: me@x.com, s@list.com\n"
            "List-Id: Dev List <dev.list.example.com>\n"
            "Subject: x\n"
            "\n"
            "body\n")
    );
    CHECK(m.listId == "dev.list.example.com");
    // participants = from ∪ to ∪ cc, deduped (s@list.com appears in both)
    const auto p = m.participants();
    REQUIRE(p.size() == 2);
}

TEST_CASE("parse: malformed (no body separator) does not crash", "[imap][mime]") {
    const auto m = Mime::parse("Subject: orphan headers only\r\n");
    CHECK(m.subject == "orphan headers only");
    CHECK(m.textPlain.isEmpty());
}

// ── Legacy charsets ──────────────────────────────────────────────────────────
// decodeText covers them through QStringDecoder when Qt has ICU and through
// iconv(3) when it doesn't (the static Linux release), so check both routes.

namespace {
struct LegacySample {
    const char *charset;
    QByteArray  bytes;
    QString     text;
};

const LegacySample kLegacySamples[] = {
    {"windows-1252", QByteArray("caf\xe9 \x80 \x93quoted\x94"), u"café € “quoted”"_s},
    {"ISO-8859-2", QByteArray("\xb3\xf3\xbf\xea\xa3\xd3\xaf\xca"), u"łóżęŁÓŻĘ"_s},
    {"KOI8-R", QByteArray("\xf0\xd2\xc9\xd7\xc5\xd4"), u"Привет"_s},
    {"Shift_JIS", QByteArray("\x82\xb1\x82\xf1\x82\xc9\x82\xbf\x82\xcd"), u"こんにちは"_s},
    {"ISO-2022-JP", QByteArray("\x1b$B$3$s$K$A$O\x1b(B!"), u"こんにちは!"_s},
    {"EUC-KR", QByteArray("\xbe\xc8\xb3\xe7"), u"안녕"_s},
    {"GB2312", QByteArray("\xc4\xe3\xba\xc3"), u"你好"_s},
    {"Big5", QByteArray("\xa7\x41\xa6\x6e"), u"你好"_s},
};
} // namespace

TEST_CASE("decodeText: legacy mail charsets", "[imap][mime]") {
    for (const auto &s : kLegacySamples) {
        INFO(s.charset);
        CHECK(Mime::decodeText(s.bytes, QString::fromLatin1(s.charset)) == s.text);
    }
}

TEST_CASE("decodeText: RFC 2047 word in a legacy charset", "[imap][mime]") {
    CHECK(
        Mime::decodeEncodedWords("=?ISO-2022-JP?B?GyRCJDMkcyRLJEEkTxsoQg==?=") == u"こんにちは"_s
    );
}

#ifdef MSGA_HAVE_ICONV
TEST_CASE("decodeWithIconv: legacy charsets and Outlook's labels", "[imap][mime]") {
    for (const auto &s : kLegacySamples) {
        INFO(s.charset);
        QString out;
        REQUIRE(Mime::decodeWithIconv(s.bytes, QString::fromLatin1(s.charset), &out));
        CHECK(out == s.text);
    }
    QString out;
    // Outlook's label for Korean mail, written in CP949.
    REQUIRE(Mime::decodeWithIconv(QByteArray("\xbe\xc8\xb3\xe7"), u"ks_c_5601-1987"_s, &out));
    CHECK(out == u"안녕"_s);
}

TEST_CASE("decodeWithIconv: bad bytes become U+FFFD, unknown charset fails", "[imap][mime]") {
    QString out;
    // 0xFF is never a Shift_JIS byte; 0x82 at the end is a cut-off lead byte.
    REQUIRE(
        Mime::decodeWithIconv(
            QByteArray(
                "a\xff"
                "b"
            ),
            u"Shift_JIS"_s,
            &out
        )
    );
    CHECK(out == u"a\uFFFDb"_s);
    REQUIRE(Mime::decodeWithIconv(QByteArray("ab\x82"), u"Shift_JIS"_s, &out));
    CHECK(out == u"ab\uFFFD"_s);
    CHECK_FALSE(Mime::decodeWithIconv("abc", u"x-no-such-charset"_s, &out));
}
#endif
