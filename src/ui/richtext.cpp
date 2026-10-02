// text/html for the clipboard: a writer for our few formats and a forgiving
// reader for what browsers, Slack, Google Docs and office suites put there.
// Not an HTML parser — a tag scanner that understands inline formatting,
// links, line breaks and block boundaries, and ignores everything else.
#include "ui/textedit.h"

#include "base/str.h"

#include <cstring>

namespace ui::rich {

namespace {

constexpr uint16_t kBold = TextEdit::Bold, kItalic = TextEdit::Italic;
constexpr uint16_t kStrike = TextEdit::Strike, kCode = TextEdit::Code;

void escape(std::string &out, std::string_view s, bool attr) {
    for (char c : s) {
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
        case '"':
            out += attr ? "&quot;" : "\"";
            break;
        case '\n':
            out += attr ? " " : "<br>";
            break;
        default:
            out.push_back(c);
        }
    }
}

bool ieq(std::string_view a, const char *b) {
    const size_t n = std::strlen(b);
    if (a.size() != n)
        return false;
    for (size_t i = 0; i < n; ++i)
        if ((a[i] | 0x20) != b[i])
            return false;
    return true;
}

bool ieqv(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if ((a[i] | 0x20) != (b[i] | 0x20))
            return false;
    return true;
}

bool icontains(std::string_view hay, const char *needle) {
    const size_t n = std::strlen(needle);
    for (size_t i = 0; i + n <= hay.size(); ++i) {
        size_t k = 0;
        while (k < n && (hay[i + k] | 0x20) == needle[k])
            ++k;
        if (k == n)
            return true;
    }
    return false;
}

// Decodes entities in s (text or an attribute value); &nbsp; is a plain space.
std::string decode(std::string_view s) {
    return str::decodeEntities(s, true);
}

// Value of attribute `name` inside a tag's attribute text.
std::string attr(std::string_view tag, const char *name) {
    const size_t n = std::strlen(name);
    for (size_t i = 0; i + n < tag.size(); ++i) {
        if ((i > 0 && tag[i - 1] != ' ' && tag[i - 1] != '\t' && tag[i - 1] != '\n') ||
            !ieq(tag.substr(i, n), name))
            continue;
        size_t k = i + n;
        while (k < tag.size() && tag[k] == ' ')
            ++k;
        if (k >= tag.size() || tag[k] != '=')
            continue;
        ++k;
        while (k < tag.size() && tag[k] == ' ')
            ++k;
        if (k >= tag.size())
            return {};
        const char q = tag[k];
        if (q == '"' || q == '\'') {
            const size_t e = tag.find(q, k + 1);
            return decode(
                tag.substr(k + 1, (e == std::string_view::npos ? tag.size() : e) - k - 1)
            );
        }
        size_t e = k;
        while (e < tag.size() && tag[e] != ' ' && tag[e] != '>')
            ++e;
        return decode(tag.substr(k, e - k));
    }
    return {};
}

bool isBlock(std::string_view n) {
    static const char *blocks[] = {
        "p",
        "div",
        "li",
        "tr",
        "h1",
        "h2",
        "h3",
        "h4",
        "h5",
        "h6",
        "ul",
        "ol",
        "blockquote",
        "table",
        "pre"
    };
    for (const char *b : blocks)
        if (ieq(n, b))
            return true;
    return false;
}

} // namespace

std::string
toHtml(std::string_view text, const uint16_t *fmt, const std::vector<std::string> &links) {
    std::string out;
    size_t      i = 0;
    while (i < text.size()) {
        size_t j = i + 1;
        while (j < text.size() && fmt[j] == fmt[i])
            ++j;
        const uint16_t f    = fmt[i];
        const size_t   link = f >> 8;
        if (link && link <= links.size()) {
            out += "<a href=\"";
            escape(out, links[link - 1], true);
            out += "\">";
        }
        if (f & kBold)
            out += "<b>";
        if (f & kItalic)
            out += "<i>";
        if (f & kStrike)
            out += "<s>";
        if (f & kCode)
            out += "<code>";
        escape(out, text.substr(i, j - i), false);
        if (f & kCode)
            out += "</code>";
        if (f & kStrike)
            out += "</s>";
        if (f & kItalic)
            out += "</i>";
        if (f & kBold)
            out += "</b>";
        if (link && link <= links.size())
            out += "</a>";
        i = j;
    }
    return out;
}

