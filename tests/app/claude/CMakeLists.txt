# msga_app_claude: the Claude Code workspace, against fake ~/.claude trees.
add_executable(claude_tests
    test_terminal.cpp
    test_launcher.cpp
    test_roster.cpp
    test_transcript.cpp
    test_roles.cpp
    test_outputs.cpp
    test_worktrees.cpp
    test_catalog.cpp
    test_backend.cpp
    main.cpp
)
target_link_libraries(claude_tests PRIVATE msga_app_claude msga_test)
target_compile_definitions(claude_tests PRIVATE MSGA_SOURCE_DIR="${MSGA_ROOT}")
add_test(NAME claude_tests COMMAND claude_tests)
# Headless plat, a pinned zone, a throwaway HOME/XDG: the tests never see
# the user's ~/.claude (each case builds its own fake one).
set_tests_properties(claude_tests PROPERTIES ENVIRONMENT
    "PLAT_BACKEND=headless;TZ=Europe/Stockholm;HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home/.config;XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home/.cache;XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home/.local/share;CLAUDE_CONFIG_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-home/.claude")
