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

std::string asciiLower(std::string_view s) {
    std::string out(s);
    for (auto &c : out)
        if (c >= 'A' && c <= 'Z')
            c = char(c + 32);
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
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z')
            x = char(x + 32);
        if (y >= 'A' && y <= 'Z')
            y = char(y + 32);
        if (x != y)
            return false;
    }
    return true;
}

std::string decodeEntities(std::string_view s, bool nbspAsSpace) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const size_t semi = s[i] == '&' ? s.find(';', i) : std::string_view::npos;
        if (semi == std::string_view::npos || semi - i > 10) {
            out += s[i];
            continue;
        }
        const std::string_view name = s.substr(i + 1, semi - i - 1);
        uint32_t               cp   = 0;
        if (!name.empty() && name[0] == '#') {
            const bool hex = name.size() > 1 && (name[1] | 0x20) == 'x';
            for (size_t k = hex ? 2 : 1; k < name.size(); ++k) {
                const char c = name[k];
                const int  d = c >= '0' && c <= '9' ? c - '0'
                               : hex && (c | 0x20) >= 'a' && (c | 0x20) <= 'f'
                                   ? (c | 0x20) - 'a' + 10
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
