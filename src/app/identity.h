// Who msga is to the OS, and where it keeps its files: the old Qt app's
// identities everywhere, so the new binary is a drop-in replacement for it
// (an upgrade finds its workspaces, settings and caches; launchers, the
// notification centre, the taskbar and the Homebrew cask see the same app).
//
//                    Linux                    macOS                  Windows
//   app id           msga (app_id, WM_CLASS   com.nisdos.msga        com.nisdos.msga
//                    msga/MSGA, msga.desktop) (bundle id)            (AppUserModelID)
//   URL scheme       msga://                  msga://                msga://
//   settings store   ~/.config/msga/msga.conf com.msga.msga plist    HKCU\Software\msga\msga
//                    (old QSettings; base/old_settings.h — the credentials stay there
//                    on Linux/Windows, the Keychain on macOS)
//   configDir        ~/.config/msga           <dataDir>              <dataDir>
//   dataDir          ~/.local/share/msga/MSGA ~/Library/Application  %APPDATA%\msga\MSGA
//                                             Support/msga/MSGA
//   cacheDir         ~/.cache/msga/MSGA       ~/Library/Caches/      %LOCALAPPDATA%\msga\MSGA\cache
//                                             msga/MSGA
//
// dataDir and cacheDir are Qt's AppDataLocation and CacheLocation for the old
// app (organization "msga", application "MSGA"); configDir is where the old
// QSettings file lives (Linux) or dataDir. The new app's own files go in
// these directories under names the old app never used (settings.json,
// workspaces.json, workspaces/, images/), so the two never read each
// other's formats; files whose format did not change (crash.log,
// claude-code/, workspace_icons/, tray_icon.png) are shared.
#pragma once

#include <string>

namespace plat {
class App;
}

namespace identity {

inline constexpr const char *kName      = "MSGA"; // QCoreApplication::applicationName
inline constexpr const char *kUrlScheme = "msga";
// Linux: the desktop entry / Wayland app_id / WM_CLASS instance; elsewhere
// the bundle id / AUMID.
const char                  *appId();
// The single-instance channel's name (plat::App::claimSingleInstance).
const char                  *instanceKey();

// The old Qt app, if it is running, takes this launch the way it took its
// own second launches (old app/single_instance.cpp: a QLocalSocket
// "msga-<home folder name>", one QDataStream QString — the msga:// URL, or
// nothing): it shows its window and gets the URL. True when it did; this
// process then exits, so the two never run on one store at once.
bool handOffToOldApp(const std::string &url);

// Absolute, no trailing slash; "" when the OS gives no such directory.
std::string configDir(plat::App &app);
std::string dataDir(plat::App &app);
std::string cacheDir(plat::App &app);
// <dataDir>/crash.log: where the old crash handler appended its reports.
std::string crashLogPath(plat::App &app);

} // namespace identity
