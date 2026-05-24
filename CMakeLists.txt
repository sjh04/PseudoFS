cmake_minimum_required(VERSION 3.16)
project(PseudoFS VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Curses REQUIRED)

# Main executable (exclude test files)
file(GLOB_RECURSE SOURCES src/*.cpp)

add_executable(pfs ${SOURCES})

target_include_directories(pfs PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CURSES_INCLUDE_DIRS}
)

target_link_libraries(pfs PRIVATE ${CURSES_LIBRARIES})

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

    target_include_directories(pfs_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
    )

    target_link_libraries(pfs_tests PRIVATE GTest::gtest_main)

    include(GoogleTest)
    gtest_discover_tests(pfs_tests)
endif()
