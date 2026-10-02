# msga_screens_shell: whole shells on the test fixture (../../../assets).
set(_assets MSGA_TEST_ASSETS="${MSGA_TEST_ASSETS}")

add_executable(shell_tests test_shell.cpp main.cpp)
target_link_libraries(shell_tests PRIVATE msga_screens_shell msga_app_fake msga_llm_testkit msga_test)
target_compile_definitions(shell_tests PRIVATE ${_assets})
add_test(NAME shell_tests COMMAND shell_tests)

add_executable(shortcut_tests test_shortcuts.cpp main.cpp)
target_link_libraries(shortcut_tests PRIVATE msga_screens_shell msga_app_fake msga_test)
target_compile_definitions(shortcut_tests PRIVATE ${_assets})
add_test(NAME shortcut_tests COMMAND shortcut_tests)

add_executable(pages_tests test_pages.cpp main.cpp)
target_link_libraries(pages_tests PRIVATE msga_screens_shell msga_app_fake msga_test)
target_compile_definitions(pages_tests PRIVATE ${_assets})
add_test(NAME pages_tests COMMAND pages_tests)

add_executable(parity_tests test_parity.cpp main.cpp)
target_link_libraries(parity_tests PRIVATE msga_screens_shell msga_app_fake msga_test)
target_compile_definitions(parity_tests PRIVATE ${_assets})
add_test(NAME parity_tests COMMAND parity_tests)

add_executable(agent_tests test_agents.cpp main.cpp)
target_link_libraries(agent_tests PRIVATE msga_screens_shell msga_app_fake msga_test)
target_compile_definitions(agent_tests PRIVATE ${_assets})
add_test(NAME agent_tests COMMAND agent_tests)

add_executable(agent_ui_tests test_agent_ui.cpp main.cpp)
target_link_libraries(agent_ui_tests PRIVATE msga_screens_shell msga_app_fake msga_test)
target_compile_definitions(agent_ui_tests PRIVATE ${_assets})
add_test(NAME agent_ui_tests COMMAND agent_ui_tests)

# Headless plat, a pinned zone, and a throwaway HOME/XDG so the tests never
# touch the user's settings or font index cache.
set_tests_properties(shell_tests shortcut_tests pages_tests parity_tests agent_tests agent_ui_tests PROPERTIES ENVIRONMENT
    "PLAT_BACKEND=headless;TZ=Europe/Stockholm;HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home/.config;XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home/.cache;XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home/.local/share;XDG_STATE_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home/.local/state")

# Its own throwaway HOME: it writes a launcher entry and a socket there.
add_executable(identity_tests test_identity.cpp main.cpp)
target_link_libraries(identity_tests PRIVATE msga_screens_shell msga_test)
add_test(NAME identity_tests COMMAND identity_tests)
set(_home "${CMAKE_CURRENT_BINARY_DIR}/test-home-identity")
set_tests_properties(identity_tests PROPERTIES ENVIRONMENT
    "PLAT_BACKEND=headless;HOME=${_home};XDG_CONFIG_HOME=${_home}/.config;XDG_CACHE_HOME=${_home}/.cache;XDG_DATA_HOME=${_home}/.local/share;XDG_STATE_HOME=${_home}/.local/state")
