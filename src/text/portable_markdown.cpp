// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "portable_markdown.h"
#include "text/link_labels.h"
#include "util/slack_links.h"

#include <algorithm>

using namespace Qt::StringLiterals;

namespace PortableMarkdown {
namespace {

constexpr QChar kWordJoiner(0x2060);

// "<@", "<#" and "<!" open a Slack token wherever the text lands (the composer
// passes tokens through untouched, and Slack reads them server-side). A word
// joiner after the '<' is invisible and keeps the text what it was: text.
QString inert(QString s) {
    for (qsizetype i = s.indexOf('<'); i >= 0 && i + 1 < s.size(); i = s.indexOf('<', i + 1)) {
        const QChar next = s[i + 1];
        if (next == '@' || next == '#' || next == '!')
            s.insert(++i, kWordJoiner);
    }
    return s;
}

// `mark` around every line of `s`, whitespace kept outside: CommonMark (and
// MarkdownCompose) only close a delimiter that follows non-space text, and no
// inline mark spans a line break.
QString wrapLines(const QString &s, const QString &mark) {
    QStringList lines = s.split('\n');
    for (auto &line : lines) {
        qsizetype a = 0, b = line.size();
        while (a < b && line[a].isSpace())
            ++a;
        while (b > a && line[b - 1].isSpace())
            --b;
        if (a < b)
            line = line.left(a) + mark + line.mid(a, b - a) + mark + line.mid(b);
    }
    return lines.join('\n');
}

// Something MarkdownCompose's [label](url) pattern can carry.
bool fitsLinkSyntax(const QString &url) {
    int depth = 0;
    for (const QChar c : url) {
        if (c.isSpace() || c == '<' || c == '>')
            return false;
        if (c == '(' && ++depth > 1)
            return false;
        if (c == ')' && --depth < 0)
            return false;
    }
    return depth == 0;
}

QString markdownLink(QString label, const QString &url) {
    label.replace('[', '(').replace(']', ')').replace('\n', ' ');
    label = label.trimmed();
    if (label.isEmpty())
        return url;
    if (!fitsLinkSyntax(url))
        return label + u" ("_s + url + u")"_s;
    return u"["_s + label + u"]("_s + url + u")"_s;
}

bool isBlock(EntityType t) {
    return t == EntityType::Pre || t == EntityType::Blockquote;
}

class Writer {
public:
    Writer(const TextWithEntities &twe, const LeafText &leafText)
        : _text(twe.text), _leafText(leafText) {
        for (auto e : twe.entities) {
            if (e.offset < 0 || e.offset >= _text.size() || e.length <= 0)
                continue;
            e.length = std::min<int>(e.length, int(_text.size()) - e.offset);
            _ents.push_back(std::move(e));
        }
        // Parents first; of two equal spans the block is the parent (Teams'
        // <pre><code> closes the code first, so it arrives first).
        std::stable_sort(_ents.begin(), _ents.end(), [](const TextEntity &a, const TextEntity &b) {
            if (a.offset != b.offset || a.length != b.length)
                return TextEntityNestingOrder{}(a, b);
            return isBlock(a.type) && !isBlock(b.type);
        });
    }

    QString write() {
        QString out = range(0, int(_text.size()));
        while (!out.isEmpty() && out.back().isSpace())
            out.chop(1);
        return out;
    }

private:
    // [from, to) of the text, consuming the entities that start inside it.
    QString range(int from, int to) {
        QString out;
        bool    afterBlock = false;
        // The line break around a block replaces the spaces that sat there.
        auto    append     = [&](QString s) {
            if (afterBlock) {
                while (!s.isEmpty() && (s[0] == ' ' || s[0] == '\t'))
                    s.remove(0, 1);
                if (s.isEmpty())
                    return;
                if (!s.startsWith('\n'))
                    out += '\n';
            }
            afterBlock = false;
            out += s;
        };
        int pos = from;
        while (_next < _ents.size() && _ents[_next].offset < to) {
            const TextEntity e = _ents[_next++];
            if (e.offset < pos)
                continue; // straddles a sibling already written
            const int end = std::min(e.offset + e.length, to);
            append(inert(_text.mid(pos, e.offset - pos)));
            const QString s = entity(e, end);
            // A fence or a quote starts on a line of its own.
            if (isBlock(e.type)) {
                while (out.endsWith(' ') || out.endsWith('\t'))
                    out.chop(1);
                if (!out.isEmpty() && !out.endsWith('\n'))
                    out += '\n';
                afterBlock = false;
            }
            append(s);
            afterBlock = isBlock(e.type);
            pos        = end;
        }
        append(inert(_text.mid(pos, to - pos)));
        return out;
    }

