#include "app/claude/render.h"

#include "app/claude/cli.h"
#include "app/claude/common.h"
#include "app/claude/transcript.h"
#include "app/mrkdwn/markdown.h"
#include "app/model/image_size.h"
#include "base/file.h"
#include "base/i18n.h"
#include "base/str.h"

#include <algorithm>
#include <vector>

namespace claude {

using i18n::tr;

namespace {

std::vector<std::string_view> splitLines(std::string_view s) {
    std::vector<std::string_view> out;
    size_t                        i = 0;
    for (;;) {
        const size_t j = s.find('\n', i);
        out.push_back(s.substr(i, j == std::string_view::npos ? std::string_view::npos : j - i));
        if (j == std::string_view::npos)
            return out;
        i = j + 1;
    }
}

bool isWord(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// "## Title ##" → "Title"; "" when the line is no heading.
std::string_view headingText(std::string_view t) {
    size_t n = 0;
    while (n < t.size() && t[n] == '#')
        ++n;
    if (n == 0 || n > 6 || n >= t.size() || (t[n] != ' ' && t[n] != '\t'))
        return {};
    std::string_view rest = str::trim(t.substr(n));
    // A closing run of #s goes, with the spaces before it.
    size_t           end  = rest.size();
    while (end > 0 && rest[end - 1] == '#')
        --end;
    if (end < rest.size())
        rest = str::trim(rest.substr(0, end));
    return rest;
}

void replaceAll(std::string &s, std::string_view from, std::string_view to) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    for (;;) {
        const size_t j = s.find(from, i);
        if (j == std::string::npos) {
            out.append(s, i, std::string::npos);
            break;
        }
        out.append(s, i, j - i);
        out.append(to);
        i = j + from.size();
    }
    s = std::move(out);
}

// "claude:agent" / "claude:role:x" at s[i] (just past the '@'): its length,
// 0 when it isn't one or runs on into a word.
size_t teammateIdAt(std::string_view s, size_t i) {
    if (s.substr(i, 7) != "claude:")
        return 0;
    size_t j = i + 7;
    if (s.substr(j, 5) == "agent") {
        j += 5;
    } else if (s.substr(j, 5) == "role:") {
        j += 5;
        const size_t from = j;
        while (j < s.size() &&
               ((s[j] >= 'a' && s[j] <= 'z') || (s[j] >= '0' && s[j] <= '9') || s[j] == '-'))
            ++j;
        if (j == from)
            return 0;
    } else {
        return 0;
    }
    if (j < s.size() && (isWord(s[j]) || s[j] == '-'))
        return 0;
    return j - i;
}

bool wholeTeammate(std::string_view s) {
    return s.size() > 1 && s[0] == '@' && teammateIdAt(s, 1) == s.size() - 1;
}

// One plain (not code) piece of an escaped line: Claude's own <url>
// autolinks restored, teammate mentions as <@…> tokens, bare URLs as <url>.
std::string linkPlain(std::string_view part) {
    std::string out;
    out.reserve(part.size() + 16);
    size_t i = 0;
    while (i < part.size()) {
        const char c = part[i];
        // &lt;https://…&gt; — an autolink, escaped with the rest.
        if (c == '&' && part.substr(i, 4) == "&lt;") {
            const std::string_view rest = part.substr(i + 4);
            if (str::startsWith(rest, "http://") || str::startsWith(rest, "https://")) {
                // [^\s&]+ (&amp;[^\s&]+)* then &gt;
                size_t j  = 0;
                bool   ok = false;
                for (;;) {
                    const size_t from = j;
                    while (j < rest.size() && rest[j] != '&' && rest[j] != ' ' && rest[j] != '\t' &&
                           rest[j] != '\n')
                        ++j;
                    if (j == from)
                        break;
                    if (rest.substr(j, 4) == "&gt;") {
                        ok = true;
                        break;
                    }
                    if (rest.substr(j, 5) != "&amp;")
                        break;
                    j += 5;
                }
                if (ok) {
                    out += '<';
                    out.append(rest.substr(0, j));
                    out += '>';
                    i += 4 + j + 4;
                    continue;
                }
            }
            // &lt;@claude:…&gt; — a raw token from before pills were unwrapped.
            if (!rest.empty() && rest[0] == '@') {
                const size_t n = teammateIdAt(rest, 1);
                if (n && rest.substr(1 + n, 4) == "&gt;") {
                    out += "<@";
                    out.append(rest.substr(1, n));
                    out += '>';
                    i += 4 + 1 + n + 4;
                    continue;
                }
            }
        }
        // @claude:role:x, not inside a word, path or address.
        if (c == '@') {
            const bool after =
                i > 0 && (isWord(part[i - 1]) || part[i - 1] == '@' || part[i - 1] == '/' ||
                          part[i - 1] == ':' || part[i - 1] == '.' || part[i - 1] == '-');
            if (const size_t n = after ? 0 : teammateIdAt(part, i + 1)) {
                out += "<@";
                out.append(part.substr(i + 1, n));
                out += '>';
                i += 1 + n;
                continue;
            }
        }
        // A bare URL — not a markdown link's target, not a <url> already.
        if ((c == 'h') && (part.substr(i, 7) == "http://" || part.substr(i, 8) == "https://") &&
            !(i >= 2 && part[i - 2] == ']' && part[i - 1] == '(') &&
            !(i >= 1 && part[i - 1] == '<')) {
            size_t j = i;
            while (j < part.size()) {
                const char d = part[j];
                if (d == ' ' || d == '\t' || d == '\n' || d == '\r' || d == '<' || d == '>' ||
                    d == '[' || d == ']' || d == '`')
                    break;
                ++j;
            }
            std::string_view url = part.substr(i, j - i);
            // Sentence punctuation after a URL isn't part of it, nor is a ")"
            // closing a parenthesis the URL sits in.
            while (!url.empty()) {
                const char back = url.back();
                if (std::string_view(".,;:!?'\"*_~").find(back) != std::string_view::npos ||
                    (back == ')' && std::count(url.begin(), url.end(), '(') <
                                        std::count(url.begin(), url.end(), ')')))
                    url.remove_suffix(1);
                else if (str::endsWith(url, "&gt;") || str::endsWith(url, "&lt;"))
                    url.remove_suffix(4);
                else
                    break;
            }
            if (url.size() > 8) { // not "https://" alone
                out += '<';
                out.append(url);
                out += '>';
                i += url.size();
                continue;
            }
        }
        out += c;
        ++i;
    }
    return out;
}

// Slack's server turns every bare URL into a <url> link before a client sees
// it; Claude's text comes raw. Wrap them the same way (in already-escaped
// text), outside code, and leave markdown [label](url) links to the
// converter. A teammate mention ("@claude:role:engineer") is how the
// composer's pill reaches Claude; shown, it becomes a <@…> mention again.
std::string linkBareUrls(std::string_view escaped) {
    std::string out;
    out.reserve(escaped.size() + 32);
    bool       inFence = false;
    const auto lines   = splitLines(escaped);
    for (size_t li = 0; li < lines.size(); ++li) {
        const std::string_view line = lines[li];
        if (li > 0)
            out += '\n';
        if (str::startsWith(str::trim(line), "```")) {
            inFence = !inFence;
            out.append(line);
            continue;
        }
        if (inFence) {
            out.append(line);
            continue;
        }
        std::vector<std::string_view> spans;
        for (size_t i = 0;;) {
            const size_t j = line.find('`', i);
            spans.push_back(
                line.substr(i, j == std::string_view::npos ? std::string_view::npos : j - i)
            );
            if (j == std::string_view::npos)
                break;
            i = j + 1;
        }
        for (size_t i = 0; i < spans.size(); ++i) {
            // Odd pieces sit between backticks — unless the last backtick is
            // unpaired, then that tail is plain text.
            const bool code = i % 2 == 1 && (i < spans.size() - 1 || spans.size() % 2 == 1);
            // Claude likes to quote a teammate's id as `@claude:role:x`; a span
            // that is nothing but one is a mention, not code.
            if (code && wholeTeammate(spans[i])) {
                out += '<';
                out.append(spans[i]);
                out += '>';
                ++i;
                if (i < spans.size())
                    out += linkPlain(spans[i]);
                continue;
            }
            if (i > 0)
                out += '`';
            if (code)
                out.append(spans[i]);
            else
                out += linkPlain(spans[i]);
        }
    }
    return out;
}

std::string mimeFor(std::string_view path) {
    const std::string ext = str::asciiLower(file::extension(path));
    static const struct {
        const char *ext, *mime;
    } kTypes[] = {
        {"png", "image/png"},
        {"jpg", "image/jpeg"},
        {"jpeg", "image/jpeg"},
        {"gif", "image/gif"},
        {"webp", "image/webp"},
        {"svg", "image/svg+xml"},
        {"pdf", "application/pdf"},
        {"txt", "text/plain"},
        {"md", "text/markdown"},
        {"csv", "text/csv"},
        {"html", "text/html"},
        {"json", "application/json"},
        {"mp3", "audio/mpeg"},
        {"m4a", "audio/mp4"},
        {"wav", "audio/wav"},
        {"mp4", "video/mp4"},
    };
    for (const auto &t : kTypes)
        if (ext == t.ext)
            return t.mime;
    return "application/octet-stream";
}

// "| a | b |" → {"a", "b"}. A pipe escaped (\|) or inside `code` stays in
// its cell.
std::vector<std::string> splitTableRow(std::string_view line) {
    std::string_view s = str::trim(line);
    if (str::startsWith(s, "|"))
        s.remove_prefix(1);
    if (str::endsWith(s, "|") && !str::endsWith(s, "\\|"))
        s.remove_suffix(1);
    std::vector<std::string> cells;
    std::string              cell;
    bool                     inCode = false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '\\' && i + 1 < s.size() && s[i + 1] == '|') {
            cell += '|';
            ++i;
        } else if (c == '`') {
            inCode = !inCode;
            cell += c;
        } else if (c == '|' && !inCode) {
            cells.emplace_back(str::trim(cell));
            cell.clear();
        } else {
            cell += c;
        }
    }
    cells.emplace_back(str::trim(cell));
    return cells;
}

