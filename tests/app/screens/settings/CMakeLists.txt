# The Settings dialog, driven inside a whole shell on the test fixture
# (../../../assets), headless.
add_executable(settings_tests test_settings.cpp main.cpp)
target_link_libraries(settings_tests PRIVATE msga_screens_shell msga_app_fake msga_llm_testkit msga_test)
target_compile_definitions(settings_tests PRIVATE MSGA_TEST_ASSETS="${MSGA_TEST_ASSETS}")
add_test(NAME settings_tests COMMAND settings_tests)
set_tests_properties(settings_tests PROPERTIES ENVIRONMENT
    "PLAT_BACKEND=headless;TZ=Europe/Stockholm;HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home/.config;XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home/.cache;XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home/.local/share;XDG_STATE_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home/.local/state")
