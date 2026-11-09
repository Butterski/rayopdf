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
# ThreadSanitizer on every target (Clang; not together with RAYOPDF_SANITIZE).
option(RAYOPDF_TSAN "Build with ThreadSanitizer" OFF)
# libFuzzer targets of tests/fuzz (Clang only; implies the sanitizers).
option(RAYOPDF_FUZZ "Build the libFuzzer targets of tests/fuzz" OFF)

if(RAYOPDF_FUZZ)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        message(FATAL_ERROR "RAYOPDF_FUZZ needs Clang (libFuzzer).")
    endif()
    set(RAYOPDF_SANITIZE ON)
endif()
if(RAYOPDF_TSAN AND RAYOPDF_SANITIZE)
    message(FATAL_ERROR "RAYOPDF_TSAN and RAYOPDF_SANITIZE exclude each other.")
endif()

# Threads for the batch modes' workers (--batch, --serve --json --workers=N).
find_package(Threads REQUIRED)

# Flags of all own code: no exceptions and no RTTI anywhere (roadmap D1), warnings on. Release
# builds on Linux also have no unwind tables: nothing unwinds (a throw of libstdc++'s ends the
# process with or without them), and the tables only take file space.
function(rayopdf_flags target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -fno-exceptions -fno-rtti -fno-math-errno
            $<$<CONFIG:Release>:-O2>
            $<$<CONFIG:Release>:-ffunction-sections>
            $<$<CONFIG:Release>:-fdata-sections>)
        if(NOT WIN32)
            target_compile_options(${target} PRIVATE
                $<$<CONFIG:Release>:-fno-asynchronous-unwind-tables>
                $<$<CONFIG:Release>:-fno-unwind-tables>)
        endif()
        if(RAYOPDF_SANITIZE)
            target_compile_options(${target} PRIVATE -fsanitize=address,undefined
                -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
            target_link_options(${target} PRIVATE -fsanitize=address,undefined)
        endif()
        if(RAYOPDF_TSAN)
            target_compile_options(${target} PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
            target_link_options(${target} PRIVATE -fsanitize=thread)
        endif()
        if(RAYOPDF_FUZZ)
            target_compile_options(${target} PRIVATE -fsanitize=fuzzer-no-link)
        endif()
    endif()
    if(MINGW)
        # C99 printf and strtod from MinGW-w64 rather than msvcrt's, so numbers are written as on
        # Linux; every executable fully static (no libstdc++, libgcc or winpthread DLL beside it).
        target_compile_definitions(${target} PRIVATE __USE_MINGW_ANSI_STDIO=1)
        target_link_options(${target} PRIVATE -static)
    endif()
    target_compile_definitions(${target} PRIVATE RAYOPDF_VERSION="${RAYOPDF_VERSION}")
endfunction()

# One static library per module directory src/<module>/ (every .cpp in it). Dependencies:
# common <- crypt <- pdf <- text, image <- layout <- the command line; raster (paths and the
# rasterizer for glyph-shape recovery) <- text. `ocr` is the OCR add-on's
# own code (pre- and post-processing, protocol server, Tesseract backend; no inference runtime):
# only the add-on rayopdf-ocr, its tests and its fuzz target link it, never `rayopdf`.
set(RAYOPDF_MODULES common crypt pdf raster text image layout ocr)
set(RAYOPDF_DEPENDS_crypt common)
set(RAYOPDF_DEPENDS_pdf crypt common)
set(RAYOPDF_DEPENDS_raster common)
set(RAYOPDF_DEPENDS_text pdf raster)
set(RAYOPDF_DEPENDS_image pdf)
set(RAYOPDF_DEPENDS_layout text image)
set(RAYOPDF_DEPENDS_ocr crypt common)
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
        if(module STREQUAL "common")
            target_link_libraries(rayopdf_common PUBLIC Threads::Threads)
        endif()
    endif()
endforeach()

# GNU ld (not gold, lld or a PE linker) takes the INSERT scripts below.
set(RAYOPDF_GNU_LD OFF)
if(NOT WIN32 AND NOT APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    separate_arguments(rayopdf_linker_flags UNIX_COMMAND "${CMAKE_EXE_LINKER_FLAGS}")
    execute_process(COMMAND ${CMAKE_CXX_COMPILER} ${rayopdf_linker_flags} -Wl,--version
        OUTPUT_VARIABLE rayopdf_linker_version ERROR_QUIET)
    if(rayopdf_linker_version MATCHES "^GNU ld")
        set(RAYOPDF_GNU_LD ON)
    endif()
endif()

# lines.cpp without GCC's basic-block (SLP) vectorizer: it packs BuildLines' per-glyph boxes into
# vector registers and spends shuffles on every glyph (odl: 5.4 M fewer instructions without it).
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set_source_files_properties(src/layout/lines.cpp PROPERTIES COMPILE_OPTIONS "-fno-tree-slp-vectorize")
endif()

if(TARGET rayopdf_layout)
    add_executable(rayopdf src/cli/main.cpp)
    target_link_libraries(rayopdf PRIVATE rayopdf_layout)
    rayopdf_flags(rayopdf)
    if(RAYOPDF_STATIC)
        if(WIN32 OR APPLE)
            message(FATAL_ERROR "RAYOPDF_STATIC is for Linux.")
        endif()
        # Relative relocations packed (DT_RELR): 53 KB less to read and apply at start. The static
        # binary carries its own start code, so this needs no newer glibc where it runs.
        target_link_options(rayopdf PRIVATE -static-pie $<$<CONFIG:Release>:-Wl,-z,pack-relative-relocs>)
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND NOT APPLE)
        target_link_options(rayopdf PRIVATE -static-libstdc++ -static-libgcc)
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE)
        target_link_options(rayopdf PRIVATE $<$<CONFIG:Release>:-Wl,--gc-sections>)
    endif()
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND NOT WIN32 AND NOT APPLE)
        # Release builds: a throw ends the process without unwinding (no_unwind.cpp, which also
        # stands for what glibc's libc.a calls of the unwinder in the static build), so libgcc's
        # unwinder, the personality routines and the demangler are not linked, and no_unwind.ld
        # drops the unwind tables nothing reads.
        target_sources(rayopdf PRIVATE $<$<CONFIG:Release>:${CMAKE_CURRENT_SOURCE_DIR}/src/cli/no_unwind.cpp>)
        if(RAYOPDF_GNU_LD)
            set(script "${CMAKE_CURRENT_SOURCE_DIR}/src/cli/no_unwind.ld")
            target_link_options(rayopdf PRIVATE "$<$<CONFIG:Release>:-Wl,-T,${script}>")
            set_property(TARGET rayopdf APPEND PROPERTY LINK_DEPENDS "${script}")
        endif()
    endif()
    if(RAYOPDF_GNU_LD)
        # Release builds place the code fresh conversions run first, in the order of src/hot.ld
        # (generated by tools/hot_sections.py; an INSERT script, so the default layout stays for
        # the rest): fewer 64 KiB windows of code to fault in. Names it lists that no longer exist
        # match nothing.
        set(script "${CMAKE_CURRENT_SOURCE_DIR}/src/hot.ld")
        target_link_options(rayopdf PRIVATE "$<$<CONFIG:Release>:-Wl,-T,${script}>")
        set_property(TARGET rayopdf APPEND PROPERTY LINK_DEPENDS "${script}")
    endif()
