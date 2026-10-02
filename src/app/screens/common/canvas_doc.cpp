#include "app/screens/common/canvas_doc.h"

#include "base/str.h"

#include <algorithm>
#include <cstdlib>

namespace screens::canvas {

namespace {

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if ((a[i] | 0x20) != (b[i] | 0x20))
            return false;
    return true;
}

// An attribute's value in an opening tag ("" if absent).
std::string_view attr(std::string_view tag, std::string_view name) {
    for (size_t at = tag.find(name); at != std::string_view::npos; at = tag.find(name, at + 1)) {
        const size_t eq = at + name.size();
        if ((at > 0 && tag[at - 1] != ' ') || eq + 1 >= tag.size() || tag[eq] != '=')
            continue;
        const char q = tag[eq + 1];
        if (q != '"' && q != '\'')
            continue;
        const size_t end = tag.find(q, eq + 2);
        return end == std::string_view::npos ? std::string_view()
                                             : tag.substr(eq + 2, end - eq - 2);
    }
    return {};
}

// Builds the editor's HTML: one <p> per line, its markdown prefix first.
struct Lines {
    std::string out, line;
    bool        hasText = false, space = false;

    void start(std::string_view prefix) {
        end();
        line = prefix;
    }
    void end() {
        if (hasText)
            out += str::concat({"<p>", line, "</p>"});
        line.clear();
        hasText = space = false;
    }
    void text(std::string_view t) {
        for (char c : t) {
            if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
                space = hasText;
                continue;
            }
            if (space)
                line += ' ';
            space   = false;
            hasText = true;
            line += c;
        }
    }
    void markup(std::string_view tag) { line += tag; } // inline tags: no text of their own
};

} // namespace

