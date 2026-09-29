// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QList>
#include <QString>

namespace Spell {

// A [start, start + length) run of one line of composer text.
struct Span {
    int start  = 0;
    int length = 0;

    int  end() const { return start + length; }
    bool operator==(const Span &o) const = default;
};

// The words of one line of composer text that a spell checker should look at,
// in order. What is never a word to check:
//   • ``` fenced code ``` (which may span lines: `inFence` carries the state
//     from the previous line in and this line's out) and `inline code`;
//   • URLs (scheme:// or www.), Slack's <…> tokens, :emoji: shortcodes;
//   • whitespace-separated chunks holding an '@' (mentions typed as text,
//     e-mail addresses), starting with '#' (channels) or '/' (slash commands,
//     paths), holding a '\' or a dot between two letters (file names, hosts);
//   • words with a digit or an inner '_' (identifiers), ALL-CAPS and camelCase
//     words (acronyms, code), single letters and anything over 64 characters;
//   • the `excluded` ranges (mention pills), read as whitespace.
// A word is letters and combining marks, with apostrophes (' and ’) kept
// between letters ("don't") and markdown markers around it dropped (*bold*,
// _it_, ~gone~).
QList<Span> checkableWords(const QString &line, bool &inFence, const QList<Span> &excluded = {});

// The word around `pos` in `line` (a cursor inside it or at either end), with
// the same letters-and-apostrophes rule; {pos, 0} when there is none.
Span wordAt(const QString &line, int pos);

} // namespace Spell
