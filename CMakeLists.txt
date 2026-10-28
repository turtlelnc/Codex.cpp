cmake_minimum_required(VERSION 3.16)
project(CodexCpp LANGUAGES CXX)
find_package(Threads REQUIRED)
add_executable(codex-cpp codex.cpp)
target_compile_features(codex-cpp PRIVATE cxx_std_17)
set_target_properties(codex-cpp PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(codex-cpp PRIVATE Threads::Threads)
if(WIN32)
  target_link_libraries(codex-cpp PRIVATE ws2_32 bcrypt advapi32 shell32)
endif()
if(MSVC)
  target_compile_options(codex-cpp PRIVATE /utf-8 /W4)
else()
  target_compile_options(codex-cpp PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_custom_target(codex-web-assets ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/web $<TARGET_FILE_DIR:codex-cpp>/web
  DEPENDS codex-cpp
  VERBATIM)
