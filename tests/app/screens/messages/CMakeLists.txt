# msga_screens_messages on the test fixture (../../../assets), headless. A
# suite of msga_app_tests (../../CMakeLists.txt), one ctest test per case
# group ("image: ...").
target_sources(msga_app_tests PRIVATE test_messages.cpp)
target_link_libraries(msga_app_tests PRIVATE msga_screens_messages msga_app_fake msga_llm_testkit)
foreach(t image grouping dates anchoring actions thread loading selection opening blocks threads audio)
    msga_add_test(messages_${t} COMMAND msga_app_tests test_messages ${t} TZ LABELS messages)
endforeach()
