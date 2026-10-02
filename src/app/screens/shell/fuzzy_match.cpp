#include "screens/shell/fuzzy_match.h"

#include "base/utf8.h"

#include <algorithm>
#include <limits>
#include <vector>

namespace shell {

namespace {

// Weights borrowed from fzy: a consecutive match (1.0) outranks any single
// boundary bonus, so a verbatim substring always beats a scattered alignment
// of the same characters; gap penalties are two orders smaller so they only
// break ties between otherwise equal alignments. Unlike fzy, characters left
// over AFTER the last match cost nothing: "des" scores "design-review" and
// "design-backend" the same, so the caller's own order decides, instead of
// the shorter name always winning. Typing the whole name still puts that
// conversation first via kExact.
constexpr double kMin              = -std::numeric_limits<double>::infinity();
constexpr double kGapLeading       = -0.005;
constexpr double kGapTrailing      = 0.0;
constexpr double kGapInner         = -0.01;
constexpr double kExact            = 1.0; // query == whole haystack
constexpr double kMatchConsecutive = 1.0;
constexpr double kMatchStart       = 0.9; // first character of the haystack
constexpr double kMatchWord        = 0.8; // after a separator
constexpr double kMatchCamel       = 0.7; // lower→Upper transition
constexpr double kMatchDot         = 0.6; // after '.'

// Beyond this the O(n·m) table is not worth it for a pick-list.
constexpr size_t kMaxHaystack = 512;

bool isSeparator(uint32_t c) {
    switch (c) {
    case ' ':
    case '-':
    case '_':
    case ',':
    case '/':
    case '\\':
    case '@':
    case '#':
    case ':':
    case '(':
    case ')':
    case '[':
    case ']':
        return true;
    default:
        // Whitespace and punctuation (ASCII exactly, else "not a word char").
        if (utf8::isSpace(c))
            return true;
        if (c < 0x80)
            return (c >= 0x21 && c <= 0x2f) || (c >= 0x3a && c <= 0x40) ||
                   (c >= 0x5b && c <= 0x60) || (c >= 0x7b && c <= 0x7e);
        return !utf8::isWordChar(c);
    }
}

bool isUpper(uint32_t c) {
    return utf8::foldCase(c) != c;
}

bool isLower(uint32_t c) {
    return !isUpper(c) && utf8::isWordChar(c) && !utf8::isDigit(c);
}

std::vector<uint32_t> codePoints(std::string_view s) {
    std::vector<uint32_t> out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();)
        out.push_back(utf8::decode(s, i));
    return out;
}

} // namespace

std::optional<double> fuzzyScore(std::string_view query, std::string_view haystack) {
    if (query.empty())
        return 0.0;
    if (haystack.empty())
        return std::nullopt;
    const std::vector<uint32_t> orig = codePoints(haystack);
    std::vector<uint32_t>       q    = codePoints(query), h(orig.size());
    for (uint32_t &c : q)
        c = utf8::foldCase(c);
    for (size_t j = 0; j < orig.size(); ++j)
        h[j] = utf8::foldCase(orig[j]);
    if (q.size() > h.size())
        return std::nullopt;
    // Cheap gate: most candidates fail here and never reach the table.
    size_t qi = 0;
    for (size_t hi = 0; hi < h.size() && qi < q.size(); ++hi)
        if (h[hi] == q[qi])
            ++qi;
    if (qi != q.size())
        return std::nullopt;
    if (h.size() > kMaxHaystack)
        return 0.0; // matched, but not worth ranking

    const size_t        n = q.size(), m = h.size();
    std::vector<double> bonus(m);
    for (size_t j = 0; j < m; ++j) {
        if (j == 0) {
            bonus[j] = kMatchStart;
            continue;
        }
        const uint32_t prev = orig[j - 1], cur = orig[j];
        bonus[j] = prev == '.'                     ? kMatchDot
                   : isSeparator(prev)             ? kMatchWord
                   : isLower(prev) && isUpper(cur) ? kMatchCamel
                                                   : 0.0;
    }
    // M[j]: best score with q[i] matched exactly at h[j].
    // D[j]: best score with q[0..i] consumed somewhere within h[0..j].
    std::vector<double> prevM(m, kMin), prevD(m, kMin), curM(m), curD(m);
    for (size_t i = 0; i < n; ++i) {
        const double gap       = i == n - 1 ? kGapTrailing : kGapInner;
        double       prevScore = kMin;
        for (size_t j = 0; j < m; ++j) {
            if (q[i] == h[j]) {
                double s = kMin;
                if (i == 0)
                    s = double(j) * kGapLeading + bonus[j];
                else if (j > 0)
                    s = std::max(prevD[j - 1] + bonus[j], prevM[j - 1] + kMatchConsecutive);
                curM[j]   = s;
                prevScore = std::max(s, prevScore + gap);
                curD[j]   = prevScore;
            } else {
                curM[j]   = kMin;
                prevScore = prevScore + gap;
                curD[j]   = prevScore;
            }
        }
        std::swap(prevM, curM);
        std::swap(prevD, curD);
    }
    return prevD[m - 1] + (n == m ? kExact : 0.0);
}

} // namespace shell