endif()

# The OCR add-on rayopdf-ocr (docs/development/ocr-engine.md): src/ocr/addon/ plus one inference
# runtime, a static build of it under RAYOPDF_OCR_RUNTIME_DIR (include/, lib/), as
# docker/ocr.Dockerfile installs them (tools/dev.py configs ocr, ocr-ort, ocr-windows).
option(RAYOPDF_OCR_ADDON "Build the OCR add-on rayopdf-ocr" OFF)
set(RAYOPDF_OCR_RUNTIME "ncnn" CACHE STRING "Inference runtime of rayopdf-ocr: ncnn or onnxruntime")
set(RAYOPDF_OCR_RUNTIME_DIR "" CACHE PATH "Static build of that runtime (include/, lib/)")
set(RAYOPDF_OCR_TIER "small" CACHE STRING "Default model tier of rayopdf-ocr: tiny, small or medium")
set(RAYOPDF_OCR_MODELS "" CACHE PATH "Model folder (with <tier>/) for tests/verify_ocr.py")
if(RAYOPDF_OCR_ADDON)
    if(NOT RAYOPDF_OCR_RUNTIME_DIR)
        message(FATAL_ERROR "RAYOPDF_OCR_ADDON needs RAYOPDF_OCR_RUNTIME_DIR.")
    endif()
    add_executable(rayopdf-ocr src/ocr/addon/main.cpp src/ocr/addon/runtime_${RAYOPDF_OCR_RUNTIME}.cpp)
    target_link_libraries(rayopdf-ocr PRIVATE rayopdf_ocr)
    rayopdf_flags(rayopdf-ocr)
    target_compile_definitions(rayopdf-ocr PRIVATE RAYOPDF_OCR_TIER="${RAYOPDF_OCR_TIER}")
    # In a folder of its own: the core's add-on discovery looks beside the running executable, and
    # the core tests in this build directory must not find (and start) the real add-on.
    set_target_properties(rayopdf-ocr PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/addon")
    if(RAYOPDF_OCR_RUNTIME STREQUAL "ncnn")
        target_include_directories(rayopdf-ocr SYSTEM PRIVATE "${RAYOPDF_OCR_RUNTIME_DIR}/include/ncnn")
        target_link_libraries(rayopdf-ocr PRIVATE "${RAYOPDF_OCR_RUNTIME_DIR}/lib/libncnn.a" Threads::Threads)
    elseif(RAYOPDF_OCR_RUNTIME STREQUAL "onnxruntime")
        file(GLOB ort_libraries "${RAYOPDF_OCR_RUNTIME_DIR}/lib/*.a")
        target_include_directories(rayopdf-ocr SYSTEM PRIVATE "${RAYOPDF_OCR_RUNTIME_DIR}/include")
        target_link_libraries(rayopdf-ocr PRIVATE -Wl,--start-group ${ort_libraries} -Wl,--end-group
            Threads::Threads ${CMAKE_DL_LIBS})
        # MLAS's assembly kernels lack .note.GNU-stack; the stack stays non-executable regardless.
        target_link_options(rayopdf-ocr PRIVATE -Wl,-z,noexecstack)
    else()
        message(FATAL_ERROR "RAYOPDF_OCR_RUNTIME must be ncnn or onnxruntime.")
    endif()
    if(RAYOPDF_STATIC)
        target_link_options(rayopdf-ocr PRIVATE -static-pie)
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND NOT APPLE)
        target_link_options(rayopdf-ocr PRIVATE -static-libstdc++ -static-libgcc)
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE)
        target_link_options(rayopdf-ocr PRIVATE $<$<CONFIG:Release>:-Wl,--gc-sections>)
    endif()
