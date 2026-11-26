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
# The C ABI as a shared library (librayopdf.so, librayopdf.dylib, rayopdf.dll) for the Python
# package: every target position-independent, and outside Windows hidden symbols but the C ABI's.
option(RAYOPDF_SHARED "Build the shared library of the C ABI" OFF)

if(RAYOPDF_FUZZ)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        message(FATAL_ERROR "RAYOPDF_FUZZ needs Clang (libFuzzer).")
    endif()
    set(RAYOPDF_SANITIZE ON)
endif()
if(RAYOPDF_TSAN AND RAYOPDF_SANITIZE)
    message(FATAL_ERROR "RAYOPDF_TSAN and RAYOPDF_SANITIZE exclude each other.")
endif()
if(RAYOPDF_SHARED)
    if(RAYOPDF_STATIC)
        message(FATAL_ERROR "RAYOPDF_SHARED and RAYOPDF_STATIC exclude each other.")
    endif()
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# The WebAssembly module (EMSCRIPTEN, tools/dev.py config wasm): the C ABI alone, on one thread
# (no threads linked; the C ABI converts on one page thread), without processes (the OCR add-on
# is never found: src/layout/ocr_process_none.cpp), so no command line, batch, serve or add-on.
if(EMSCRIPTEN)
    if(RAYOPDF_SHARED OR RAYOPDF_STATIC OR RAYOPDF_SANITIZE OR RAYOPDF_TSAN OR RAYOPDF_OCR_ADDON)
        message(FATAL_ERROR "An Emscripten build takes none of RAYOPDF_SHARED, _STATIC, _SANITIZE, _TSAN, _FUZZ, _OCR_ADDON.")
    endif()
else()
    # Threads for the batch modes' workers (--batch, --serve --json --workers=N).
    find_package(Threads REQUIRED)
endif()

# Apple's ld (Xcode 15 on) warns of a static library named twice on a link line, which CMake writes
# for a test that links a module and an object library depending on it; GNU ld says nothing.
if(APPLE)
    add_link_options(LINKER:-no_warn_duplicate_libraries)
endif()

