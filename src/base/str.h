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
// ASCII case-insensitive equality (header names, tokens).
bool             iequals(std::string_view a, std::string_view b);
// Every run of Unicode whitespace (utf8::isSpace) becomes
// one space, none at the ends.
std::string      simplified(std::string_view s);

// HTML/XML character references: &amp; &lt; &gt; &quot; &apos; &nbsp; (any
// case) and &#NN; / &#xHH;. Anything else, a NUL reference, or a ';' more than
// ten bytes away stays literal. With nbspAsSpace, U+00A0 written as a
// reference comes out as a plain space (what a clipboard reader wants).
std::string decodeEntities(std::string_view s, bool nbspAsSpace = false);

// File sizes in binary units (1 KB = 1024 B), in one of these styles.
enum class ByteSize : uint8_t {
    Whole, // "812 B", "12 KB", "3 MB": all floored (composer chips, Settings)
    File,  // floored KB, MB with one decimal below 10: "1.4 MB", "23 MB";
           // "" for 0 or less, an unknown size (message file chips)
    Exact, // rounded KB, one decimal for MB and GB: "1.4 MB", "2.0 GB" (file browser)
};
std::string byteSize(int64_t bytes, ByteSize style = ByteSize::Whole);

} // namespace str
