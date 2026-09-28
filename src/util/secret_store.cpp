// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
//
// Cross-platform part of SecretStore: the read-with-migration helper. The
// read/write/remove/isKeychainBacked primitives are provided per-platform
// (secret_store_mac.mm on macOS, secret_store_qsettings.cpp elsewhere).
#include "util/secret_store.h"

#include <QDebug>
#include <QSettings>

using namespace Qt::StringLiterals;

namespace SecretStore {

namespace {
// Set the first time the OS keychain refuses a write this process. Latches so a
// locked keychain does not turn into a prompt on every read/write.
bool g_keychainRefused = false;
} // namespace

QString readMigrating(const QString &key) {
    const QString v = read(key);
    // Already in the keychain, or the fallback store *is* QSettings(key) — either
    // way there is nothing to migrate.
    if (!v.isEmpty() || !isKeychainBacked())
        return v;

    // Keychain miss: pull a pre-keychain plaintext value forward, once.
    QSettings     s(u"msga"_s, u"msga"_s);
    const QString legacy = s.value(key).toString();
    if (legacy.isEmpty())
        return QString();
    // Once the keychain has refused us this process, stop knocking: every
    // retry can raise another unlock/allow prompt, and the plaintext copy
    // works fine. The promotion is retried on the next launch.
    if (g_keychainRefused)
        return legacy;
    if (!write(key, legacy)) {
        // The keychain refused (locked, prompt denied). The plaintext copy is
        // the only one there is — keep it.
        g_keychainRefused = true;
        qWarning() << "[SecretStore] could not promote" << key
                   << "into the keychain; leaving the plaintext copy in place";
        return legacy;
    }
    s.remove(key); // promoted — now scrub the plaintext copy
    return legacy;
}

bool writeScrubbingLegacy(const QString &key, const QString &value) {
    if (isKeychainBacked() && g_keychainRefused && !value.isEmpty()) {
        // Keychain already refused this process — go straight to the fallback.
        QSettings(u"msga"_s, u"msga"_s).setValue(key, value);
        return false;
    }
    if (!write(key, value)) {
        if (isKeychainBacked()) {
            // The keychain refused (locked, non-default keychain, prompt denied).
            // Losing the credential here would leave the workspace registered
            // with no auth — every request fails "not_authed" and the UI spins
            // forever. Fall back to the plaintext QSettings copy, exactly where
            // readMigrating() looks for a legacy value, so the session works
            // and the promotion is retried on the next launch.
            g_keychainRefused = true;
            QSettings s(u"msga"_s, u"msga"_s);
            if (value.isEmpty())
                s.remove(key);
            else
                s.setValue(key, value);
            qWarning() << "[SecretStore] keychain write failed for" << key
                       << "— stored the plaintext fallback copy instead";
        } else {
            qWarning() << "[SecretStore] write failed for" << key;
        }
        return false;
    }
    if (isKeychainBacked())
        QSettings(u"msga"_s, u"msga"_s).remove(key);
    return true;
}

} // namespace SecretStore
