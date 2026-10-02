#include "core/transfer.h"

namespace plat::core {

namespace {

char lower(char c) {
    return c >= 'A' && c <= 'Z' ? char(c | 0x20) : c;
}

bool ieq(std::string_view a, std::string_view lowerB) {
    if (a.size() != lowerB.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (lower(a[i]) != lowerB[i])
            return false;
    return true;
}

std::string_view trimSpaces(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.remove_suffix(1);
    return s;
}

int hexValue(char c) {
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                  : -1;
}

} // namespace

bool isTextMime(std::string_view m) {
    if (m == "UTF8_STRING" || m == "STRING" || m == "TEXT")
        return true;
    const size_t semi = m.find(';');
    if (!ieq(trimSpaces(m.substr(0, semi)), "text/plain"))
        return false;
    if (semi == std::string_view::npos)
        return true;
    const std::string_view param = trimSpaces(m.substr(semi + 1));
    const size_t           eq    = param.find('=');
    if (eq == std::string_view::npos || !ieq(trimSpaces(param.substr(0, eq)), "charset"))
        return false;
    std::string_view cs = trimSpaces(param.substr(eq + 1));
    if (cs.size() >= 2 && cs.front() == '"' && cs.back() == '"')
        cs = cs.substr(1, cs.size() - 2);
    return ieq(cs, "utf-8") || ieq(cs, "utf8");
}

std::vector<std::string> parseUriList(std::string_view list) {
    std::vector<std::string> out;
    size_t                   start = 0;
    while (start < list.size()) {
        size_t end = list.find('\n', start);
        if (end == std::string_view::npos)
            end = list.size();
        std::string_view line = list.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\0' || line.back() == ' '))
            line.remove_suffix(1);
        if (!line.empty() && line[0] != '#')
            out.emplace_back(line);
        start = end + 1;
    }
    return out;
}

std::string percentEncode(std::string_view s, std::string_view keep) {
    static const char hex[] = "0123456789ABCDEF";
    std::string       out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '.' || c == '_' || c == '~' ||
            (c && keep.find(char(c)) != std::string_view::npos)) {
            out += char(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string percentDecode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && hexValue(s[i + 1]) >= 0 && hexValue(s[i + 2]) >= 0) {
            out += char(hexValue(s[i + 1]) * 16 + hexValue(s[i + 2]));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string fileUri(std::string_view absPath) {
    return "file://" + percentEncode(absPath, "/");
}

std::string pathFromFileUri(std::string_view uri) {
    constexpr std::string_view scheme = "file://";
    if (uri.substr(0, scheme.size()) != scheme)
        return {};
    uri.remove_prefix(scheme.size());
    if (uri.substr(0, 9) == "localhost")
        uri.remove_prefix(9);
    if (uri.empty() || uri[0] != '/')
        return {}; // file://otherhost/… is not ours to open
    std::string out = percentDecode(uri);
    if (out.find('\0') != std::string::npos)
        return {}; // no path can hold a NUL
    return out;
}

} // namespace plat::core