// "|---|:--:|": a table's header separator (each cell :?-+:?).
bool isTableSeparator(std::string_view line) {
    std::string_view s = str::trim(line);
    if (str::startsWith(s, "|"))
        s.remove_prefix(1);
    if (str::endsWith(s, "|"))
        s.remove_suffix(1);
    for (;;) {
        const size_t     bar  = s.find('|');
        std::string_view cell = str::trim(s.substr(0, bar));
        if (str::startsWith(cell, ":"))
            cell.remove_prefix(1);
        if (str::endsWith(cell, ":"))
            cell.remove_suffix(1);
        if (cell.empty() || cell.find_first_not_of('-') != std::string_view::npos)
            return false;
        if (bar == std::string_view::npos)
            return true;
        s.remove_prefix(bar + 1);
    }
}

} // namespace

std::string escapeMrkdwn(std::string_view text) {
    // A colon opening a ":name:" would read as an emoji.
    const auto emojiOpener = [&](size_t i) {
        size_t j = i + 1;
        while (j < text.size() && (isWord(text[j]) || text[j] == '+' || text[j] == '-'))
            ++j;
        return j > i + 1 && j < text.size() && text[j] == ':';
    };
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        // Marks meant literally (mrkdwn's "&#42;").
        case '*':
        case '_':
        case '~':
        case '`':
            out += str::concat({"&#", str::number(int64_t(uint8_t(c))), ";"});
            break;
        case ':':
            out += emojiOpener(i) ? "&#58;" : ":";
            break;
        default:
            out += c;
        }
    }
    return out;
}

