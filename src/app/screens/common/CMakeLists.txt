# msga_screens_common — what every screen group shares beyond Context:
# the file/folder choosers (OS dialog, else the in-app ui::FileBrowser) and
# the disk cache of downloaded pictures (remote_images.h), the loading
# ring and the avatar placeholder letter.
add_library(msga_screens_common STATIC
    avatar_initial.cpp
    canvas_doc.cpp
    tag_badge.cpp
    downloads.cpp
    file_dialogs.cpp
    loading_indicator.cpp
    message_text.cpp
    remote_images.cpp
)
target_include_directories(msga_screens_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../..
    ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_link_libraries(msga_screens_common PUBLIC msga_app_core msga_ui msga_net msga_flags)