std::string
editorHtml(std::string_view h, const std::vector<std::string> &titles, std::string *title) {
    if (title)
        title->clear();
    // The title: a leading <h1> naming the file (msga's splitTitleH1).
    {
        size_t at = 0;
        while (at < h.size()) {
            at = h.find_first_not_of(" \t\r\n", at);
            if (at == std::string_view::npos || h.compare(at, 4, "<div") != 0)
                break;
            at = h.find('>', at) + 1; // the quip-canvas-content wrapper
        }
        if (at != std::string_view::npos && h.compare(at, 3, "<h1") == 0) {
            const size_t open  = h.find('>', at);
            const size_t close = open == std::string_view::npos ? open : h.find("</h1>", open);
            if (close != std::string_view::npos) {
                std::string              text;
                std::vector<uint16_t>    fmt;
                std::vector<std::string> links;
                ui::rich::fromHtml(h.substr(open + 1, close - open - 1), &text, &fmt, &links);
                const std::string t(str::trim(text));
                for (const std::string &cand : titles)
                    if (!t.empty() && std::string_view(str::trim(cand)) == t) {
                        if (title)
                            *title = t;
                        h = h.substr(close + 5);
                        break;
                    }
            }
        }
    }

    Lines lines;
    struct List {
        bool    ordered = false, checklist = false;
        int32_t n = 0;
    };
    std::vector<List> lists;
    int               quote = 0, pre = 0, rowCells = -1, skip = 0, sectionStyle = 0;
    bool              firstRow = true, br = false, code = false;
    auto              prefix = [&](int heading) {
        std::string p;
        for (int i = 0; i < quote; ++i)
            p += "&gt; ";
        if (heading)
            return p + std::string(size_t(heading), '#') + ' ';
        return p;
    };
    for (size_t i = 0; i < h.size();) {
        if (h[i] != '<') {
            const size_t e = std::min(h.find('<', i), h.size());
            if (!skip) {
                if (br && rowCells < 0) {
                    lines.start(prefix(0));
                    br = false;
                }
                if (pre) {
                    std::string_view t = h.substr(i, e - i);
                    for (size_t nl; (nl = t.find('\n')) != std::string_view::npos;) {
                        lines.line += t.substr(0, nl);
                        lines.hasText = true;
                        lines.start("");
                        t.remove_prefix(nl + 1);
                    }
                    lines.line += t;
                    lines.hasText = lines.hasText || !t.empty();
                } else {
                    lines.text(h.substr(i, e - i));
                }
            }
            i = e;
            continue;
        }
        const size_t e = h.find('>', i);
        if (e == std::string_view::npos)
            break;
        std::string_view tag = h.substr(i + 1, e - i - 1);
        i                    = e + 1;
        const bool closing   = !tag.empty() && tag[0] == '/';
        if (closing)
            tag.remove_prefix(1);
        size_t ne = 0;
        while (ne < tag.size() && tag[ne] != ' ' && tag[ne] != '/' && tag[ne] != '\t')
            ++ne;
        const std::string_view name = tag.substr(0, ne);
        if (ieq(name, "style") || ieq(name, "script")) {
            skip += closing ? -1 : 1;
            skip = std::max(skip, 0);
            continue;
        }
        if (skip)
            continue;
        if (ieq(name, "br")) {
            br = lines.hasText; // a break inside a block: the rest on its own line
            continue;
        }
        if (name.size() == 2 && (name[0] == 'h' || name[0] == 'H') && name[1] >= '1' &&
            name[1] <= '6') {
            if (closing)
                lines.end();
            else
                lines.start(prefix(std::min(name[1] - '0', 3)));
            br = false;
            continue;
        }
        if (ieq(name, "ul") || ieq(name, "ol")) {
            if (closing) {
                if (!lists.empty())
                    lists.pop_back();
                lines.end();
            } else {
                lines.end();
                // Quip's lists are all <ul>s; the wrapping div's
                // data-section-style says which kind (5 bullets, 6
                // numbers, 7 checklist).
                List l;
                l.ordered   = ieq(name, "ol") || sectionStyle == 6;
                l.checklist = sectionStyle == 7 ||
                              attr(tag, "class").find("checklist") != std::string_view::npos;
                lists.push_back(l);
                sectionStyle = 0;
            }
            continue;
        }
        if (ieq(name, "li")) {
            if (closing) {
                lines.end();
            } else {
                std::string p = prefix(0);
                if (!lists.empty()) {
                    List &l = lists.back();
                    p.append((lists.size() - 1) * 2, ' ');
                    if (l.checklist)
                        p += attr(tag, "class").find("checked") != std::string_view::npos
                                 ? "- [x] "
                                 : "- [ ] ";
                    else if (l.ordered)
                        p += std::to_string(++l.n) + ". ";
                    else
                        p += "- ";
                } else {
                    p += "- ";
                }
                lines.start(p);
            }
            br = false;
            continue;
        }
        if (ieq(name, "blockquote")) {
            lines.end();
            quote = std::max(0, quote + (closing ? -1 : 1));
            continue;
        }
        if (ieq(name, "pre")) { // a fenced block, a line per line
            lines.start("```");
            lines.hasText = true;
            lines.end();
            pre = std::max(0, pre + (closing ? -1 : 1));
            continue;
        }
        if (ieq(name, "table")) {
            lines.end();
            firstRow = true;
            continue;
        }
        if (ieq(name, "tr")) {
            if (closing) {
                lines.markup(" |");
                lines.hasText   = true;
                const int cells = rowCells;
                lines.end();
                if (firstRow && cells > 0) { // markdown's header rule
                    std::string rule = "|";
                    for (int c = 0; c < cells; ++c)
                        rule += " --- |";
                    lines.out += str::concat({"<p>", rule, "</p>"});
                }
                firstRow = false;
                rowCells = -1;
            } else {
                lines.start("|");
                rowCells = 0;
            }
            continue;
        }
        if (ieq(name, "td") || ieq(name, "th")) {
            if (!closing && rowCells >= 0) {
                lines.markup(rowCells ? " | " : " ");
                lines.space = false;
                ++rowCells;
            }
            continue;
        }
        if (ieq(name, "div")) {
            if (!closing)
                sectionStyle = std::atoi(std::string(attr(tag, "data-section-style")).c_str());
            continue;
        }
        if (ieq(name, "p")) {
            if (rowCells >= 0 || !lists.empty())
                continue; // a cell's or an item's paragraph stays on its line
            // A code block: one <p class="prettyprint">, lines split by <br>.
            if (!closing && attr(tag, "class").find("prettyprint") != std::string_view::npos) {
                lines.start("```");
                lines.hasText = true;
                code          = true;
            }
            if (closing) {
                lines.end();
                if (code) {
                    lines.start("```");
                    lines.hasText = true;
                    lines.end();
                }
                code = false;
            } else {
                lines.start(prefix(0));
            }
            br = false;
            continue;
        }
        // Inline formats pass through for insertHtml; links (Slack's <lnk>
        // too) keep their target.
        static const struct {
            std::string_view from, to;
        } kInline[] = {
            {"b", "b"},
            {"strong", "b"},
            {"i", "i"},
            {"em", "i"},
            {"s", "s"},
            {"del", "s"},
            {"strike", "s"},
            {"code", "code"},
        };
        bool known = false;
        for (const auto &k : kInline)
            if (ieq(name, k.from)) {
                lines.markup(str::concat({closing ? "</" : "<", k.to, ">"}));
                known = true;
                break;
            }
        if (!known && (ieq(name, "a") || ieq(name, "lnk"))) {
            if (closing)
                lines.markup("</a>");
            else
                lines.markup(str::concat({"<a href=\"", attr(tag, "href"), "\">"}));
        }
    }
    lines.end();
    return lines.out;
}

