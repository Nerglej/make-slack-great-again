#include "core/hash.h"

namespace plat::core {

uint64_t fnv1a(const void *data, size_t n) {
    uint64_t    h = 1469598103934665603ull;
    const auto *p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < n; ++i)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

uint64_t fnv1a(std::string_view data) {
    return fnv1a(data.data(), data.size());
}

} // namespace plat::core
