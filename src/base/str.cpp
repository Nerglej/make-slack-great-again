#include "base/str.h"

#include <charconv>

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

} // namespace str
