// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "spell/spell_words.h"

#include <QRegularExpression>

namespace Spell {
namespace {

constexpr int kMaxWordLength = 64;

bool isApostrophe(QChar c) {
    return c == u'\'' || c == QChar(0x2019);
}

bool isWordChar(QChar c) {
    return c.isLetterOrNumber() || c.isMark() || c == u'_';
}

// An apostrophe at `i` that joins two letters ("don't"), not a quote mark.
bool isInnerApostrophe(const QString &s, int i) {
    return isApostrophe(s[i]) && i > 0 && i + 1 < s.size() && s[i - 1].isLetter() &&
           s[i + 1].isLetter();
}

void blank(QString &s, int from, int to) {
    for (int i = std::max(0, from); i < std::min(to, int(s.size())); ++i)
        s[i] = u' ';
}

void blankMatches(QString &s, const QRegularExpression &re) {
    auto it = re.globalMatch(s);
    while (it.hasNext()) {
        const auto m = it.next();
        blank(s, m.capturedStart(), m.capturedEnd());
    }
}

// ``` fences (their markers included, open across lines via inFence) and
// `inline code` outside them. A lone backtick with no partner on the line is
// literal text, as Slack renders it.
void blankCode(QString &s, bool &inFence) {
    const int n          = s.size();
    int       fenceStart = inFence ? 0 : -1;
    int       i          = 0;
    while (i < n) {
        if (i + 2 < n && s[i] == u'`' && s[i + 1] == u'`' && s[i + 2] == u'`') {
            if (inFence)
                blank(s, fenceStart, i + 3);
            else
                fenceStart = i;
            inFence = !inFence;
            i += 3;
            continue;
        }
        if (!inFence && s[i] == u'`') {
            const int close = s.indexOf(u'`', i + 1);
            if (close > i) {
                blank(s, i, close + 1);
                i = close + 1;
                continue;
            }
        }
        ++i;
    }
    if (inFence)
        blank(s, fenceStart, n);
}

// A whitespace-separated chunk that is a mention, address, channel, command,
// path or host name as a whole — none of its words are prose.
bool isSkippedChunk(const QString &s, int from, int to) {
    if (s[from] == u'#' || s[from] == u'/')
        return true;
    for (int i = from; i < to; ++i) {
        if (s[i] == u'@' || s[i] == u'\\')
            return true;
        if (s[i] == u'.' && i > from && i + 1 < to && s[i - 1].isLetter() && s[i + 1].isLetter())
            return true;
    }
    return false;
}

bool isCheckableWord(const QString &s, int from, int to) {
    const int len = to - from;
    if (len < 2 || len > kMaxWordLength)
        return false;
    int  letters = 0, uppers = 0;
    bool innerUpper = false;
    for (int i = from; i < to; ++i) {
        const QChar c = s[i];
        if (c.isNumber() || c == u'_')
            return false;
        if (!c.isLetter())
            continue;
        ++letters;
        if (c.isUpper()) {
            ++uppers;
            if (i > from)
                innerUpper = true;
        }
    }
    if (letters >= 2 && uppers == letters)
        return false;   // ALL CAPS: an acronym
    return !innerUpper; // camelCase / iPhone-style names
}

} // namespace

QList<Span> checkableWords(const QString &line, bool &inFence, const QList<Span> &excluded) {
    static const QRegularExpression kUrl(
        QStringLiteral("(?:\\b[A-Za-z][A-Za-z0-9+.\\-]*://|\\bwww\\.)\\S*")
    );
    static const QRegularExpression kToken(QStringLiteral("<[^<>\\s][^<>]*>"));
    static const QRegularExpression kEmoji(QStringLiteral(":[A-Za-z0-9_+'\\-]+:"));

    QString s = line;
    for (const Span &x : excluded)
        blank(s, x.start, x.end());
    blankCode(s, inFence);
    blankMatches(s, kUrl);
    blankMatches(s, kToken);
    blankMatches(s, kEmoji);

    QList<Span> words;
    const int   n = s.size();
    int         i = 0;
    while (i < n) {
        while (i < n && s[i].isSpace())
            ++i;
        const int chunkStart = i;
        while (i < n && !s[i].isSpace())
            ++i;
        const int chunkEnd = i;
        if (chunkStart == chunkEnd || isSkippedChunk(s, chunkStart, chunkEnd))
            continue;
        int j = chunkStart;
        while (j < chunkEnd) {
            while (j < chunkEnd && !isWordChar(s[j]))
                ++j;
            int from = j;
            while (j < chunkEnd && (isWordChar(s[j]) || isInnerApostrophe(s, j)))
                ++j;
            int to = j;
            // _italic_ markers belong to the markup, not the word.
            while (from < to && s[from] == u'_')
                ++from;
            while (to > from && s[to - 1] == u'_')
                --to;
            if (isCheckableWord(s, from, to))
                words.append({from, to - from});
        }
    }
    return words;
}

Span wordAt(const QString &line, int pos) {
    const auto inWord = [&](int i) {
        return i >= 0 && i < line.size() && (isWordChar(line[i]) || isInnerApostrophe(line, i));
    };
    int from = pos, to = pos;
    while (inWord(from - 1))
        --from;
    while (inWord(to))
        ++to;
    return {from, to - from};
}

} // namespace Spell
