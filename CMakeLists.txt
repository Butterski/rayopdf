# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.16)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" RAYOPDF_VERSION LIMIT_COUNT 1)
project(rayopdf VERSION ${RAYOPDF_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(RAYOPDF_TESTS "Build the core tests" ON)
# A fully static, position-independent Linux binary (glibc linked in): it runs in any container,
# including the evaluation harness's Debian 12 `cli` image.
option(RAYOPDF_STATIC "Link the Linux CLI fully statically" OFF)
# AddressSanitizer and UBSan on every target.
option(RAYOPDF_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
# libFuzzer targets of tests/fuzz (Clang only; implies the sanitizers).
option(RAYOPDF_FUZZ "Build the libFuzzer targets of tests/fuzz" OFF)

if(RAYOPDF_FUZZ)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        message(FATAL_ERROR "RAYOPDF_FUZZ needs Clang (libFuzzer).")
    endif()
    set(RAYOPDF_SANITIZE ON)
endif()

# Flags of all own code: no exceptions and no RTTI anywhere (roadmap D1), warnings on.
function(rayopdf_flags target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -fno-exceptions -fno-rtti -fno-math-errno
            $<$<CONFIG:Release>:-O2>
            $<$<CONFIG:Release>:-ffunction-sections>
            $<$<CONFIG:Release>:-fdata-sections>)
        if(RAYOPDF_SANITIZE)
            target_compile_options(${target} PRIVATE -fsanitize=address,undefined
                -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
            target_link_options(${target} PRIVATE -fsanitize=address,undefined)
        endif()
        if(RAYOPDF_FUZZ)
            target_compile_options(${target} PRIVATE -fsanitize=fuzzer-no-link)
        endif()
    endif()
    target_compile_definitions(${target} PRIVATE RAYOPDF_VERSION="${RAYOPDF_VERSION}")
endfunction()

# One static library per module directory src/<module>/ (every .cpp in it). Dependencies:
# common <- crypt <- pdf <- text, image <- layout <- the command line.
set(RAYOPDF_MODULES common crypt pdf text image layout)
set(RAYOPDF_DEPENDS_crypt common)
set(RAYOPDF_DEPENDS_pdf crypt common)
set(RAYOPDF_DEPENDS_text pdf)
set(RAYOPDF_DEPENDS_image pdf)
set(RAYOPDF_DEPENDS_layout text image)
foreach(module ${RAYOPDF_MODULES})
    file(GLOB sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/${module}/*.cpp")
    if(sources)
        add_library(rayopdf_${module} STATIC ${sources})
        target_include_directories(rayopdf_${module} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
        foreach(dependency ${RAYOPDF_DEPENDS_${module}})
            if(TARGET rayopdf_${dependency})
                target_link_libraries(rayopdf_${module} PUBLIC rayopdf_${dependency})
            endif()
        endforeach()
        rayopdf_flags(rayopdf_${module})
    else()
        message(STATUS "rayopdf: module ${module} has no sources yet")
    endif()
endforeach()

if(TARGET rayopdf_layout)
    add_executable(rayopdf src/cli/main.cpp)
    target_link_libraries(rayopdf PRIVATE rayopdf_layout)
    rayopdf_flags(rayopdf)
    if(RAYOPDF_STATIC)
        if(WIN32 OR APPLE)
            message(FATAL_ERROR "RAYOPDF_STATIC is for Linux.")
        endif()
        target_link_options(rayopdf PRIVATE -static-pie)
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND NOT APPLE)
        target_link_options(rayopdf PRIVATE -static-libstdc++ -static-libgcc)
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE)
        target_link_options(rayopdf PRIVATE $<$<CONFIG:Release>:-Wl,--gc-sections>)
    endif()
endif()

if(RAYOPDF_TESTS)
    enable_testing()
    # Core tests: one executable per file tests/test_<module>[_<part>].cpp, linked to its module.
    # Every check fails with a code of its own (tests/check.h lists the ranges);
    # tests/verify_cli.py checks the built command line.
    file(GLOB test_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_*.cpp")
    foreach(source ${test_sources})
        get_filename_component(name ${source} NAME_WE)
        string(REGEX REPLACE "^test_([a-z]+).*$" "\\1" module ${name})
        if(TARGET rayopdf_${module})
            add_executable(${name} ${source})
            target_link_libraries(${name} PRIVATE rayopdf_${module})
            target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
            rayopdf_flags(${name})
            string(REGEX REPLACE "^test_" "" test_name ${name})
            add_test(NAME ${test_name} COMMAND ${name})
        endif()
    endforeach()
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND AND TARGET rayopdf)
        add_test(NAME cli COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_cli.py"
            --binary $<TARGET_FILE:rayopdf>)
    endif()
endif()

if(RAYOPDF_FUZZ)
    # tests/fuzz/fuzz_<module>_<name>.cpp, linked to its module.
    file(GLOB fuzz_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/fuzz_*.cpp")
    foreach(source ${fuzz_sources})
        get_filename_component(name ${source} NAME_WE)
        string(REGEX REPLACE "^fuzz_([a-z]+)_.*$" "\\1" module ${name})
        if(TARGET rayopdf_${module})
            add_executable(${name} ${source})
            target_link_libraries(${name} PRIVATE rayopdf_${module})
            rayopdf_flags(${name})
            target_link_options(${name} PRIVATE -fsanitize=fuzzer)
        endif()
    endforeach()
endif()
