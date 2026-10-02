#include "app/model/image_size.h"

#include <cstdio>
#include <cstring>

namespace model {

namespace {

uint32_t be16(const unsigned char *p) {
    return uint32_t(p[0]) << 8 | p[1];
}
uint32_t be32(const unsigned char *p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
uint32_t le16(const unsigned char *p) {
    return uint32_t(p[1]) << 8 | p[0];
}
uint32_t le24(const unsigned char *p) {
    return uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
}

} // namespace

bool imageSize(const std::string &path, int32_t *w, int32_t *h) {
    *w = *h = 0;
    std::string head;
    if (FILE *f = std::fopen(path.c_str(), "rb")) {
        head.resize(256 * 1024);
        head.resize(std::fread(head.data(), 1, head.size(), f));
        std::fclose(f);
    }
    const auto  *p = reinterpret_cast<const unsigned char *>(head.data());
    const size_t n = head.size();
    if (n >= 24 && std::memcmp(p, "\x89PNG\r\n\x1a\n", 8) == 0 &&
        std::memcmp(p + 12, "IHDR", 4) == 0) {
        *w = int32_t(be32(p + 16));
        *h = int32_t(be32(p + 20));
    } else if (n >= 10 && std::memcmp(p, "GIF8", 4) == 0) {
        *w = int32_t(le16(p + 6));
        *h = int32_t(le16(p + 8));
    } else if (n >= 30 && std::memcmp(p, "RIFF", 4) == 0 && std::memcmp(p + 8, "WEBP", 4) == 0) {
        if (std::memcmp(p + 12, "VP8X", 4) == 0) {
            *w = int32_t(le24(p + 24) + 1);
            *h = int32_t(le24(p + 27) + 1);
        } else if (std::memcmp(p + 12, "VP8L", 4) == 0 && n >= 25) {
            const uint32_t b = uint32_t(p[21]) | uint32_t(p[22]) << 8 | uint32_t(p[23]) << 16 |
                               uint32_t(p[24]) << 24;
            *w               = int32_t((b & 0x3FFF) + 1);
            *h               = int32_t(((b >> 14) & 0x3FFF) + 1);
        } else if (std::memcmp(p + 12, "VP8 ", 4) == 0) {
            *w = int32_t(le16(p + 26) & 0x3FFF);
            *h = int32_t(le16(p + 28) & 0x3FFF);
        }
    } else if (n >= 4 && p[0] == 0xFF && p[1] == 0xD8) {
        // Walk the JPEG segments to the first start-of-frame.
        size_t i = 2;
        while (i + 9 < n) {
            if (p[i] != 0xFF) {
                ++i;
                continue;
            }
            const unsigned marker = p[i + 1];
            if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7) ||
                marker == 0xFF) {
                i += marker == 0xFF ? 1 : 2;
                continue;
            }
            const uint32_t len = be16(p + i + 2);
            if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 &&
                marker != 0xCC) {
                *h = int32_t(be16(p + i + 5));
                *w = int32_t(be16(p + i + 7));
                break;
            }
            i += 2 + len;
        }
    }
    return *w > 0 && *h > 0;
}

} // namespace model