# Flags of all own code: no exceptions and no RTTI anywhere (roadmap D1), warnings on. The same
# arithmetic and the same `char` on every target, so that the output is the same: no fused
# multiply-add (GCC fuses across statements on aarch64; x86-64 has no FMA in its base set, so this
# changes nothing there), and `char` signed (unsigned on Linux aarch64). Release builds on Linux
# also have no unwind tables: nothing unwinds (a throw of libstdc++'s ends the process with or
# without them), and the tables only take file space.
function(rayopdf_flags target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -fno-exceptions -fno-rtti -fno-math-errno -ffp-contract=off -fsigned-char
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
        if(RAYOPDF_SHARED AND NOT WIN32)
            target_compile_options(${target} PRIVATE -fvisibility=hidden)
        endif()
    endif()
    if(MINGW)
        # C99 printf and strtod from MinGW-w64 rather than msvcrt's, so numbers are written as on
        # Linux; every executable fully static (no libstdc++, libgcc or winpthread DLL beside it).
        target_compile_definitions(${target} PRIVATE __USE_MINGW_ANSI_STDIO=1)
        target_link_options(${target} PRIVATE -static)
        # GCC keeps the Windows x64 stack only 16-byte aligned, yet spills 256-bit registers of the
        # AVX2 kernels (common/cpu.h) with aligned moves (GCC bug 54412): the assembler writes every
        # aligned vector move as an unaligned one, which is as fast on aligned addresses.
        target_compile_options(${target} PRIVATE -Wa,-muse-unaligned-vector-move)
    endif()
    target_compile_definitions(${target} PRIVATE RAYOPDF_VERSION="${RAYOPDF_VERSION}")
endfunction()

# One static library per module directory src/<module>/ (every .cpp in it). Dependencies:
# common <- crypt <- pdf <- text, image <- layout <- api (the C++ API, its public header under
# include/) <- the command line; raster (paths and the rasterizer for glyph-shape recovery) <- text.
# `ocr` is the OCR add-on's own code (pre- and post-processing, protocol server; no inference
# runtime): only the add-on rayopdf-ocr, its tests and its fuzz target link it, never `rayopdf`.
set(RAYOPDF_MODULES common crypt pdf raster text image layout api ocr)
set(RAYOPDF_DEPENDS_crypt common)
set(RAYOPDF_DEPENDS_pdf crypt common)
set(RAYOPDF_DEPENDS_raster common)
set(RAYOPDF_DEPENDS_text pdf raster)
set(RAYOPDF_DEPENDS_image pdf)
set(RAYOPDF_DEPENDS_layout text image)
set(RAYOPDF_DEPENDS_api layout)
set(RAYOPDF_DEPENDS_ocr crypt common)
if(EMSCRIPTEN)
    list(REMOVE_ITEM RAYOPDF_MODULES ocr)
endif()
foreach(module ${RAYOPDF_MODULES})
    file(GLOB sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/${module}/*.cpp")
    # The OCR add-on's child process: ocr_process.cpp where processes exist, else the stand-in
    # that starts none.
    if(EMSCRIPTEN)
        list(FILTER sources EXCLUDE REGEX "/layout/ocr_process\\.cpp$")
    else()
        list(FILTER sources EXCLUDE REGEX "/layout/ocr_process_none\\.cpp$")
    endif()
    if(sources)
        add_library(rayopdf_${module} STATIC ${sources})
        target_include_directories(rayopdf_${module} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
        foreach(dependency ${RAYOPDF_DEPENDS_${module}})
            if(TARGET rayopdf_${dependency})
                target_link_libraries(rayopdf_${module} PUBLIC rayopdf_${dependency})
            endif()
        endforeach()
        rayopdf_flags(rayopdf_${module})
        if(module STREQUAL "common" AND NOT EMSCRIPTEN)
            target_link_libraries(rayopdf_common PUBLIC Threads::Threads)
        endif()
    endif()
endforeach()
if(TARGET rayopdf_api)
    target_include_directories(rayopdf_api PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
endif()

# The C ABI (include/rayopdf/rayopdf_c.h, src/api/c/), outside the api module's folder: an object
# library that test_api_c and the shared library link, never the command line.
if(TARGET rayopdf_api)
    add_library(rayopdf_c_obj OBJECT src/api/c/c_api.cpp)
    target_link_libraries(rayopdf_c_obj PUBLIC rayopdf_api)
    target_compile_definitions(rayopdf_c_obj PUBLIC RAYOPDF_C_BUILD)
    rayopdf_flags(rayopdf_c_obj)
endif()

# librayopdf.so (Linux), librayopdf.dylib (macOS) or rayopdf.dll (MinGW-w64): the C ABI and every
# module, with the C++ runtime linked in where it can be (not on macOS, whose libc++ is part of the
# system), so that it loads into any process (the Python package's wheels, tools/build_wheel.py).
# On Linux a version script exports the three functions of the C ABI alone (not the inline C++
# the objects instantiate); on macOS they are the only symbols of default visibility; on Windows
# only what RAYOPDF_API marks dllexport is exported.
if(TARGET rayopdf_api AND RAYOPDF_SHARED)
    add_library(rayopdf_c SHARED $<TARGET_OBJECTS:rayopdf_c_obj>)
    target_link_libraries(rayopdf_c PRIVATE rayopdf_api)
    rayopdf_flags(rayopdf_c)
    set_target_properties(rayopdf_c PROPERTIES OUTPUT_NAME rayopdf)
    if(WIN32)
        set_target_properties(rayopdf_c PROPERTIES PREFIX "")
    elseif(APPLE)
        target_link_options(rayopdf_c PRIVATE $<$<CONFIG:Release>:-Wl,-dead_strip>)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        set(script "${CMAKE_BINARY_DIR}/rayopdf_c.map")
        file(WRITE "${script}" "{\n  global: rayopdf_convert; rayopdf_output_free; rayopdf_version;\n  local: *;\n};\n")
        target_link_options(rayopdf_c PRIVATE -static-libstdc++ -static-libgcc "-Wl,--version-script=${script}"
            -Wl,--no-undefined $<$<CONFIG:Release>:-Wl,--gc-sections>)
        set_property(TARGET rayopdf_c APPEND PROPERTY LINK_DEPENDS "${script}")
    endif()
endif()

# The WebAssembly module of the npm package (packaging/npm/): rayopdf-core.mjs (an ES6 module
# factory for browsers and Node.js) and rayopdf-core.wasm, the C ABI and every module, built
# without SIMD (the scalar paths) on one thread.
if(TARGET rayopdf_api AND EMSCRIPTEN)
    add_executable(rayopdf_wasm $<TARGET_OBJECTS:rayopdf_c_obj>)
    target_link_libraries(rayopdf_wasm PRIVATE rayopdf_api)
    rayopdf_flags(rayopdf_wasm)
    set_target_properties(rayopdf_wasm PROPERTIES OUTPUT_NAME rayopdf-core SUFFIX ".mjs")
    target_link_options(rayopdf_wasm PRIVATE -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=web,node
        -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=8MB
        -sEXPORTED_FUNCTIONS=_rayopdf_convert,_rayopdf_output_free,_rayopdf_version,_malloc,_free
        -sEXPORTED_RUNTIME_METHODS=FS,UTF8ToString,stringToUTF8,lengthBytesUTF8,HEAPU8,HEAPU32)
endif()

# GNU ld (not gold, lld or a PE linker) takes the INSERT scripts below.
set(RAYOPDF_GNU_LD OFF)
if(NOT WIN32 AND NOT APPLE AND NOT EMSCRIPTEN AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    separate_arguments(rayopdf_linker_flags UNIX_COMMAND "${CMAKE_EXE_LINKER_FLAGS}")
    execute_process(COMMAND ${CMAKE_CXX_COMPILER} ${rayopdf_linker_flags} -Wl,--version
        OUTPUT_VARIABLE rayopdf_linker_version ERROR_QUIET)
    if(rayopdf_linker_version MATCHES "^GNU ld")
        set(RAYOPDF_GNU_LD ON)
    endif()
endif()

# lines.cpp without GCC's basic-block (SLP) vectorizer: it packs BuildLines' per-glyph boxes into
# vector registers and spends shuffles on every glyph (odl: 5.4 M fewer instructions without it).
# markdown_writer.cpp and math.cpp with a larger inlining budget for the unit than GCC's default
# 40 %: at the default, 120 and 130 calls went out of line for want of it, among them
# Writer::Inline's space before each word and the LaTeX rebuild's string and vector helpers
# (`-fopt-info-inline-missed` lists none at 60). math.cpp also inlines functions not declared
# inline up to -O3's size (max-inline-insns-auto 30 against -O2's 15): its builders' small helpers.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set_source_files_properties(src/layout/lines.cpp PROPERTIES COMPILE_OPTIONS "-fno-tree-slp-vectorize")
    set_source_files_properties(src/layout/markdown_writer.cpp PROPERTIES COMPILE_OPTIONS "--param=inline-unit-growth=60")
    set_source_files_properties(src/layout/math.cpp PROPERTIES COMPILE_OPTIONS
                                "--param=inline-unit-growth=60;--param=max-inline-insns-auto=30")
endif()

if(TARGET rayopdf_api AND NOT EMSCRIPTEN)
    add_executable(rayopdf src/cli/main.cpp)
    target_link_libraries(rayopdf PRIVATE rayopdf_api)
    rayopdf_flags(rayopdf)
    if(RAYOPDF_STATIC)
        if(WIN32 OR APPLE)
            message(FATAL_ERROR "RAYOPDF_STATIC is for Linux.")
        endif()
        # Relative relocations packed (DT_RELR): 53 KB less to read and apply at start. The static
        # binary carries its own start code, so this needs no newer glibc where it runs. x86-64
        # only: binutils 2.42's ld ignores the option for aarch64, with a warning.
        target_link_options(rayopdf PRIVATE -static-pie)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
            target_link_options(rayopdf PRIVATE $<$<CONFIG:Release>:-Wl,-z,pack-relative-relocs>)
        endif()
        # glibc start-up work the static binary has no use for (static_start.cpp).
        target_sources(rayopdf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/cli/static_start.cpp)
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND NOT APPLE)
        target_link_options(rayopdf PRIVATE -static-libstdc++ -static-libgcc)
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE)
        target_link_options(rayopdf PRIVATE $<$<CONFIG:Release>:-Wl,--gc-sections>)
    elseif(APPLE)
        target_link_options(rayopdf PRIVATE $<$<CONFIG:Release>:-Wl,-dead_strip>)
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
    if(RAYOPDF_GNU_LD AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
        # Release builds on x86-64 place the code fresh conversions run first, in the order of
        # src/hot.ld (generated by tools/hot_sections.py on x86-64; an INSERT script, so the default
        # layout stays for the rest): fewer 64 KiB windows of code to fault in. Names it lists that
        # no longer exist match nothing.
        set(script "${CMAKE_CURRENT_SOURCE_DIR}/src/hot.ld")
        target_link_options(rayopdf PRIVATE "$<$<CONFIG:Release>:-Wl,-T,${script}>")
        set_property(TARGET rayopdf APPEND PROPERTY LINK_DEPENDS "${script}")
    endif()
endif()

# The C++ API as one library for the release's lib archives (tools/package.py): librayopdf.a in the
# build directory, every module the command line links merged by an `ar -M` script, on macOS by
# Apple's `libtool -static` (whose ar has no scripts; ZERO_AR_DATE keeps the member times at 0 so
# that two builds give the same bytes). Built by the static, Windows and macOS builds, whose
# objects need no shared runtime of their own.
if(TARGET rayopdf_api AND (RAYOPDF_STATIC OR WIN32 OR (APPLE AND NOT RAYOPDF_SHARED)))
    set(bundle "${CMAKE_BINARY_DIR}/librayopdf.a")
    set(bundle_script "${CMAKE_BINARY_DIR}/librayopdf.mri")
    set(bundle_libraries)
    set(bundle_files)
    set(content "CREATE ${bundle}\n")
    foreach(module api layout text image pdf raster crypt common)
        string(APPEND content "ADDLIB $<TARGET_FILE:rayopdf_${module}>\n")
        list(APPEND bundle_libraries rayopdf_${module})
        list(APPEND bundle_files $<TARGET_FILE:rayopdf_${module}>)
    endforeach()
    if(APPLE)
        add_custom_command(OUTPUT "${bundle}"
            COMMAND ${CMAKE_COMMAND} -E remove -f "${bundle}"
            COMMAND ${CMAKE_COMMAND} -E env ZERO_AR_DATE=1 libtool -static -no_warning_for_no_symbols
                    -o "${bundle}" ${bundle_files}
            DEPENDS ${bundle_libraries}
            COMMENT "Merging the modules into librayopdf.a"
            VERBATIM)
    else()
        string(APPEND content "SAVE\nEND\n")
        file(GENERATE OUTPUT "${bundle_script}" CONTENT "${content}")
        add_custom_command(OUTPUT "${bundle}"
            COMMAND ${CMAKE_COMMAND} -E remove -f "${bundle}"
            COMMAND sh -c "\"${CMAKE_AR}\" -M < \"${bundle_script}\""
            DEPENDS ${bundle_libraries} "${bundle_script}"
            COMMENT "Merging the modules into librayopdf.a"
            VERBATIM)
    endif()
    add_custom_target(rayopdf_bundle ALL DEPENDS "${bundle}")
endif()

# The OCR add-on rayopdf-ocr (docs/development/ocr-engine.md): src/ocr/addon/ plus the inference
# runtime ncnn, a static build of it under RAYOPDF_OCR_RUNTIME_DIR (include/, lib/), as
# docker/ocr.Dockerfile installs it (tools/dev.py configs ocr, ocr-windows).
option(RAYOPDF_OCR_ADDON "Build the OCR add-on rayopdf-ocr" OFF)
set(RAYOPDF_OCR_RUNTIME_DIR "" CACHE PATH "Static build of ncnn (include/, lib/)")
set(RAYOPDF_OCR_TIER "small" CACHE STRING "Default model tier of rayopdf-ocr: tiny or small")
set(RAYOPDF_OCR_MODELS "" CACHE PATH "Model folder (with <tier>/) for tests/verify_ocr.py")
if(RAYOPDF_OCR_ADDON)
    if(NOT RAYOPDF_OCR_RUNTIME_DIR)
        message(FATAL_ERROR "RAYOPDF_OCR_ADDON needs RAYOPDF_OCR_RUNTIME_DIR.")
    endif()
    add_executable(rayopdf-ocr src/ocr/addon/main.cpp src/ocr/addon/runtime_ncnn.cpp)
    target_link_libraries(rayopdf-ocr PRIVATE rayopdf_ocr)
    rayopdf_flags(rayopdf-ocr)
    target_compile_definitions(rayopdf-ocr PRIVATE RAYOPDF_OCR_TIER="${RAYOPDF_OCR_TIER}")
    # In a folder of its own: the core's add-on discovery looks beside the running executable, and
    # the core tests in this build directory must not find (and start) the real add-on.
    set_target_properties(rayopdf-ocr PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/addon")
    target_include_directories(rayopdf-ocr SYSTEM PRIVATE "${RAYOPDF_OCR_RUNTIME_DIR}/include/ncnn")
    target_link_libraries(rayopdf-ocr PRIVATE "${RAYOPDF_OCR_RUNTIME_DIR}/lib/libncnn.a" Threads::Threads)
    if(RAYOPDF_STATIC)
        target_link_options(rayopdf-ocr PRIVATE -static-pie)
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND NOT APPLE)
        target_link_options(rayopdf-ocr PRIVATE -static-libstdc++ -static-libgcc)
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE)
        target_link_options(rayopdf-ocr PRIVATE $<$<CONFIG:Release>:-Wl,--gc-sections>)
    endif()
endif()

if(RAYOPDF_TESTS AND EMSCRIPTEN)
    enable_testing()
    # The npm package over the WebAssembly module (tests/verify_wasm.mjs), run by the Node.js the
    # Emscripten toolchain names as its emulator. The core tests start processes and threads and
    # run in the native configs only.
    if(NOT CMAKE_CROSSCOMPILING_EMULATOR)
        message(FATAL_ERROR "The Emscripten toolchain names no Node.js (CMAKE_CROSSCOMPILING_EMULATOR).")
    endif()
    list(GET CMAKE_CROSSCOMPILING_EMULATOR 0 rayopdf_node)
    add_test(NAME wasm COMMAND ${rayopdf_node} "${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_wasm.mjs"
        --module $<TARGET_FILE:rayopdf_wasm>)
elseif(RAYOPDF_TESTS)
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
    if(TARGET test_api_c)
        target_link_libraries(test_api_c PRIVATE rayopdf_c_obj)
    endif()
    # The C++ API's example (src/api/example/, shipped in the lib archives), built with the tests
    # so that it always compiles against the public header.
    if(TARGET rayopdf_api)
        add_executable(rayopdf_api_example src/api/example/example.cpp)
        target_link_libraries(rayopdf_api_example PRIVATE rayopdf_api)
        rayopdf_flags(rayopdf_api_example)
    endif()
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
    # The shared library as a C program on Windows loads it (tests/c_api_smoke.c, under Wine in the
    # windows config): LoadLibrary("rayopdf.dll") from the folder both are built in.
    if(TARGET rayopdf_c AND WIN32)
        add_executable(c_api_smoke tests/c_api_smoke.c)
        target_include_directories(c_api_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
        target_compile_options(c_api_smoke PRIVATE -Wall -Wextra)
        target_link_options(c_api_smoke PRIVATE -static)
        add_dependencies(c_api_smoke rayopdf_c)
        add_test(NAME c_api_smoke COMMAND c_api_smoke)
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
    # The Python package over librayopdf.so or librayopdf.dylib (tests/verify_python.py; the wheel
    # and macos-lib configs). ELF libraries are read with the toolchain's objdump, Mach-O with nm.
    if(Python3_Interpreter_FOUND AND TARGET rayopdf_c AND NOT WIN32)
        set(rayopdf_objdump)
        if(NOT APPLE)
            set(rayopdf_objdump --objdump "${CMAKE_OBJDUMP}")
        endif()
        add_test(NAME python COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_python.py"
            --library $<TARGET_FILE:rayopdf_c> ${rayopdf_objdump})
        set_tests_properties(python PROPERTIES ENVIRONMENT "${rayopdf_test_environment}")
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
