cmake_minimum_required(VERSION 3.16)
project(CodexCpp LANGUAGES C CXX)
# Remove the obsolete cache switch: even old OFF caches now use the sole backend.
unset(CODEX_WITH_LIBGIT2 CACHE)
find_package(Threads REQUIRED)
include(FetchContent)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_CLI OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(USE_SSH OFF CACHE STRING "" FORCE)
set(USE_HTTPS OFF CACHE STRING "" FORCE)
FetchContent_Declare(codex_libgit2
  URL https://codeload.github.com/libgit2/libgit2/tar.gz/refs/tags/v1.8.4
  URL_HASH SHA256=49d0fc50ab931816f6bfc1ac68f8d74b760450eebdb5374e803ee36550f26774)
FetchContent_MakeAvailable(codex_libgit2)

add_library(codex-runtime INTERFACE)
target_compile_features(codex-runtime INTERFACE cxx_std_17)
target_include_directories(codex-runtime INTERFACE "${codex_libgit2_SOURCE_DIR}/include")
target_link_libraries(codex-runtime INTERFACE Threads::Threads libgit2package)
if(WIN32)
  target_link_libraries(codex-runtime INTERFACE ws2_32 bcrypt advapi32 shell32)
endif()
add_executable(codex-cpp codex.cpp)
# Build this test target explicitly; no extra application-sized compile for users.
add_executable(codex-self-test EXCLUDE_FROM_ALL tests/agent_self_test.cpp)
foreach(target codex-cpp codex-self-test)
  set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
  target_link_libraries(${target} PRIVATE codex-runtime)
  if(MSVC)
    target_compile_options(${target} PRIVATE /utf-8 /W4)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endforeach()
add_custom_target(codex-web-assets ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/web $<TARGET_FILE_DIR:codex-cpp>/web
  DEPENDS codex-cpp
  VERBATIM)
