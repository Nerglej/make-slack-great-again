#include "core/strings.h"

#include <algorithm>

namespace plat::core {

namespace {

bool isAlpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

} // namespace

char asciiLower(char c) {
    return c >= 'A' && c <= 'Z' ? char(c | 0x20) : c;
}

std::string asciiLower(std::string_view s) {
    std::string out(s);
    for (char &c : out)
        c = asciiLower(c);
    return out;
}

std::string_view trim(std::string_view s, std::string_view chars) {
    const size_t b = s.find_first_not_of(chars);
    if (b == std::string_view::npos)
        return {};
    return s.substr(b, s.find_last_not_of(chars) - b + 1);
}

std::string escapeMarkup(std::string_view s, bool quotes) {
    std::string out;
    out.reserve(s.size());
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
            out += quotes ? "&quot;" : "\"";
            break;
        default:
            out += c;
        }
    }
    return out;
}

bool validScheme(std::string_view s) {
    if (s.empty() || !isAlpha(s[0]))
        return false;
    return std::all_of(s.begin(), s.end(), [](char c) {
        return isAlpha(c) || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
    });
}

void rememberScheme(std::vector<std::string> &schemes, std::string_view scheme) {
    std::string s = asciiLower(scheme);
    if (std::find(schemes.begin(), schemes.end(), s) == schemes.end())
        schemes.push_back(std::move(s));
}

bool isSchemeUrl(std::string_view arg, const std::vector<std::string> &schemes) {
    const size_t colon = arg.find(':');
    if (colon == std::string_view::npos || colon == 0)
        return false;
    const std::string s = asciiLower(arg.substr(0, colon));
    return std::find(schemes.begin(), schemes.end(), s) != schemes.end();
}

std::vector<std::string>
schemeUrls(const std::vector<std::string> &args, const std::vector<std::string> &schemes) {
    std::vector<std::string> out;
    for (const auto &a : args)
        if (isSchemeUrl(a, schemes))
            out.push_back(a);
    return out;
}

} // namespace plat::core
