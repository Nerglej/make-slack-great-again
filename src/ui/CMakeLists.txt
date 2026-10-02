# ui — the widget toolkit (see ui.h).
add_library(msga_ui STATIC
    view.cpp
    window.cpp
    theme.cpp
    widgets.cpp
    popup.cpp
   
    datetime.cpp
   
    controls.cpp
    scroll.cpp
    vlist.cpp
    textedit.cpp
    richtext.cpp
    debug.cpp
)
# The in-app file chooser is Linux-only: Windows and macOS always have the
# OS dialog.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(msga_ui PRIVATE filebrowser.cpp)
endif()
target_include_directories(msga_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(msga_ui PUBLIC msga_text msga_gfx msga_base plat msga_flags)

# ── Gallery ─────────────────────────────────────────────────────────────────
# Every widget in a fake chat layout (10,000 generated messages), pictures
# from the test media (tests/assets). The tests are in tests/ui.
add_executable(ui_gallery gallery/gallery.cpp)
target_link_libraries(ui_gallery PRIVATE msga_ui)
if(COMMAND msga_size_map)
    msga_size_map(ui_gallery)
endif()
target_compile_definitions(ui_gallery PRIVATE
    MSGA_TEST_ASSETS="${MSGA_TEST_ASSETS}")