std::string markdown(const std::string &t, const std::vector<ui::TextEdit::Run> &runs) {
    // Lines of the block kinds that stay together without a blank line.
    auto grouped = [](std::string_view l) {
        l = str::trim(l);
        if (l.empty())
            return false;
        if (str::startsWith(l, "- ") || str::startsWith(l, "* ") || str::startsWith(l, "> ") ||
            l[0] == '|' || str::startsWith(l, "```"))
            return true;
        size_t d = 0;
        while (d < l.size() && l[d] >= '0' && l[d] <= '9')
            ++d;
        return d > 0 && str::startsWith(l.substr(d), ". ");
    };
    std::string out, prev;
    bool        inFence = false;
    for (size_t at = 0; at <= t.size();) {
        const size_t           nl = std::min(t.find('\n', at), t.size());
        const std::string_view raw(t.data() + at, nl - at);
        std::string            line;
        for (const ui::TextEdit::Run &r : runs) {
            const uint32_t a = std::max<uint32_t>(r.start, uint32_t(at));
            const uint32_t b = std::min<uint32_t>(r.end, uint32_t(nl));
            if (a >= b)
                continue;
            std::string_view piece(t.data() + a, b - a);
            if ((!r.format && r.link.empty()) || inFence) {
                line += piece;
                continue;
            }
            const size_t lead  = piece.find_first_not_of(' ');
            const size_t trail = piece.find_last_not_of(' ');
            if (lead == std::string_view::npos) {
                line += piece;
                continue;
            }
            const std::string_view core = piece.substr(lead, trail - lead + 1);
            std::string            open, close;
            if (!r.link.empty())
                open += '[';
            if (r.format & ui::TextEdit::Bold)
                open += "**", close.insert(0, "**");
            if (r.format & ui::TextEdit::Italic)
                open += '_', close.insert(0, "_");
            if (r.format & ui::TextEdit::Strike)
                open += "~~", close.insert(0, "~~");
            if (r.format & ui::TextEdit::Code)
                open += '`', close.insert(0, "`");
            if (!r.link.empty())
                close += str::concat({"](", r.link, ")"});
            line += piece.substr(0, lead);
            line += open;
            line += core;
            line += close;
            line += piece.substr(trail + 1);
        }
        at = nl + 1;
        if (str::startsWith(str::trim(raw), "```"))
            inFence = !inFence;
        if (str::trim(line).empty()) {
            if (inFence)
                out += '\n';
            continue;
        }
        if (!out.empty())
            out += grouped(prev) && grouped(line) ? "\n" : "\n\n";
        out += line;
        prev = line;
    }
    return out;
}