std::string renderMarkdown(std::string_view markdown) {
    // Pre-pass on whole lines, outside code fences: headings have no mrkdwn
    // form, so bold them; a table is only readable aligned, so fence it
    // (monospace).
    const auto  lines = splitLines(markdown);
    std::string text;
    text.reserve(markdown.size() + 16);
    bool first   = true;
    bool inFence = false;
    auto put     = [&](std::string_view l) {
        if (!first)
            text += '\n';
        first = false;
        text.append(l);
    };
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string_view line    = lines[i];
        const std::string_view trimmed = str::trim(line);
        if (str::startsWith(trimmed, "```")) {
            inFence = !inFence;
            put(line);
            continue;
        }
        if (inFence) {
            put(line);
            continue;
        }
        if (str::startsWith(trimmed, "|")) {
            size_t end = i;
            while (end + 1 < lines.size() && str::startsWith(str::trim(lines[end + 1]), "|"))
                ++end;
            if (end > i) {
                put("```");
                for (size_t k = i; k <= end; ++k)
                    put(lines[k]);
                put("```");
                i = end;
                continue;
            }
        }
        if (const std::string_view h = headingText(trimmed); !h.empty()) {
            put(str::concat({"**", h, "**"}));
            continue;
        }
        put(line);
    }
    // Claude's text is plain markdown, never Slack tokens: escape what mrkdwn
    // would read as a token or entity. The parser decodes these back.
    replaceAll(text, "&", "&amp;");
    replaceAll(text, "<", "&lt;");
    replaceAll(text, ">", "&gt;");
    // Markdown strikes only with "~~": a lone '~' is "approximately" ("~4 MB
    // on Linux, ~3 MB"), which mrkdwn would pair into a strike.
    std::string marked;
    marked.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        const bool lone = text[i] == '~' && (i == 0 || text[i - 1] != '~') &&
                          (i + 1 == text.size() || text[i + 1] != '~');
        if (lone)
            marked += "&#126;";
        else
            marked += text[i];
    }
    return mrkdwn::convertOutgoing(linkBareUrls(marked));
}

