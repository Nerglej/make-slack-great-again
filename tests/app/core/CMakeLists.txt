# msga_app_core / msga_app_slack / msga_app_update, the fake workspace on the
# test fixture (../../assets), and the Slack Web API against a fake server
# (../../support/fake_slack.py).
add_executable(msga_app_tests
    test_mrkdwn.cpp
    test_markdown_compose.cpp
    test_emoji.cpp
    test_store.cpp
    test_jobs.cpp
    test_fixture.cpp
    test_languages.cpp
    test_slack.cpp
    test_slack_write.cpp
    test_slack_read.cpp
    test_slack_cache.cpp
    test_slack_realtime.cpp
    test_slack_features.cpp
    test_update.cpp
    ../../support/fake_slack_server.cpp
    main.cpp
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(msga_app_tests PRIVATE test_local_import.cpp)
endif()
target_link_libraries(msga_app_tests PRIVATE msga_app_fake msga_app_slack msga_app_update msga_test)
target_compile_definitions(msga_app_tests PRIVATE
    MSGA_SOURCE_DIR="${MSGA_ROOT}"
    MSGA_TEST_ASSETS="${MSGA_TEST_ASSETS}"
    FAKE_SLACK_SCRIPT="${MSGA_ROOT}/tests/support/fake_slack.py")
add_test(NAME msga_app_tests COMMAND msga_app_tests)
# The fake backend's timers run on plat's headless loop; times pin a zone.
set_tests_properties(msga_app_tests PROPERTIES ENVIRONMENT "TZ=Europe/Stockholm;PLAT_BACKEND=headless")

if(UNIX)
    # plat audio through fake helper programs (Linux) / the OS player
    # without playing anything (macOS). Never the speakers or the mic.
    add_executable(audio_tests test_audio.cpp main.cpp)
    target_link_libraries(audio_tests PRIVATE msga_app_core msga_test)
    target_compile_definitions(audio_tests PRIVATE MSGA_SOURCE_DIR="${MSGA_ROOT}")
    add_test(NAME audio_tests COMMAND audio_tests)
    set_tests_properties(audio_tests PROPERTIES ENVIRONMENT "PLAT_BACKEND=headless")
    # Its own binary: crash handlers and the watchdog are process-wide.
    add_executable(crash_tests test_crash.cpp)
    target_link_libraries(crash_tests PRIVATE msga_app_core msga_test)
    add_test(NAME crash_tests COMMAND crash_tests)
    set_tests_properties(crash_tests PROPERTIES ENVIRONMENT "PLAT_BACKEND=headless")
endif()

if(Python3_FOUND)
    # Every tr() string is in the .po files and the table is regenerated.
    add_test(NAME i18n_up_to_date
        COMMAND Python3::Interpreter ${MSGA_ROOT}/src/tools/i18n.py check)
endif()