std::string markdown(
    const std::string &t, const std::vector<uint16_t> &fmt, const std::vector<std::string> &links
) {
    std::vector<ui::TextEdit::Run> runs;
    const uint32_t                 n = uint32_t(std::min(t.size(), fmt.size()));
    for (uint32_t i = 0; i < n;) {
        uint32_t j = i + 1;
        while (j < n && fmt[j] == fmt[i])
            ++j;
        ui::TextEdit::Run r;
        r.start           = i;
        r.end             = j;
        r.format          = uint8_t(fmt[i] & 0xff);
        const size_t link = fmt[i] >> 8;
        if (link && link <= links.size())
            r.link = links[link - 1];
        runs.push_back(r);
        i = j;
    }
    if (n < t.size()) {
        ui::TextEdit::Run r;
        r.start = n;
        r.end   = uint32_t(t.size());
        runs.push_back(r);
    }
    return markdown(t, runs);
}

// ── Section diff ────────────────────────────────────────────────────────────

namespace {

using Kind = Chunk::Kind;

bool isListLine(std::string_view l) {
    l = str::trim(l);
    if (str::startsWith(l, "- ") || str::startsWith(l, "* "))
        return true;
    size_t d = 0;
    while (d < l.size() && l[d] >= '0' && l[d] <= '9')
        ++d;
    return d > 0 && str::startsWith(l.substr(d), ". ");
}

// The text from the start of `html` with the leading title h1 taken out
// (editorHtml's rule), and the quip wrapper dropped.
std::string_view bodyOf(std::string_view h, const std::vector<std::string> &titles) {
    size_t at = 0;
    while (at < h.size()) {
        at = h.find_first_not_of(" \t\r\n", at);
        if (at == std::string_view::npos || h.compare(at, 4, "<div") != 0 ||
            attr(h.substr(at, h.find('>', at) - at), "class").find("quip-canvas-content") ==
                std::string_view::npos)
            break;
        at         = h.find('>', at) + 1;
        // ... and its closing tag at the very end.
        size_t end = h.find_last_not_of(" \t\r\n");
        if (end != std::string_view::npos && end >= 5 && h.compare(end - 5, 6, "</div>") == 0)
            h = h.substr(0, end - 5);
    }
    if (at == std::string_view::npos)
        return {};
    h = h.substr(at);
    std::string title;
    editorHtml(
        h.substr(
            0,
            std::min(
                h.size(),
                h.find("</h1>") == std::string_view::npos ? size_t(0) : h.find("</h1>") + 5
            )
        ),
        titles,
        &title
    );
    if (!title.empty())
        h = h.substr(h.find("</h1>") + 5);
    return h;
}

// End (exclusive) of the element whose opening tag starts at `start`,
// counting nested tags of the same name; npos when unbalanced.
size_t elementEnd(std::string_view h, size_t start, std::string_view name) {
    int    depth = 0;
    size_t pos   = start;
    while (pos < h.size()) {
        const size_t lt = h.find('<', pos);
        if (lt == std::string_view::npos)
            return std::string_view::npos;
        const size_t gt = h.find('>', lt);
        if (gt == std::string_view::npos)
            return std::string_view::npos;
        std::string_view tag     = h.substr(lt + 1, gt - lt - 1);
        const bool       closing = !tag.empty() && tag[0] == '/';
        if (closing)
            tag.remove_prefix(1);
        size_t ne = 0;
        while (ne < tag.size() && tag[ne] != ' ' && tag[ne] != '/' && tag[ne] != '\t' &&
               tag[ne] != '\n')
            ++ne;
        if (ieq(tag.substr(0, ne), name) && !(tag.size() && tag.back() == '/')) {
            depth += closing ? -1 : 1;
            if (depth == 0)
                return gt + 1;
        }
        pos = gt + 1;
    }
    return std::string_view::npos;
}

// An element's markdown as the editor would produce it.
std::string elementMd(std::string_view element) {
    const std::string        eh = editorHtml(element, {}, nullptr);
    std::string              text;
    std::vector<uint16_t>    fmt;
    std::vector<std::string> links;
    ui::rich::fromHtml(eh, &text, &fmt, &links);
    return markdown(text, fmt, links);
}

std::string_view tagId(std::string_view openTag) {
    return attr(openTag, "id");
}

} // namespace