endif()

if(RAYOPDF_TESTS)
    enable_testing()
    # Core tests: one executable per file tests/test_<module>[_<part>].cpp, linked to its module.
    # Every check fails with a code of its own (tests/check.h lists the ranges);
    # tests/verify_cli.py checks the built command line. RAYOPDF_OCR names no file in every test, so
    # an OCR add-on installed beside the build or on PATH is never found (an environment variable
    # naming nothing ends the add-on search); tests/test_layout_ocr.cpp names its stand-in itself.
    set(rayopdf_test_environment "RAYOPDF_OCR=/nonexistent/rayopdf-ocr")
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
            set_tests_properties(${test_name} PROPERTIES ENVIRONMENT "${rayopdf_test_environment}")
        endif()
    endforeach()
    # The stand-in OCR add-on that tests/test_layout_ocr.cpp and tests/test_layout_glyph.cpp start
    # (tests/ocr_mock_addon.cpp).
    if(TARGET test_layout_ocr)
        add_executable(ocr_mock_addon tests/ocr_mock_addon.cpp)
        target_link_libraries(ocr_mock_addon PRIVATE rayopdf_common)
        rayopdf_flags(ocr_mock_addon)
        foreach(test test_layout_ocr test_layout_glyph)
            if(TARGET ${test})
                add_dependencies(${test} ocr_mock_addon)
                target_compile_definitions(${test} PRIVATE RAYOPDF_OCR_MOCK="$<TARGET_FILE:ocr_mock_addon>")
            endif()
        endforeach()
    endif()
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND AND TARGET rayopdf)
        # A cross build's binary runs under its emulator (Wine for the Windows build).
        set(rayopdf_emulator)
        if(CMAKE_CROSSCOMPILING_EMULATOR)
            list(GET CMAKE_CROSSCOMPILING_EMULATOR 0 rayopdf_emulator)
            set(rayopdf_emulator --emulator ${rayopdf_emulator})
        endif()
        add_test(NAME cli COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_cli.py"
            --binary $<TARGET_FILE:rayopdf> ${rayopdf_emulator})
        set_tests_properties(cli PROPERTIES ENVIRONMENT "${rayopdf_test_environment}")
        if(TARGET rayopdf-ocr AND RAYOPDF_OCR_MODELS)
            add_test(NAME ocr_addon COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_ocr.py"
                --binary $<TARGET_FILE:rayopdf-ocr> --models "${RAYOPDF_OCR_MODELS}" ${rayopdf_emulator})
        endif()
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
