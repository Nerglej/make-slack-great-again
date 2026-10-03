# prim — the few string, UTF-8, hash and UTF-16 primitives both plat and base
# need. plat can't link base, so they live here, below both, with no
# dependencies: base re-exports them under its own names (str::, utf8::,
# crypto::fnv1a, base::wide/narrow), plat under core:: and win32::. One copy
# of each in the binary.
add_library(prim STATIC
    str.cpp
    utf8.cpp
    hash.cpp
)
if(WIN32)
    target_sources(prim PRIVATE winstr.cpp)
endif()
target_include_directories(prim PUBLIC include)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(prim PRIVATE -Wall -Wextra -ffunction-sections -fdata-sections)
endif()
