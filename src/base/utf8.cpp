#include "base/utf8.h"

namespace utf8 {

uint32_t decode(std::string_view s, size_t &i) {
    const size_t n = s.size();
    if (i >= n)
        return kReplacement;
    const uint8_t b0 = uint8_t(s[i]);
    if (b0 < 0x80) {
        ++i;
        return b0;
    }
    int      len;
    uint32_t cp, min;
    if ((b0 & 0xE0) == 0xC0) {
        len = 2, cp = b0 & 0x1F, min = 0x80;
    } else if ((b0 & 0xF0) == 0xE0) {
        len = 3, cp = b0 & 0x0F, min = 0x800;
    } else if ((b0 & 0xF8) == 0xF0) {
        len = 4, cp = b0 & 0x07, min = 0x10000;
    } else {
        ++i; // stray continuation or 0xF8+
        return kReplacement;
    }
    if (i + len > n) {
        ++i;
        return kReplacement;
    }
    for (int k = 1; k < len; ++k) {
        const uint8_t b = uint8_t(s[i + k]);
        if ((b & 0xC0) != 0x80) {
            ++i;
            return kReplacement;
        }
        cp = (cp << 6) | (b & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        ++i;
        return kReplacement;
    }
    i += len;
    return cp;
}

int encodedLength(uint32_t cp) {
    if (cp < 0x80)
        return 1;
    if (cp < 0x800)
        return 2;
    if (cp < 0x10000)
        return 3;
    return cp <= 0x10FFFF ? 4 : 3;
}

size_t encode(char *out, uint32_t cp) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        cp = kReplacement;
    if (cp < 0x80) {
        out[0] = char(cp);
        return 1;
    }
    if (cp < 0x800) {
        out[0] = char(0xC0 | (cp >> 6));
        out[1] = char(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = char(0xE0 | (cp >> 12));
        out[1] = char(0x80 | ((cp >> 6) & 0x3F));
        out[2] = char(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = char(0xF0 | (cp >> 18));
    out[1] = char(0x80 | ((cp >> 12) & 0x3F));
    out[2] = char(0x80 | ((cp >> 6) & 0x3F));
    out[3] = char(0x80 | (cp & 0x3F));
    return 4;
}

void append(std::string &out, uint32_t cp) {
    char b[4];
    out.append(b, encode(b, cp));
}

// A decoded U+FFFD is only an error when the input did not literally hold
// EF BF BD — the one thing decode() cannot tell its caller.
static bool isLiteralReplacement(std::string_view s, size_t start, size_t end) {
    return end - start == 3 && uint8_t(s[start]) == 0xEF;
}

bool isValid(std::string_view s) {
    for (size_t i = 0; i < s.size();) {
        if (uint8_t(s[i]) < 0x80) {
            ++i;
            continue;
        }
        const size_t start = i;
        if (decode(s, i) == kReplacement && !isLiteralReplacement(s, start, i))
            return false;
    }
    return true;
}

std::string sanitize(std::string_view s) {
    if (isValid(s))
        return std::string(s);
    std::string out;
    out.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size();) {
        const size_t   start = i;
        const uint32_t cp    = decode(s, i);
        if (cp == kReplacement && !isLiteralReplacement(s, start, i))
            append(out, kReplacement);
        else
            out.append(s.substr(start, i - start));
    }
    return out;
}

size_t countCodePoints(std::string_view s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size(); ++n)
        decode(s, i);
    return n;
}

size_t nextBoundary(std::string_view s, size_t i) {
    if (i >= s.size())
        return s.size();
    decode(s, i);
    return i;
}

size_t prevBoundary(std::string_view s, size_t i) {
    if (i == 0)
        return 0;
    if (i > s.size())
        return s.size();
    // Step back over at most three continuation bytes, then verify the lead
    // really spans up to i (otherwise the byte before i stands alone).
    size_t j = i - 1;
    for (int k = 0; k < 3 && j > 0 && (uint8_t(s[j]) & 0xC0) == 0x80; ++k)
        --j;
    size_t probe = j;
    decode(s, probe);
    return probe == i ? j : i - 1;
}

size_t truncateAt(std::string_view s, size_t maxBytes) {
    if (maxBytes >= s.size())
        return s.size();
    // Only a sequence that starts up to three bytes back can span maxBytes;
    // a byte that isn't a continuation byte always starts one.
    size_t lead = maxBytes;
    for (int k = 0; k < 3 && lead > 0 && (uint8_t(s[lead]) & 0xC0) == 0x80; ++k)
        --lead;
    if ((uint8_t(s[lead]) & 0xC0) == 0x80)
        return maxBytes; // a stray continuation byte: a sequence of its own
    size_t end = lead;
    decode(s, end);
    return end > maxBytes ? lead : maxBytes;
}

bool isSpace(uint32_t cp) {
    return (cp >= 9 && cp <= 13) || cp == 32 || cp == 0x85 || cp == 0xA0 || cp == 0x1680 ||
           (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 || cp == 0x202F ||
           cp == 0x205F || cp == 0x3000;
}

bool isDigit(uint32_t cp) {
    return cp >= '0' && cp <= '9';
}

bool isWordChar(uint32_t cp) {
    if (cp < 0x80)
        return (cp >= '0' && cp <= '9') || ((cp | 0x20) >= 'a' && (cp | 0x20) <= 'z');
    // Non-letter blocks. Everything else above ASCII (Latin-1 letters, Greek,
    // Cyrillic, CJK, …) is treated as a letter.
    if (cp < 0xC0)
        return cp == 0xAA || cp == 0xB5 || cp == 0xBA; // ª µ º
    if (cp == 0xD7 || cp == 0xF7)
        return false; // × ÷
    if (isSpace(cp))
        return false;
    if (cp >= 0x2000 && cp <= 0x2BFF) // punctuation, symbols, arrows, dingbats
        return false;
    if (cp >= 0x3000 && cp <= 0x303F) // CJK punctuation
        return false;
    if ((cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xFF00 && cp <= 0xFF0F) ||
        (cp >= 0xFF1A && cp <= 0xFF20))
        return false;
    if (cp >= 0x1F000 && cp <= 0x1FAFF) // emoji and pictographs
        return false;
    return true;
}

uint32_t foldCase(uint32_t cp) {
    if (cp < 0x80)
        return (cp >= 'A' && cp <= 'Z') ? cp + 32 : cp;
    if (cp < 0x100)
        return (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) ? cp + 32 : cp;
    if (cp < 0x180) { // Latin Extended-A: mostly even upper / odd lower pairs
        if (cp == 0x130)
            return 'i';
        if (cp == 0x178)
            return 0xFF;
        if (cp == 0x17F)
            return 's';
        if ((cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E))
            return (cp & 1) ? cp + 1 : cp;
        if (cp == 0x131 || cp == 0x138 || cp == 0x149)
            return cp;
        return cp | 1;
    }
    if (cp >= 0x391 && cp <= 0x3AB && cp != 0x3A2)
        return cp + 32; // Greek capitals
    if (cp == 0x3C2)
        return 0x3C3; // final sigma
    if (cp >= 0x410 && cp <= 0x42F)
        return cp + 32; // Cyrillic А-Я
    if (cp >= 0x400 && cp <= 0x40F)
        return cp + 80; // Ѐ-Џ
    if ((cp >= 0x460 && cp <= 0x481) || (cp >= 0x48A && cp <= 0x4BF))
        return cp | 1;
    if (cp >= 0x531 && cp <= 0x556)
        return cp + 48; // Armenian
    if (cp >= 0x1E00 && cp <= 0x1EFF && !(cp >= 0x1E96 && cp <= 0x1E9F))
        return cp | 1; // Latin Extended Additional (Vietnamese)
    if (cp >= 0xFF21 && cp <= 0xFF3A)
        return cp + 32; // fullwidth A-Z
    return cp;
}

std::string foldCase(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (uint8_t(s[i]) < 0x80) { // fast path
            const char c = s[i++];
            out += (c >= 'A' && c <= 'Z') ? char(c + 32) : c;
            continue;
        }
        append(out, foldCase(decode(s, i)));
    }
    return out;
}

bool containsFolded(std::string_view haystack, std::string_view needle) {
    return needle.empty() || containsFoldedNeedle(haystack, foldCase(needle));
}

bool containsFoldedNeedle(std::string_view haystack, std::string_view n) {
    if (n.empty())
        return true;
    for (size_t start = 0; start < haystack.size(); start = nextBoundary(haystack, start)) {
        size_t h = start, k = 0;
        bool   match = true;
        while (k < n.size()) {
            if (h >= haystack.size() || foldCase(decode(haystack, h)) != decode(n, k)) {
                match = false;
                break;
            }
        }
        if (match)
            return true;
    }
    return false;
}

bool containsPrefolded(std::string_view h, std::string_view n) {
    if (n.empty())
        return true;
    if (h.find(n) == std::string_view::npos) // the usual answer, without segmenting
        return false;
    for (size_t start = 0; start < h.size(); start = nextBoundary(h, start))
        if (h.compare(start, n.size(), n) == 0)
            return true;
    return false;
}

} // namespace utf8