std::vector<model::Block> markdownBlocks(std::string_view markdown) {
    const auto                lines = splitLines(markdown);
    std::vector<model::Block> blocks;
    std::string               text; // lines waiting to become a text block
    bool                      sawTable  = false;
    const auto                flushText = [&] {
        const std::string_view chunk = str::trim(text);
        if (!chunk.empty()) {
            model::Block b;
            b.text = renderMarkdown(chunk);
            blocks.push_back(std::move(b));
        }
        text.clear();
    };
    bool inFence = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string_view trimmed = str::trim(lines[i]);
        if (str::startsWith(trimmed, "```"))
            inFence = !inFence;
        // A table: a "|" row followed by its "|---|" separator line.
        if (!inFence && str::startsWith(trimmed, "|") && i + 1 < lines.size() &&
            isTableSeparator(lines[i + 1])) {
            flushText();
            model::Block table;
            table.kind        = model::Block::Kind::Table;
            const auto addRow = [&](std::string_view line) {
                std::vector<std::string> row;
                for (const std::string &cell : splitTableRow(line))
                    row.push_back(renderMarkdown(cell));
                table.rows.push_back(std::move(row));
            };
            addRow(lines[i]); // the header: the table bolds row 0
            for (i += 2; i < lines.size() && str::startsWith(str::trim(lines[i]), "|"); ++i)
                addRow(lines[i]);
            --i;
            blocks.push_back(std::move(table));
            sawTable = true;
            continue;
        }
        if (!text.empty())
            text += '\n';
        text.append(lines[i]);
    }
    if (!sawTable)
        return {};
    flushText();
    return blocks;
}

