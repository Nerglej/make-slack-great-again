# NSURLSession transport (transport.h): HTTP data tasks and
# NSURLSessionWebSocketTask, with the OS's TLS, proxies and HTTP/2 — nothing
# bundled. Objective-C++ with ARC, like plat's Cocoa backend; msga_flags'
# -fno-exceptions/-fno-rtti only reach C++ sources, so they are repeated here.
enable_language(OBJCXX)
set(_net_mac_sources ${CMAKE_CURRENT_LIST_DIR}/nsurlsession.mm)
target_sources(msga_net PRIVATE ${_net_mac_sources})
set_source_files_properties(${_net_mac_sources} PROPERTIES
    COMPILE_OPTIONS "-fobjc-arc;-fno-exceptions;-fno-rtti")
target_link_libraries(msga_net PUBLIC "-framework Foundation" "-framework Security")
