cmake_minimum_required(VERSION 3.30)

project(
    alonite
    VERSION 0.1
    LANGUAGES CXX)

include(cmake/common.cmake)

set(CMAKE_AUTOMOC ON)

add_library(
    alonite
    INTERFACE
    include/alonite/closed_error.h
    include/alonite/contract.h
    include/alonite/log.h
    include/alonite/mpsc.h
    include/alonite/mutex.h
    include/alonite/qt.h
    include/alonite/runtime.h
    include/alonite/scope_exit.h)
set_target_properties(alonite PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED
                                                         ON)
target_include_directories(alonite INTERFACE include)

option(alonite_ABORT_ON_INVARIANT_VIOLATION "Abort on invariant violation" OFF)
if(alonite_ABORT_ON_INVARIANT_VIOLATION)
    target_compile_definitions(alonite
                               INTERFACE alonite_ABORT_ON_INVARIANT_VIOLATION)
endif()

option(alonite_ABORT_ON_TODO "Abort on alonite_todo" OFF)
if(alonite_ABORT_ON_TODO)
    target_compile_definitions(alonite INTERFACE alonite_ABORT_ON_TODO)
endif()
option(alonite_WARNINGS_ARE_ERRORS "Warnings are errors" ON)

find_package(
    QT NAMES Qt6 Qt5
    COMPONENTS Widgets
    QUIET)
message(STATUS "Using Qt${QT_VERSION_MAJOR}")
find_package(Qt${QT_VERSION_MAJOR} REQUIRED QUIET COMPONENTS Core)
target_link_libraries(alonite INTERFACE Qt${QT_VERSION_MAJOR}::Core)

add_executable(test_alonite test/mpsc.cpp test/mutex.cpp test/runtime.cpp
                            test/qt.cpp)
find_package(Catch2 REQUIRED QUIET)
target_link_libraries(test_alonite PRIVATE alonite Catch2::Catch2WithMain)
set_target_build_settings(test_alonite WARNINGS_ARE_ERRORS
                          ${alonite_WARNINGS_ARE_ERRORS})

add_executable(bench_alonite bench/runtime.cpp)
find_package(Boost CONFIG REQUIRED COMPONENTS system)
target_link_libraries(bench_alonite PRIVATE alonite Boost::boost Boost::system
                                            Qt${QT_VERSION_MAJOR}::Core)
set_target_build_settings(bench_alonite WARNINGS_ARE_ERRORS
                          ${alonite_WARNINGS_ARE_ERRORS})

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set_property(
        DIRECTORY
        APPEND
        PROPERTY ADDITIONAL_CLEAN_FILES ${CMAKE_BINARY_DIR}/dcov
                 "${CMAKE_BINARY_DIR}/cov.sh")
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/cov.sh
        COMMAND ${CMAKE_COMMAND} -E copy ${PROJECT_SOURCE_DIR}/script/cov.sh
                ${CMAKE_BINARY_DIR}
        DEPENDS script/cov.sh)
    add_custom_target(
        cov
        DEPENDS ${CMAKE_BINARY_DIR}/cov.sh
        DEPENDS test_alonite)
    add_custom_command(
        TARGET cov
        POST_BUILD
        COMMAND "${CMAKE_BINARY_DIR}/cov.sh"
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
else()
    message(NOTICE "Build in Debug mode to enable coverage")
endif()
