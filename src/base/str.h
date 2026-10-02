// Small string helpers. concat() exists for size: every `a + b + c` on
// std::string inlines its own allocation and copy sequence at the call site,
// and error-message building repeats that dozens of times per file.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

namespace str {

// One allocation, one out-of-line function: concat({"user ", id, ": bad tz"}).
std::string concat(std::initializer_list<std::string_view> parts);
std::string number(int64_t n); // decimal

inline bool startsWith(std::string_view s, std::string_view p) {
    return s.substr(0, p.size()) == p;
}
inline bool endsWith(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && s.substr(s.size() - p.size()) == p;
}
// ASCII whitespace (space, tab, CR, LF) off both ends.
std::string_view trim(std::string_view s);
// ASCII-only case changes (identifiers, extensions, hosts); for user text
// see utf8::foldCase.
std::string      asciiLower(std::string_view s);
std::string      asciiUpper(std::string_view s);

} // namespace str
