// FNV-1a, 64-bit: the backends' allocation-free hash for names that must be
// short (socket paths, pipe names, monitor ids). Not cryptographic. The app
// side has the same function as crypto::fnv1a (plat can't use base), with
// the same basis (a digit short of FNV's published one; see crypto.h): the
// instance socket and pipe names derived from it must not change.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace plat::core {

uint64_t fnv1a(std::string_view data);
uint64_t fnv1a(const void *data, size_t n);

} // namespace plat::core
