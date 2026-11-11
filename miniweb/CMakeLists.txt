if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.18)
  project(CodexMiniWeb LANGUAGES C CXX)
  find_package(Threads REQUIRED)
endif()
# Embed the DOM prelude as bytes, not a compiler-limited string literal.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/prelude.js")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/prelude.js" prelude HEX)
string(REGEX REPLACE "(..)" "0x\\1," prelude "${prelude}")
string(REPEAT "0x[0-9a-f][0-9a-f]," 16 prelude_row)
string(REGEX REPLACE "(${prelude_row})" "\\1\n" prelude "${prelude}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/prelude.cpp" "extern const unsigned char codex_miniweb_prelude[]={${prelude}0};\n")
# Embedded headless renderer. No windowing toolkit or external runtime.
set(qjs "${CMAKE_CURRENT_SOURCE_DIR}/third_party/quickjs")
add_library(codex-miniweb STATIC
  src/canvas.cpp src/css.cpp src/dom.cpp src/engine.cpp src/font.cpp src/html.cpp
  src/image.cpp src/js.cpp src/layout.cpp src/loader.cpp src/paint.cpp src/stb_impl.cpp
  src/style_resolver.cpp src/svg.cpp src/util.cpp worker.cpp ${CMAKE_CURRENT_BINARY_DIR}/prelude.cpp
  ${qjs}/quickjs.c ${qjs}/dtoa.c ${qjs}/libregexp.c ${qjs}/libunicode.c)
target_include_directories(codex-miniweb PRIVATE src third_party ${qjs})
target_compile_features(codex-miniweb PUBLIC cxx_std_17)
set_target_properties(codex-miniweb PROPERTIES C_STANDARD 11)
if(MSVC)
  target_compile_definitions(codex-miniweb PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(codex-miniweb PRIVATE /utf-8)
else()
  target_compile_definitions(codex-miniweb PRIVATE _GNU_SOURCE)
  target_link_libraries(codex-miniweb PRIVATE m Threads::Threads ${CMAKE_DL_LIBS})
endif()
if(WIN32)
  target_link_libraries(codex-miniweb PRIVATE ws2_32)
endif()
add_executable(miniweb-regression EXCLUDE_FROM_ALL regression.cpp regression_rendering.cpp regression_tables.cpp regression_diagnostics.cpp)
target_link_libraries(miniweb-regression PRIVATE codex-miniweb)
