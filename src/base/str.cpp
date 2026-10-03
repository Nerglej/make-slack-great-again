#include "base/str.h"

#include "base/utf8.h"

#include <algorithm>
#include <charconv>
#include <cstdio>

namespace str {

std::string concat(std::initializer_list<std::string_view> parts) {
    size_t n = 0;
    for (auto p : parts)
        n += p.size();
    std::string out;
    out.reserve(n);
    for (auto p : parts)
        out.append(p);
    return out;
}

std::string number(int64_t n) {
    char buf[24];
    auto r = std::to_chars(buf, buf + sizeof buf, n);
    return std::string(buf, r.ptr);
}

std::string_view trim(std::string_view s) {
    auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && ws(s.front()))
        s.remove_prefix(1);
    while (!s.empty() && ws(s.back()))
        s.remove_suffix(1);
    return s;
}

std::string_view trimSpace(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e) {
        size_t i = b;
        if (!utf8::isSpace(utf8::decode(s, i)))
            break;
        b = i;
    }
    while (e > b) {
        size_t p = utf8::prevBoundary(s, e), i = p;
        if (!utf8::isSpace(utf8::decode(s, i)))
            break;
        e = p;
    }
    return s.substr(b, e - b);
}

std::string asciiLower(std::string_view s) {
    std::string out(s);
    for (auto &c : out)
        c = asciiLower(c);
    return out;
}

std::string asciiUpper(std::string_view s) {
    std::string out(s);
    for (auto &c : out)
        if (c >= 'a' && c <= 'z')
            c = char(c - 32);
    return out;
}

std::string simplified(std::string_view s) {
    std::string out;
    bool        gap = false;
    for (size_t i = 0; i < s.size();) {
        const size_t   at = i;
        const uint32_t cp = utf8::decode(s, i);
        if (utf8::isSpace(cp)) {
            gap = !out.empty();
            continue;
        }
        if (gap)
            out += ' ';
        gap = false;
        out.append(s.substr(at, i - at));
    }
    return out;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (asciiLower(a[i]) != asciiLower(b[i]))
            return false;
    return true;
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    c = asciiLower(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

std::vector<std::string_view> split(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    Splitter                      parts(s, sep);
    for (std::string_view p; parts.next(&p);)
        out.push_back(p);
    return out;
}

bool Splitter::next(std::string_view *part) {
    if (_done)
        return false;
    const size_t at = _rest.find(_sep);
    *part           = _rest.substr(0, at);
    if (at == std::string_view::npos)
        _done = true;
    else
        _rest.remove_prefix(at + 1);
    return true;
}

std::string decodeEntities(std::string_view s, bool nbspAsSpace) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        // The ';' is looked for nearby only: a stray '&' never scans the rest.
        const size_t semi = s[i] == '&' ? s.substr(0, i + 11).find(';', i) : std::string_view::npos;
        if (semi == std::string_view::npos) {
            out += s[i];
            continue;
        }
        const std::string_view name = s.substr(i + 1, semi - i - 1);
        uint32_t               cp   = 0;
        if (!name.empty() && name[0] == '#') {
            const bool hex = name.size() > 1 && (name[1] | 0x20) == 'x';
            for (size_t k = hex ? 2 : 1; k < name.size(); ++k) {
                const int d = hex                                ? hexDigit(name[k])
                              : name[k] >= '0' && name[k] <= '9' ? name[k] - '0'
                                                                 : -1;
                if (d < 0)
                    break;
                // Saturate: anything past U+10FFFF encodes as U+FFFD anyway.
                cp = std::min<uint32_t>(cp * (hex ? 16 : 10) + uint32_t(d), 0x110000);
            }
        } else if (iequals(name, "amp"))
            cp = '&';
        else if (iequals(name, "lt"))
            cp = '<';
        else if (iequals(name, "gt"))
            cp = '>';
        else if (iequals(name, "quot"))
            cp = '"';
        else if (iequals(name, "apos"))
            cp = '\'';
        else if (iequals(name, "nbsp"))
            cp = 0xA0;
        if (cp == 0) {
            out += '&';
            continue;
        }
        utf8::append(out, nbspAsSpace && cp == 0xA0 ? ' ' : cp);
        i = semi;
    }
    return out;
}

void appendEscapedHtml(std::string *out, std::string_view s, bool quotes) {
    for (const char c : s) {
        switch (c) {
        case '&':
            *out += "&amp;";
            break;
        case '<':
            *out += "&lt;";
            break;
        case '>':
            *out += "&gt;";
            break;
        case '"':
            *out += quotes ? "&quot;" : "\"";
            break;
        default:
            *out += c;
        }
    }
}

std::string escapeHtml(std::string_view s, bool quotes) {
    std::string out;
    out.reserve(s.size());
    appendEscapedHtml(&out, s, quotes);
    return out;
}

std::string percentEncode(std::string_view s, std::string_view keep) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string       out;
    out.reserve(s.size());
    for (const char ch : s) {
        const auto c = uint8_t(ch);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '.' || c == '_' || c == '~' ||
            keep.find(ch) != std::string_view::npos) {
            out += ch;
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
    }
    return out;
}

std::string percentDecode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        int hi = -1, lo = -1;
        if (s[i] == '%' && i + 2 < s.size() && (hi = hexDigit(s[i + 1])) >= 0 &&
            (lo = hexDigit(s[i + 2])) >= 0) {
            out += char(hi << 4 | lo);
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string byteSize(int64_t n, ByteSize style) {
    constexpr int64_t kKB = 1024, kMB = kKB * 1024, kGB = kMB * 1024;
    char              buf[32];
    if (style == ByteSize::File && n <= 0)
        return {};
    if (n < kKB) {
        std::snprintf(buf, sizeof buf, "%lld B", (long long)n);
    } else if (n < kMB) {
        const int64_t kb = (style == ByteSize::Exact ? n + kKB / 2 : n) / kKB;
        std::snprintf(buf, sizeof buf, "%lld KB", (long long)kb);
    } else if (style == ByteSize::Whole) {
        std::snprintf(buf, sizeof buf, "%lld MB", (long long)(n / kMB));
    } else if (style == ByteSize::Exact && n >= kGB) {
        std::snprintf(buf, sizeof buf, "%.1f GB", double(n) / double(kGB));
    } else {
        const double mb    = double(n) / double(kMB);
        const bool   whole = style == ByteSize::File && mb >= 10;
        std::snprintf(buf, sizeof buf, whole ? "%.0f MB" : "%.1f MB", mb);
    }
    return buf;
}

} // namespace str
