# msga_screens_messages on the test fixture (../../../assets), headless.
add_executable(messages_tests test_messages.cpp main.cpp)
target_link_libraries(messages_tests PRIVATE msga_screens_messages msga_app_fake msga_llm_testkit msga_test)
target_compile_definitions(messages_tests PRIVATE MSGA_TEST_ASSETS="${MSGA_TEST_ASSETS}")
foreach(t image grouping dates anchoring actions thread loading selection opening blocks threads audio)
    add_test(NAME messages_${t} COMMAND messages_tests ${t})
    set_tests_properties(messages_${t} PROPERTIES
        ENVIRONMENT "PLAT_BACKEND=headless;TZ=Europe/Stockholm")
endforeach()
