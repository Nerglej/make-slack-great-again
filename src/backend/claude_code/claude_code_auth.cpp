// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "claude_code_auth.h"

#include "cc_launcher.h"
#include "cc_roster.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace claude_code {

TokenStore::WorkspaceRecord toRecord(const Credentials &creds) {
    TokenStore::WorkspaceRecord rec;
    rec.key         = WorkspaceKey{kService, kWorkspaceId};
    rec.displayName = QStringLiteral("Claude Code");
    rec.iconUrl     = QStringLiteral("qrc:/claude_code_avatar.png");
    QJsonObject blob;
    blob[QStringLiteral("claudePath")] = creds.claudePath;
    rec.auth                           = QJsonDocument(blob).toJson(QJsonDocument::Compact);
    return rec;
}

Credentials fromRecord(const TokenStore::WorkspaceRecord &rec) {
    const QJsonObject blob = QJsonDocument::fromJson(rec.auth).object();
    Credentials       c;
    c.claudePath = blob.value(QStringLiteral("claudePath")).toString();
    // The CLI may have moved (reinstalled, switched installer) since the
    // workspace was added: fall back to looking again.
    if (c.claudePath.isEmpty() || !QFileInfo(c.claudePath).isExecutable())
        c.claudePath = findClaudeExecutable();
    return c;
}

QString findClaudeExecutable() {
#if defined(Q_OS_WIN)
    const QStringList names = {QStringLiteral("claude.exe"), QStringLiteral("claude.cmd")};
#else
    const QStringList names = {QStringLiteral("claude")};
#endif
    for (const auto &n : names)
        if (const QString p = QStandardPaths::findExecutable(n); !p.isEmpty())
            return p;
    // A GUI app often starts with a thinner PATH than a login shell, so also
    // try where the installers put the binary.
    const QString home  = QDir::homePath();
    QStringList   extra = {
        home + QStringLiteral("/.local/bin"),
        home + QStringLiteral("/.claude/local"),
        home + QStringLiteral("/.npm-global/bin"),
        QStringLiteral("/opt/homebrew/bin"),
        QStringLiteral("/usr/local/bin"),
    };
#if defined(Q_OS_WIN)
    extra << home + QStringLiteral("/AppData/Roaming/npm") << home + QStringLiteral("/.local/bin");
#endif
    for (const auto &n : names)
        if (const QString p = QStandardPaths::findExecutable(n, extra); !p.isEmpty())
            return p;
    return {};
}

QString notInstalledMessage() {
    return QCoreApplication::translate(
        "claude_code",
        "Claude Code isn't installed on this computer (msga can't find the claude command). "
        "Install it (https://code.claude.com/docs/en/setup), run `claude` once in a terminal "
        "to log in, then try again."
    );
}

QString notLoggedInMessage() {
    return QCoreApplication::translate(
        "claude_code",
        "Claude Code isn't logged in on this computer. Run `claude` in a terminal and log in "
        "with /login, then try again."
    );
}

void AuthStrategy::start() {
    const QString claude = findClaudeExecutable();
    if (claude.isEmpty()) {
        emit failed(notInstalledMessage());
        return;
    }
    const Paths paths = Paths::detect();
    if (!QFileInfo(paths.home).isDir()) {
        emit failed(
            QCoreApplication::translate(
                "claude_code",
                "Claude Code doesn't seem to be set up on this computer: %1 doesn't exist. Run "
                "`claude` once in a terminal, then add the workspace again."
            )
                .arg(QDir::toNativeSeparators(paths.home))
        );
        return;
    }
    // A login that can't be read (an old CLI) lets the workspace in: a session
    // that then finds none says so (see TranscriptItem::loginError).
    auto *launcher = new Launcher(claude, paths, this);
    launcher->checkLogin([this, launcher, claude](Login login) {
        launcher->deleteLater();
        if (login == Login::Out)
            emit failed(notLoggedInMessage());
        else
            emit succeeded(toRecord(Credentials{claude}));
    });
}

} // namespace claude_code
