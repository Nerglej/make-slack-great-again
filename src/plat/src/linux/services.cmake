# D-Bus desktop services for the Linux backends (tray, notifications, badge,
# file dialogs, reveal, network/sleep/lock events, portal settings,
# portal colour scheme). libdbus-1 is linked directly (the static musl build
# has no dlopen); without it the build falls back to the everything-
# unavailable stub.
pkg_check_modules(PLAT_DBUS IMPORTED_TARGET dbus-1)
if(PLAT_DBUS_FOUND)
    target_sources(plat PRIVATE
        src/linux/dbus_conn.cpp
        src/linux/file_dialog.cpp
        src/linux/notifications.cpp
        src/linux/services_dbus.cpp
        src/linux/sni_tray.cpp
        src/linux/system_events.cpp
    )
    target_link_libraries(plat PUBLIC PkgConfig::PLAT_DBUS)
    # Portal colour-scheme check driven by scripts/plat-selftest-linux-services.sh
    # against the fake desktop (the selftest cannot flip the setting itself).
    add_executable(plat_services_probe scripts/fakes/services_probe.cpp)
    target_link_libraries(plat_services_probe PRIVATE plat)
else()
    message(STATUS "plat: libdbus-1 not found, Linux desktop services disabled")
    target_sources(plat PRIVATE src/linux/services_stub.cpp)
endif()
