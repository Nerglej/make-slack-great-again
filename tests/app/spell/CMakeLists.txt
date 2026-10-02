# msga_app_spell: the word rules and the OS's (or Hunspell's) checker.
add_executable(spell_tests test_spell.cpp main.cpp)
target_link_libraries(spell_tests PRIVATE msga_app_spell msga_test)
add_test(NAME spell_tests COMMAND spell_tests)
# Headless plat and a throwaway HOME/XDG: added words never reach the
# user's own list.
set_tests_properties(spell_tests PROPERTIES ENVIRONMENT
    "PLAT_BACKEND=headless;HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home;XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home/.local/share;XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home/.cache;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home/.config")
