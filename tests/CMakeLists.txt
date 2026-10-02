# The test suites (MSGA_BUILD_TESTS=ON, added from the root CMakeLists.txt).
# One directory per module under ../src, each defining its own ctest tests;
# support/ holds what several of them share. Everything a test reads from
# disk is in assets/ (MSGA_TEST_ASSETS): the fake workspace's fixture.json,
# its pictures, GIFs and files, and the SVG fixtures.
#
# plat's own tests (plat/) belong to its standalone build (cmake -S src/plat),
# which adds that directory itself; the app build leaves them out.
get_filename_component(MSGA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
find_package(Python3 COMPONENTS Interpreter QUIET)

# ── Shared ──────────────────────────────────────────────────────────────────
# The harness: TEST/CHECK/REQUIRE and the portable helpers (support/test.h).
add_library(msga_test STATIC support/test.cpp)
target_include_directories(msga_test PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(msga_test PUBLIC msga_base msga_flags)

# A fake AI server (support/fake_llm.py) for any suite that needs answers:
# the llm tests, and the messages/settings/shell screens'. POSIX only.
if(TARGET msga_app_llm)
    add_library(msga_llm_testkit STATIC support/fake_llm_server.cpp)
    target_link_libraries(msga_llm_testkit PUBLIC msga_app_llm msga_test)
    target_compile_definitions(msga_llm_testkit PRIVATE
        FAKE_LLM_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/support/fake_llm.py")
endif()

# ── Suites, bottom-up ───────────────────────────────────────────────────────
foreach(dir base net gfx text ui app)
    if(TARGET msga_${dir} OR (dir STREQUAL "app" AND TARGET msga_app_core))
        add_subdirectory(${dir})
    endif()
endforeach()
