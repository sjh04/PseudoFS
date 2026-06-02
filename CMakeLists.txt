cmake_minimum_required(VERSION 3.16)
project(PseudoFS VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Use the wide-character ncurses (ncursesw) so the TUI renders multi-byte
# UTF-8 text (e.g. Chinese file names / content) instead of garbled bytes.
# Combined with setlocale(LC_ALL, "") in the TUI, this enables Chinese display.
set(CURSES_NEED_WIDE TRUE)
find_package(Curses REQUIRED)

# Collect all source files except main.cpp into a library
file(GLOB_RECURSE LIB_SOURCES src/*.cpp)
list(FILTER LIB_SOURCES EXCLUDE REGEX "src/main\\.cpp$")

add_library(pfs_lib STATIC ${LIB_SOURCES})
target_include_directories(pfs_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CURSES_INCLUDE_DIRS}
)
target_link_libraries(pfs_lib PUBLIC ${CURSES_LIBRARIES})

# Main executable
add_executable(pfs src/main.cpp)
target_link_libraries(pfs PRIVATE pfs_lib)

# Testing
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

file(GLOB_RECURSE TEST_SOURCES tests/*.cpp)

if(TEST_SOURCES)
    add_executable(pfs_tests ${TEST_SOURCES})
    target_link_libraries(pfs_tests PRIVATE pfs_lib GTest::gtest_main)

    include(GoogleTest)
    gtest_discover_tests(pfs_tests)
endif()
