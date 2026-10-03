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

// Bonus kinds, so a prepared name keeps a byte per character.
enum Bonus : uint8_t { kNone, kStart, kWord, kCamel, kDot };
constexpr double kBonus[] = {0.0, kMatchStart, kMatchWord, kMatchCamel, kMatchDot};

} // namespace

FuzzyText::FuzzyText(std::string_view haystack) {
    std::vector<uint32_t> orig;
    orig.reserve(haystack.size());
    for (size_t i = 0; i < haystack.size();)
        orig.push_back(utf8::decode(haystack, i));
    folded.resize(orig.size());
    for (size_t j = 0; j < orig.size(); ++j)
        folded[j] = utf8::foldCase(orig[j]);
    if (orig.size() > kMaxHaystack)
        return; // never ranked: no bonuses needed
    bonus.resize(orig.size());
    for (size_t j = 0; j < orig.size(); ++j) {
        if (j == 0) {
            bonus[j] = kStart;
            continue;
        }
        const uint32_t prev = orig[j - 1], cur = orig[j];
        bonus[j] = prev == '.'                     ? kDot
                   : isSeparator(prev)             ? kWord
                   : isLower(prev) && isUpper(cur) ? kCamel
                                                   : kNone;
    }
}

std::vector<uint32_t> fuzzyQuery(std::string_view query) {
    std::vector<uint32_t> q;
    q.reserve(query.size());
    for (size_t i = 0; i < query.size();)
        q.push_back(utf8::foldCase(utf8::decode(query, i)));
    return q;
}

std::optional<double> fuzzyScore(std::string_view query, std::string_view haystack) {
    return fuzzyScore(fuzzyQuery(query), FuzzyText(haystack));
}

std::optional<double> fuzzyScore(const std::vector<uint32_t> &q, const FuzzyText &hay) {
    const std::vector<uint32_t> &h = hay.folded;
    if (q.empty())
        return 0.0;
    if (h.empty())
        return std::nullopt;
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

    const size_t                     n = q.size(), m = h.size();
    // M[j]: best score with q[i] matched exactly at h[j].
    // D[j]: best score with q[0..i] consumed somewhere within h[0..j].
    // Kept between calls: a pick-list scores many names per keystroke.
    thread_local std::vector<double> prevM, prevD, curM, curD;
    prevM.assign(m, kMin);
    prevD.assign(m, kMin);
    curM.resize(m);
    curD.resize(m);
    for (size_t i = 0; i < n; ++i) {
        const double gap       = i == n - 1 ? kGapTrailing : kGapInner;
        double       prevScore = kMin;
        for (size_t j = 0; j < m; ++j) {
            if (q[i] == h[j]) {
                const double bonus = kBonus[hay.bonus[j]];
                double       s     = kMin;
                if (i == 0)
                    s = double(j) * kGapLeading + bonus;
                else if (j > 0)
                    s = std::max(prevD[j - 1] + bonus, prevM[j - 1] + kMatchConsecutive);
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
