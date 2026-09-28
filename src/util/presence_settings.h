// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// The global "how should msga hold my presence" preference (Settings → System →
// Presence). One value for every workspace; Session::start() applies it and
// MainWindow re-applies it to every live session when it changes.
#pragma once

#include "backend/domain.h"

#include <QSettings>
#include <QString>

namespace PresenceSettings {

inline constexpr char kKey[] = "presence/mode";

// Default: active for as long as msga runs — the whole point of the feature (see
// GitHub issue #69: without it a msga-only user is away to everyone).
inline PresenceMode mode() {
    const QString v = QSettings(QStringLiteral("msga"), QStringLiteral("msga"))
                          .value(QLatin1String(kKey), QStringLiteral("running"))
                          .toString();
    if (v == QLatin1String("native"))
        return PresenceMode::Native;
    if (v == QLatin1String("using"))
        return PresenceMode::WhileUsing;
    return PresenceMode::WhileRunning;
}

inline void setMode(PresenceMode m) {
    const char *v = m == PresenceMode::Native       ? "native"
                    : m == PresenceMode::WhileUsing ? "using"
                                                    : "running";
    QSettings(QStringLiteral("msga"), QStringLiteral("msga"))
        .setValue(QLatin1String(kKey), QLatin1String(v));
}

} // namespace PresenceSettings
