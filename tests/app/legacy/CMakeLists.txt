# msga_app_legacy: the upgrade from the old Qt msga. Its own throwaway HOME:
# it writes the old app's settings and cache there.
add_executable(legacy_tests test_legacy.cpp main.cpp)
target_link_libraries(legacy_tests PRIVATE msga_app_legacy msga_test)
add_test(NAME legacy_tests COMMAND legacy_tests)
set(_home "${CMAKE_CURRENT_BINARY_DIR}/test-home")
set_tests_properties(legacy_tests PROPERTIES ENVIRONMENT
    "PLAT_BACKEND=headless;HOME=${_home};XDG_CONFIG_HOME=${_home}/.config;XDG_CACHE_HOME=${_home}/.cache;XDG_DATA_HOME=${_home}/.local/share;XDG_STATE_HOME=${_home}/.local/state")
