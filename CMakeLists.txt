cmake_minimum_required(VERSION 3.16)
project(PseudoFS VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Curses REQUIRED)

file(GLOB_RECURSE SOURCES src/*.cpp)

add_executable(pfs ${SOURCES})

target_include_directories(pfs PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CURSES_INCLUDE_DIRS}
)

target_link_libraries(pfs PRIVATE ${CURSES_LIBRARIES})