std::vector<Chunk> documentChunks(std::string_view md) {
    std::vector<Chunk> out;
    Kind               open    = Kind::Para; // the group being extended
    bool               inGroup = false, fence = false;
    auto               push = [&](Kind k, std::string_view line, bool group) {
        std::string l(line);
        while (!l.empty() && (l.back() == ' ' || l.back() == '\t'))
            l.pop_back();
        if (group && inGroup && open == k && !out.empty()) {
            out.back().md += '\n';
            out.back().md += l;
            return;
        }
        Chunk c;
        c.kind    = k;
        c.fragile = k == Kind::Quote || k == Kind::Table;
        c.md      = std::move(l);
        out.push_back(std::move(c));
        open    = k;
        inGroup = group;
    };
    for (size_t at = 0; at <= md.size();) {
        const size_t           nl   = std::min(md.find('\n', at), md.size());
        const std::string_view line = md.substr(at, nl - at);
        at                          = nl + 1;
        const std::string_view t    = str::trim(line);
        if (fence) {
            out.back().md += '\n';
            out.back().md += std::string(line);
            if (str::startsWith(t, "```"))
                fence = false;
            continue;
        }
        if (t.empty()) {
            inGroup = false;
            continue;
        }
        if (str::startsWith(t, "```")) {
            push(Kind::Para, line, false);
            inGroup = false;
            fence   = true;
            continue;
        }
        if (t[0] == '|')
            push(Kind::Table, line, true);
        else if (str::startsWith(t, "> ") || t == ">")
            push(Kind::Quote, line, true);
        else if (isListLine(line))
            push(Kind::List, line, true);
        else if (t[0] == '#')
            push(Kind::Heading, line, false);
        else
            push(Kind::Para, line, false);
    }
    return out;
}

bool baseChunks(
    std::string_view html, const std::vector<std::string> &titles, std::vector<Chunk> *out
) {
    out->clear();
    const std::string_view h = bodyOf(html, titles);
    for (size_t pos = 0; pos < h.size();) {
        pos = h.find_first_not_of(" \t\r\n", pos);
        if (pos == std::string_view::npos)
            break;
        if (h[pos] != '<')
            return false; // stray text at the top level: unknown structure
        const size_t gt = h.find('>', pos);
        if (gt == std::string_view::npos)
            return false;
        const std::string_view open = h.substr(pos + 1, gt - pos - 1);
        size_t                 ne   = 0;
        while (ne < open.size() && open[ne] != ' ' && open[ne] != '/' && open[ne] != '>')
            ++ne;
        const std::string name(open.substr(0, ne));
        // A top-level picture: invisible context (its section is never
        // rewritten, so the picture survives on the server).
        if (ieq(name, "img") || ieq(name, "br")) {
            pos = gt + 1;
            continue;
        }
        const size_t end = elementEnd(h, pos, name);
        if (end == std::string_view::npos)
            return false;
        const std::string_view element = h.substr(pos, end - pos);
        pos                            = end;
        std::string_view id;
        bool             fragile = false;
        if (name.size() == 2 && (name[0] | 0x20) == 'h' && name[1] >= '1' && name[1] <= '6') {
            id = tagId(open);
        } else if (ieq(name, "p")) {
            id = tagId(open);
        } else if (ieq(name, "div") || ieq(name, "ul") || ieq(name, "ol")) {
            // A list: the <ul> carries the section id.
            size_t ul = element.find("<ul");
            if (ul == std::string_view::npos)
                ul = element.find("<ol");
            if (ul == std::string_view::npos)
                return false;
            id = tagId(element.substr(ul, element.find('>', ul) - ul));
        } else if (ieq(name, "blockquote") || ieq(name, "table")) {
            fragile = true;
        } else {
            return false; // <hr>, embeds, …: the whole-document save
        }
        if (!fragile && id.empty())
            return false;
        std::vector<Chunk> parts = documentChunks(elementMd(element));
        // One element may read as several editor chunks (a <p> with <br>s):
        // the first takes the id, the rest are context only.
        for (size_t i = 0; i < parts.size(); ++i) {
            parts[i].fragile = parts[i].fragile || fragile || i > 0;
            if (i == 0 && !fragile)
                parts[i].id = std::string(id);
            out->push_back(std::move(parts[i]));
        }
    }
    return true;
}

