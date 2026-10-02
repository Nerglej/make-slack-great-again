// The old Qt msga's settings and workspace list (legacy.h).
//
// Runs once: when <configDir>/settings.json (resp. workspaces.json) does not
// exist yet, it is written from the old app's QSettings store
// (base/old_settings.h). The old store is only read — a rollback to the old
// app finds it as it was. Credentials are not copied anywhere: the new app
// reads and writes them in the old app's own entries (base/secret.h).
//
// Old keys with nothing to map to here are left behind:
// notifications/defaultMigrated (applied, not kept), the Teams and IMAP
// workspaces and credentials/teamsClientId (services dropped),
// imap/domainIcons. zenMode/<workspace key> becomes Settings::zenWorkspaces.
#pragma once

#include "app/auth/workspaces.h"
#include "base/old_settings.h"

#include <string>
#include <vector>

namespace plat {
class App;
}

namespace shell {
struct Settings;
}

namespace legacy {

// `old` is QSettings("msga", "msga"), `oldApp` the bare QSettings() store
// (composer/lastAttachDir only); `dataDir` the old AppDataLocation, where
// the custom tray icon was copied; `workspaces` the imported ones with their
// auth (an OAuth one keeps the old app-keys connection default).
void importOldSettings(
    const oldsettings::Map                   &old,
    const oldsettings::Map                   &oldApp,
    const std::string                        &dataDir,
    const std::vector<auth::WorkspaceRecord> &workspaces,
    shell::Settings                          *s
);

// The old app's Slack and Claude Code workspaces in its order, and the one
// it had open. A record's auth is empty when it is in the secret store under
// the old key (WorkspaceStore reads it there); the pre-2026 layouts kept
// plain tokens in the settings, which come back as a blob to store.
std::vector<auth::WorkspaceRecord> oldWorkspaces(const oldsettings::Map &old, std::string *active);

// The custom workspace pictures the old app installed (workspace/<key>/
// customIcon), copied under the new app's names into `iconDir`.
void importOldWorkspaceIcons(const oldsettings::Map &old, const std::string &iconDir);

// The settings and workspaces part of legacy::importOldApp (see the top of
// this file). Cheap when both files exist: two stat calls.
void importOldSettingsAndWorkspaces(
    plat::App &app, const std::string &settingsPath, const std::string &workspacesPath
);

} // namespace legacy