    // Entities nested in a span written from its text alone are dropped.
    void skipInside(int end) {
        while (_next < _ents.size() && _ents[_next].offset < end)
            ++_next;
    }

    QString leaf(const TextEntity &e, int end) {
        skipInside(end);
        QString s = _leafText ? _leafText(e) : QString();
        return s.isNull() ? _text.mid(e.offset, end - e.offset) : s;
    }

    QString entity(const TextEntity &e, int end) {
        const QString span = _text.mid(e.offset, end - e.offset);
        switch (e.type) {
        case EntityType::Bold:
            return wrapLines(range(e.offset, end), u"**"_s);
        case EntityType::Italic:
            return wrapLines(range(e.offset, end), u"_"_s);
        case EntityType::Strike:
            return wrapLines(range(e.offset, end), u"~~"_s);
        case EntityType::Underline:
            // Neither CommonMark nor mrkdwn has one.
            return range(e.offset, end);
        case EntityType::Code:
            skipInside(end);
            return span.contains('`') ? inert(span) : wrapLines(inert(span), u"`"_s);
        case EntityType::Pre: {
            skipInside(end);
            QString code = span;
            if (code.startsWith('\n'))
                code.remove(0, 1);
            if (code.endsWith('\n'))
                code.chop(1);
            return u"```\n"_s + inert(code) + u"\n```"_s;
        }
        case EntityType::Blockquote: {
            QString inner = range(e.offset, end);
            while (inner.endsWith('\n'))
                inner.chop(1);
            QStringList lines = inner.split('\n');
            for (auto &line : lines)
                line = line.isEmpty() ? u">"_s : (u"> "_s + line);
            return lines.join('\n');
        }
        case EntityType::Link: {
            if (e.data.isEmpty())
                return range(e.offset, end);
            // A mail link labeled with its address reads as the address, which
            // every target links again by itself.
            if (e.data.startsWith(u"mailto:"_s, Qt::CaseInsensitive) &&
                span.trimmed() == e.data.mid(7)) {
                skipInside(end);
                return inert(span.trimmed());
            }
            if (LinkLabels::isUrlLabel(span, e.data)) {
                skipInside(end);
                return e.data;
            }
            return markdownLink(range(e.offset, end), e.data);
        }
        case EntityType::MessageLink: {
            const auto    ref  = SlackLinks::refFromToken(e.data);
            const QString url  = ref.isValid() ? SlackLinks::messagePermalink(ref) : span;
            const QString text = leaf(e, end);
            return text == url ? url : markdownLink(text, url);
        }
        case EntityType::UserMention:
        case EntityType::UsergroupMention:
        case EntityType::HereCommand:
        case EntityType::ChannelCommand: {
            // Plain "@name" words: nobody on the target is mentioned or pinged.
            const QString s = inert(leaf(e, end));
            return s.startsWith('@') ? s : (u"@"_s + s);
        }
        case EntityType::ChannelMention:
        case EntityType::Emoji:
            return inert(leaf(e, end));
        }
        return range(e.offset, end);
    }

    const QString          &_text;
    const LeafText         &_leafText;
    std::vector<TextEntity> _ents;
    size_t                  _next = 0;
};

} // namespace

QString fromText(const TextWithEntities &twe, const LeafText &leafText) {
    return Writer(twe, leafText).write();
}

} // namespace PortableMarkdown
