# text: one binary, one ctest test per case group.
add_executable(text_tests text_tests.cpp)
target_link_libraries(text_tests PRIVATE msga_text ${MSGA_HB_TARGET})
foreach(t wrap ellipsis emoji_caret emoji_color emoji_paint rtl cjk_fallback hit_roundtrip inline_box style_boundary
          word_select malformed_utf8 glyph_cache ink_bounds ink_lean perf recolor measure_cache)
    add_test(NAME text_${t} COMMAND text_tests ${t})
endforeach()