void fromHtml(
    std::string_view          h,
    std::string              *text,
    std::vector<uint16_t>    *fmt,
    std::vector<std::string> *links
) {
    struct Open {
        std::string_view name;
        uint16_t         formats; // what this element turned on
        uint16_t         link;    // link index it set (0 = none)
        bool             pre;
    };
    std::vector<Open> stack;
    text->clear();
    fmt->clear();
    int  skip         = 0; // inside <script>/<style>/<head>/<title>
    bool pendingSpace = false;
    // Only the StartFragment…EndFragment part of Windows' CF_HTML-style payloads.
    if (size_t s = h.find("<!--StartFragment-->"); s != std::string_view::npos) {
        const size_t e = h.find("<!--EndFragment-->", s);
        h = h.substr(s + 20, e == std::string_view::npos ? std::string_view::npos : e - s - 20);
    }
    auto current = [&]() {
        uint16_t f = 0, link = 0;
        for (const Open &o : stack) {
            f |= o.formats;
            if (o.link)
                link = o.link;
        }
        return uint16_t(f | (link << 8));
    };
    auto inPre = [&]() {
        for (const Open &o : stack)
            if (o.pre)
                return true;
        return false;
    };
    auto emit = [&](std::string_view s, uint16_t f) {
        text->append(s);
        fmt->insert(fmt->end(), s.size(), f);
    };
    auto newline = [&]() {
        pendingSpace = false;
        if (!text->empty() && text->back() != '\n')
            emit("\n", 0);
    };
    size_t i = 0;
    while (i < h.size()) {
        if (h[i] == '<') {
            if (h.substr(i, 4) == "<!--") {
                const size_t e = h.find("-->", i + 4);
                i              = e == std::string_view::npos ? h.size() : e + 3;
                continue;
            }
            const size_t e = h.find('>', i);
            if (e == std::string_view::npos)
                break;
            std::string_view tag = h.substr(i + 1, e - i - 1);
            i                    = e + 1;
            if (tag.empty() || tag[0] == '!' || tag[0] == '?')
                continue;
            const bool closing = tag[0] == '/';
            if (closing)
                tag.remove_prefix(1);
            size_t ne = 0;
            while (ne < tag.size() && tag[ne] != ' ' && tag[ne] != '/' && tag[ne] != '\t' &&
                   tag[ne] != '\n')
                ++ne;
            const std::string_view name = tag.substr(0, ne);
            const bool skipper = ieq(name, "script") || ieq(name, "style") || ieq(name, "head") ||
                                 ieq(name, "title");
            if (skipper) {
                skip += closing ? -1 : 1;
                if (skip < 0)
                    skip = 0;
                continue;
            }
            if (skip)
                continue;
            if (ieq(name, "br")) {
                emit("\n", 0);
                pendingSpace = false;
                continue;
            }
            if (isBlock(name))
                newline();
            if (closing) {
                for (size_t k = stack.size(); k-- > 0;)
                    if (ieqv(stack[k].name, name)) {
                        stack.resize(k);
                        break;
                    }
                continue;
            }
            if (!tag.empty() && tag.back() == '/')
                continue; // self-closing: no content to format
            Open o{name, 0, 0, false};
            if (ieq(name, "b") || ieq(name, "strong"))
                o.formats |= kBold;
            else if (ieq(name, "i") || ieq(name, "em"))
                o.formats |= kItalic;
            else if (ieq(name, "s") || ieq(name, "strike") || ieq(name, "del"))
                o.formats |= kStrike;
            else if (ieq(name, "code") || ieq(name, "tt") || ieq(name, "kbd") || ieq(name, "samp"))
                o.formats |= kCode;
            else if (ieq(name, "pre")) {
                o.formats |= kCode;
                o.pre = true;
            } else if (ieq(name, "a")) {
                std::string href = attr(tag, "href");
                if (!href.empty() && links->size() < 255) {
                    links->push_back(std::move(href));
                    o.link = uint16_t(links->size());
                }
            }
            // Inline styles (Google Docs wraps everything in <b style="font-weight:normal">).
            const std::string st = attr(tag, "style");
            if (!st.empty()) {
                if (icontains(st, "font-weight:normal") || icontains(st, "font-weight: normal") ||
                    icontains(st, "font-weight:400"))
                    o.formats &= uint16_t(~kBold);
                else if (
                    icontains(st, "font-weight:bold") || icontains(st, "font-weight: bold") ||
                    icontains(st, "font-weight:700") || icontains(st, "font-weight:600")
                )
                    o.formats |= kBold;
                if (icontains(st, "italic"))
                    o.formats |= kItalic;
                if (icontains(st, "line-through"))
                    o.formats |= kStrike;
            }
            stack.push_back(o);
            continue;
        }
        // Text up to the next tag.
        size_t e = h.find('<', i);
        if (e == std::string_view::npos)
            e = h.size();
        if (!skip) {
            const std::string t   = decode(h.substr(i, e - i));
            const uint16_t    f   = current();
            const bool        pre = inPre();
            std::string       run;
            for (char c : t) {
                const bool ws = c == ' ' || c == '\t' || c == '\n' || c == '\r';
                if (pre) {
                    if (c != '\r')
                        run.push_back(c);
                } else if (ws) {
                    pendingSpace = true;
                } else {
                    if (pendingSpace && !run.empty()) {
                        run.push_back(' ');
                    } else if (pendingSpace && !text->empty() && text->back() != '\n') {
                        // A space collapsed across a tag takes the format of
                        // the text before it ("one <b>two</b>": the space is plain).
                        emit(" ", fmt->back());
                    }
                    pendingSpace = false;
                    run.push_back(c);
                }
            }
            emit(run, f);
        }
        i = e;
    }
    while (!text->empty() && (text->back() == '\n' || text->back() == ' ')) {
        text->pop_back();
        fmt->pop_back();
    }
}

} // namespace ui::rich
