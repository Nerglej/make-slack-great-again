# msga_app_llm: the provider wires and the Service, against the fake AI
# server (msga_llm_testkit, ../../CMakeLists.txt).
add_executable(msga_llm_tests test_llm.cpp test_voice.cpp main.cpp)
target_link_libraries(msga_llm_tests PRIVATE msga_llm_testkit msga_test)
add_test(NAME msga_llm_tests COMMAND msga_llm_tests)
set_tests_properties(msga_llm_tests PROPERTIES ENVIRONMENT "PLAT_BACKEND=headless")