bool diff(const std::vector<Chunk> &base, const std::vector<Chunk> &cur, std::vector<Change> *ops) {
    using Op    = Change::Op;
    const int n = int(base.size()), m = int(cur.size());
    auto eq = [&](int i, int j) { return base[i].kind == cur[j].kind && base[i].md == cur[j].md; };
    // Longest common subsequence, suffix table.
    std::vector<int> dp(size_t(n + 1) * size_t(m + 1), 0);
    auto at = [&](int i, int j) -> int & { return dp[size_t(i) * size_t(m + 1) + size_t(j)]; };
    for (int i = n - 1; i >= 0; --i)
        for (int j = m - 1; j >= 0; --j)
            at(i, j) = eq(i, j) ? at(i + 1, j + 1) + 1 : std::max(at(i + 1, j), at(i, j + 1));
    std::vector<Change> inserts, replaces, deletes;
    // Slack rejects an empty document_content; a lone space is an empty line.
    auto safe = [](const std::string &md) { return md.empty() ? std::string(" ") : md; };
    int  i = 0, j = 0;
    while (i < n || j < m) {
        if (i < n && j < m && eq(i, j)) {
            ++i, ++j;
            continue;
        }
        // The maximal unmatched gap [i, bi) × [j, cj).
        int bi = i, cj = j;
        while (bi < n || cj < m) {
            if (bi < n && cj < m && eq(bi, cj) && at(bi, cj) == at(i, j))
                break; // the next match
            if (bi < n && at(bi + 1, cj) == at(i, j)) {
                ++bi;
                continue;
            }
            if (cj < m && at(bi, cj + 1) == at(i, j)) {
                ++cj;
                continue;
            }
            break;
        }
        const int lenB = bi - i, lenC = cj - j, pairs = std::min(lenB, lenC);
        for (int p = 0; p < pairs; ++p) {
            const Chunk &b = base[size_t(i + p)];
            if (b.fragile || b.id.empty())
                return false;
            replaces.push_back({Op::ReplaceSection, safe(cur[size_t(j + p)].md), b.id});
        }
        for (int p = pairs; p < lenB; ++p) {
            const Chunk &b = base[size_t(i + p)];
            if (b.fragile || b.id.empty())
                return false;
            deletes.push_back({Op::DeleteSection, {}, b.id});
        }
        if (lenC > pairs) {
            // An anchor for the extra chunks; inserts go first, so one that
            // is replaced or deleted later is still alive.
            std::string after, before;
            if (lenB > 0 && !base[size_t(i + lenB - 1)].fragile)
                after = base[size_t(i + lenB - 1)].id;
            else if (bi < n && !base[size_t(bi)].fragile && !base[size_t(bi)].id.empty())
                before = base[size_t(bi)].id;
            else if (i > 0 && !base[size_t(i - 1)].fragile && !base[size_t(i - 1)].id.empty())
                after = base[size_t(i - 1)].id;
            else if (n == 0)
                return false; // nothing to anchor on: replace the whole document
            else
                return false;
            if (!before.empty())
                for (int p = pairs; p < lenC; ++p)
                    inserts.push_back({Op::InsertBefore, safe(cur[size_t(j + p)].md), before});
            else // insert_after the same id: in reverse, so they end up in order
                for (int p = lenC - 1; p >= pairs; --p)
                    inserts.push_back({Op::InsertAfter, safe(cur[size_t(j + p)].md), after});
        }
        i = bi;
        j = cj;
    }
    ops->clear();
    ops->insert(ops->end(), inserts.begin(), inserts.end());
    ops->insert(ops->end(), replaces.begin(), replaces.end());
    ops->insert(ops->end(), deletes.begin(), deletes.end());
    return true;
}

} // namespace screens::canvas
