// The old Qt msga's settings store — QSettings("msga", "msga") — read and
// written in Qt's own encodings, so the two apps share one store:
//
//   Linux     $XDG_CONFIG_HOME/msga/msga.conf, Qt's INI dialect
//   macOS     CFPreferences domain "com.msga.msga" (~/Library/Preferences/
//             com.msga.msga.plist); a Qt key's '/' is '.' there
//   Windows   HKEY_CURRENT_USER\Software\msga\msga, one subkey per '/'
//
// The new app reads it once to import the old settings (the shell's
// importer) and, on Linux and Windows, keeps its credentials in it exactly as
// the old app did (secret.h): the old app had no keychain there, and a
// rolled-back install must still find refreshed tokens. Keys are Qt's
// ("workspace/slack:T0/auth"), values UTF-8.
//
// `app` names the store: "msga" is QSettings("msga", "msga"), the one the old
// app used everywhere; "MSGA" is a bare QSettings() (its application name),
// where only composer/lastAttachDir ever went.
//
// A test process (base::testProcess(), which the test harness marks) uses
// the INI file on every OS, under the XDG_CONFIG_HOME the harness points at
// a temporary directory: no test reaches CFPreferences or the registry. Only
// the native backends' own tests do (detail::nativeInTests), in the domain
// "com.msga-tests.<app>" / key "Software\msga-tests\<app>", and remove
// what they wrote.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace oldsettings {

// A QVariant as QSettings stores it.
struct Value {
    enum class Kind : uint8_t { None, String, List, Bytes, Int, Bool };
    Kind                     kind = Kind::None;
    std::string              s;     // String (UTF-8), Bytes (raw)
    std::vector<std::string> list;  // List
    int64_t                  n = 0; // Int; Bool as 0/1

    // QVariant::toString / toBool / toLongLong / toStringList.
    std::string              text() const;
    bool                     toBool(bool def) const;
    int64_t                  toInt(int64_t def) const;
    std::vector<std::string> toList() const;
};
using Map = std::map<std::string, Value, std::less<>>;

// Every key of the store; empty when there is none.
Map   load(std::string_view app = "msga");
// One key (Kind::None when absent).
Value get(std::string_view key, std::string_view app = "msga");
// Sets a string value (as QSettings::setValue(key, QString)), or removes the
// key. False when the store could not be written.
bool  write(std::string_view key, std::string_view value, std::string_view app = "msga");
bool  remove(std::string_view key, std::string_view app = "msga");

// ── Qt's encodings (pure; the platform code above and the tests use them) ──
// QSettingsPrivate::stringToVariant: "@ByteArray(…)", "@@…", "@Invalid()".
Value       decodeString(std::string s);
// QSettingsPrivate::variantToString for a QString ("@…" gets another '@').
std::string encodeString(std::string_view s);
// A whole INI file as QSettings reads it.
Map         parseIni(std::string_view text);
// `text` with `key` set to the string `value` (or removed when null). Every
// other line stays as it was.
std::string setIniValue(std::string_view text, std::string_view key, const std::string *value);
// A registry value (REG_SZ, REG_MULTI_SZ, REG_BINARY = UTF-16LE bytes,
// REG_DWORD, REG_QWORD) as QSettings reads it.
Value       decodeRegistry(uint32_t type, std::string_view bytes);
// "$XDG_CONFIG_HOME/msga/<app>.conf" (Linux's store, and every OS's in
// tests; "" without a home).
std::string iniPath(std::string_view app = "msga");

namespace detail {
// Tests only, macOS and Windows: true sends load/get/write/remove to the
// native store (its test domain) instead of the tests' INI file.
extern bool nativeInTests;
} // namespace detail

} // namespace oldsettings
