cmake_minimum_required(VERSION 3.16)
project(redis-compressed C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(redis-compressed MODULE src/main.c)

target_include_directories(redis-compressed PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BROTLI_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(BROTLI_DISABLE_TESTS ON CACHE BOOL "" FORCE)
set(BROTLI_BUNDLED_MODE ON)

include(FetchContent)

FetchContent_Declare(
    brotli
    GIT_REPOSITORY https://github.com/google/brotli.git
    GIT_TAG v1.2.0
)

FetchContent_MakeAvailable(brotli)

target_link_libraries(redis-compressed PRIVATE brotlienc brotlicommon)