model::Message toMessage(const TranscriptItem &item, model::UserRef me, model::UserRef claude) {
    model::Message m;
    m.ts = item.ts;
    switch (item.kind) {
    case TranscriptItem::Kind::UserPrompt:
        m.user = me;
        m.text = renderMarkdown(item.text);
        if (auto blocks = markdownBlocks(item.text); !blocks.empty())
            m.extras().blocks = std::move(blocks);
        for (size_t i = 0; i < item.images.size(); ++i) {
            const std::string &path = item.images[i];
            model::File        f;
            f.id = str::concat({model::formatTs(item.ts), "-img", str::number(int64_t(i))});
            f.name =
                i < item.imageNames.size() ? item.imageNames[i] : std::string(file::baseName(path));
            f.mime       = mimeFor(path);
            f.prettyType = str::asciiUpper(file::extension(path));
            f.path       = path;
            f.size       = std::max<int64_t>(0, file::size(path));
            // A file sent along that isn't a picture is a download.
            if (str::startsWith(f.mime, "image/") &&
                (!showsAsPicture(path, f.mime) || !model::imageSize(path, &f.width, &f.height))) {
                f.width  = 0;
                f.height = 0;
            }
            m.extras().files.push_back(std::move(f));
        }
        break;
    case TranscriptItem::Kind::AssistantText: {
        m.user                 = claude;
        const std::string text = item.loginError ? std::string(notLoggedInMessage()) : item.text;
        m.text                 = renderMarkdown(text);
        if (auto blocks = markdownBlocks(text); !blocks.empty())
            m.extras().blocks = std::move(blocks);
        if (item.state == TranscriptItem::State::Progress)
            m.extras().subtype = kProgressSubtype;
        break;
    }
    case TranscriptItem::Kind::ToolGroup: {
        m.user                      = claude;
        m.extras().subtype          = kProgressSubtype;
        // One card per run of calls: the tool and what it touched, one per
        // line. Capped so a turn of a hundred calls stays a compact card.
        constexpr size_t  kMaxLines = 12;
        const size_t      n         = item.tools.size();
        model::Attachment card;
        card.color = "#8a8a8a";
        card.title = i18n::trn("%n tool call", "%n tool calls", int64_t(n));
        std::string  body;
        const size_t first = n > kMaxLines ? n - kMaxLines : 0;
        if (first > 0)
            body = i18n::trn("\xE2\x80\xA6 %n earlier", "\xE2\x80\xA6 %n earlier", int64_t(first)) +
                   "\n";
        for (size_t i = first; i < n; ++i) {
            const auto       &c       = item.tools[i];
            // Plain text: the tool's name bold, what it touched as is.
            const std::string summary = escapeMrkdwn(c.summary);
            body += str::concat({"*", escapeMrkdwn(c.name), "*"});
            if (!summary.empty())
                body += "  " + summary;
            if (c.error)
                body += "  \xE2\x9C\x97"; // ✗
            if (i + 1 < n)
                body += '\n';
        }
        card.text = std::move(body);
        // No text of its own: the card says it all.
        m.extras().attachments.push_back(std::move(card));
        break;
    }
    case TranscriptItem::Kind::Subagent: {
        m.user             = claude;
        m.extras().subtype = kProgressSubtype;
        const std::string what =
            item.text.empty() && !item.tools.empty() ? item.tools.front().name : item.text;
        m.text = str::concat({"_", i18n::arg(tr("Subagent: %1"), escapeMrkdwn(what)), "_"});
        break;
    }
    case TranscriptItem::Kind::PeerMessage: {
        // Said to the session, not by the user: shown as what it is. A report
        // from a subagent with a thread here is a pointer to that thread
        // instead (Backend::visibleMessages).
        m.user             = claude;
        m.extras().subtype = kProgressSubtype; // what the session says next notifies
        std::string header;
        if (!item.agentId.empty())
            header = tr("Subagent report");
        else if (!item.peerName.empty())
            header = i18n::arg(tr("Message from session “%1”"), item.peerName);
        else
            header = tr("Message from another session");
        m.text = renderMarkdown(str::concat({"_", header, "_\n\n", item.text}));
        break;
    }
    }
    return m;
}

} // namespace claude
