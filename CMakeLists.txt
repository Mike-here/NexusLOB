cmake_minimum_required(VERSION 3.20)
project(NexusLOB LANGUAGES CXX)

option(NEXUSLOB_NATIVE_ARCH "Compile for the host CPU" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_library(nexuslob_core STATIC src/order_book.cpp)
target_include_directories(nexuslob_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(nexuslob_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
target_compile_options(nexuslob_core PRIVATE $<$<CONFIG:Release>:-O3>)

if(NEXUSLOB_NATIVE_ARCH AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nexuslob_core PRIVATE -march=native)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(nexuslob_server src/main.cpp src/network_listener.cpp)
    target_link_libraries(nexuslob_server PRIVATE nexuslob_core Threads::Threads)
    target_compile_options(nexuslob_server PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    target_compile_options(nexuslob_server PRIVATE $<$<CONFIG:Release>:-O3>)
    if(NEXUSLOB_NATIVE_ARCH AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(nexuslob_server PRIVATE -march=native)
    endif()
else()
    message(STATUS "nexuslob_server uses Linux epoll and is built in Docker/Linux only")
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(order_book_tests tests/order_book_tests.cpp)
    target_link_libraries(order_book_tests PRIVATE nexuslob_core)
    target_compile_options(order_book_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    add_test(NAME order_book_tests COMMAND order_book_tests)
endif()
