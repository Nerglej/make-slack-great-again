// secret.h where the old app had no keychain backend (Linux, Windows): the
// credentials live in its settings store, under the same keys, exactly as
// its secret_store_qsettings.cpp kept them (old_settings.h) — so tokens the
// old app wrote are read directly, and a rolled-back install finds the ones
// this app refreshed. Not encryption at rest; the old app's documented
// limitation, kept for compatibility.
#include "base/old_settings.h"
#include "base/secret.h"

namespace secret {

bool available() {
    return true;
}

bool write(std::string_view key, std::string_view value) {
    return value.empty() ? oldsettings::remove(key) : oldsettings::write(key, value);
}

std::string read(std::string_view key, bool *found) {
    const oldsettings::Value v = oldsettings::get(key);
    if (found)
        *found = v.kind != oldsettings::Value::Kind::None;
    return v.text();
}

void remove(std::string_view key) {
    oldsettings::remove(key);
}

} // namespace secret
