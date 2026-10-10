# =============================================================================
# libhotkv: Official HotKV C Client Library
# Copyright (c) 2026 HotKV Ltd
# Licensed under the Apache License, Version 2.0. See LICENSE in the project root.
# =============================================================================

cmake_minimum_required(VERSION 3.16)

project(libhotkv
    VERSION 1.0.0
    DESCRIPTION "A high-performance C client for HotKV, a RESP2/RESP3-compatible in-memory data store with built-in AI and LLM-serving features."
    HOMEPAGE_URL "https://hotkv.com"
    LANGUAGES C
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(HOTKV_USE_SSL      "Enable TLS support via OpenSSL"       OFF)
option(HOTKV_BUILD_TESTS  "Build unit tests"                     ON)
option(HOTKV_BUILD_EXAMPLES "Build example programs"             ON)

# ---------------------------------------------------------------------------
# Compiler warnings
# ---------------------------------------------------------------------------
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wstrict-prototypes
                        -Wmissing-prototypes -Wshadow -Wconversion
                        -Wno-unused-parameter)
elseif(MSVC)
    add_compile_options(/W4 /wd4100 /wd4996)
endif()

# ---------------------------------------------------------------------------
# Core library sources
# ---------------------------------------------------------------------------
set(HOTKV_SOURCES
    src/hotkv.c
    src/hotkv_resp.c
    src/hotkv_commands.c
    src/hotkv_datastructs.c
    src/hotkv_enterprise.c
    src/hotkv_ai.c
    src/hotkv_url.c
    src/hotkv_num.c
    src/hotkv_crc16.c
    src/hotkv_pool.c
)

if(HOTKV_USE_SSL)
    list(APPEND HOTKV_SOURCES src/hotkv_ssl.c)
endif()

# ---------------------------------------------------------------------------
# Library target
# ---------------------------------------------------------------------------
add_library(hotkv ${HOTKV_SOURCES})
add_library(hotkv::hotkv ALIAS hotkv)

# Shared builds are libhotkv.so.1 / libhotkv.1.dylib, with libhotkv.so as the
# development link. Bump the major version (SOVERSION) on an ABI break.
set_target_properties(hotkv PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

target_include_directories(hotkv
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# When building the library, define HOTKV_BUILDING for correct dllexport.
# When building static, define HOTKV_STATIC so consumers see plain symbols.
target_compile_definitions(hotkv PRIVATE HOTKV_BUILDING)
get_target_property(_hotkv_type hotkv TYPE)
if(_hotkv_type STREQUAL "STATIC_LIBRARY")
    target_compile_definitions(hotkv PUBLIC HOTKV_STATIC)
endif()

# Platform-specific link dependencies
if(WIN32)
    target_link_libraries(hotkv PRIVATE ws2_32)
    target_compile_definitions(hotkv PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

if(UNIX)
    target_link_libraries(hotkv PRIVATE m)
    # The connection pool uses pthread mutexes and condition variables, which
    # need -lpthread on glibc older than 2.34.
    find_package(Threads REQUIRED)
    target_link_libraries(hotkv PRIVATE Threads::Threads)
endif()

if(UNIX AND NOT APPLE)
    include(CheckFunctionExists)
    check_function_exists(clock_gettime HAVE_CLOCK_GETTIME)
    if(NOT HAVE_CLOCK_GETTIME)
        target_link_libraries(hotkv PRIVATE rt)
    endif()
endif()

# ---------------------------------------------------------------------------
# Optional: OpenSSL
# ---------------------------------------------------------------------------
if(HOTKV_USE_SSL)
    find_package(OpenSSL REQUIRED)
    target_link_libraries(hotkv PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(hotkv PUBLIC HOTKV_USE_SSL=1)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(HOTKV_BUILD_TESTS)
    enable_testing()
    add_executable(test_resp   tests/test_resp.c)
    add_executable(test_commands tests/test_commands.c)
    add_executable(test_pool   tests/test_pool.c)
    add_executable(test_url    tests/test_url.c)
    add_executable(test_crc16  tests/test_crc16.c)
    add_executable(test_enterprise tests/test_enterprise.c)
    add_executable(test_ai     tests/test_ai.c)
    add_executable(test_num    tests/test_num.c)

    target_link_libraries(test_resp   PRIVATE hotkv)
    target_link_libraries(test_commands PRIVATE hotkv)
    target_link_libraries(test_pool   PRIVATE hotkv)
    target_link_libraries(test_url    PRIVATE hotkv)
    target_link_libraries(test_crc16  PRIVATE hotkv)
    target_link_libraries(test_enterprise PRIVATE hotkv)
    target_link_libraries(test_ai     PRIVATE hotkv)
    target_link_libraries(test_num    PRIVATE hotkv)

    add_test(NAME resp_parser   COMMAND test_resp)
    add_test(NAME commands      COMMAND test_commands)
    add_test(NAME pool          COMMAND test_pool)
    add_test(NAME url_parser    COMMAND test_url)
    add_test(NAME crc16         COMMAND test_crc16)
    add_test(NAME enterprise_commands COMMAND test_enterprise)
    add_test(NAME ai_helpers    COMMAND test_ai)
    add_test(NAME number_format COMMAND test_num)

    # Exact bytes sent for numbers and a few commands that cannot run live,
    # checked against a fork()ed fake server (POSIX; skips on Windows).
    add_executable(test_wire   tests/test_wire.c)
    target_link_libraries(test_wire   PRIVATE hotkv)
    add_test(NAME wire_format COMMAND test_wire)
    set_tests_properties(wire_format PROPERTIES SKIP_RETURN_CODE 77)

    # Integration tests: only registered (and only actually do anything)
    # when HOTKV_TEST_URL is set in the environment; ctest still runs the
    # binary, which exits 77 (skip) and prints why when the variable is unset.
    add_executable(test_integration tests/test_integration.c)
    target_link_libraries(test_integration PRIVATE hotkv)
    add_test(NAME integration COMMAND test_integration)
    set_tests_properties(integration PROPERTIES SKIP_RETURN_CODE 77)

    # Value-checked live tests for every typed core wrapper (same gating).
    add_executable(test_live_core tests/test_live_core.c)
    target_link_libraries(test_live_core PRIVATE hotkv)
    add_test(NAME live_core COMMAND test_live_core)
    set_tests_properties(live_core PROPERTIES SKIP_RETURN_CODE 77)

    # Value-checked live tests for every typed enterprise / AI wrapper
    # (needs a licensed server; skipped without HOTKV_TEST_URL).
    add_executable(test_live_enterprise tests/test_live_enterprise.c)
    target_link_libraries(test_live_enterprise PRIVATE hotkv)
    add_test(NAME live_enterprise COMMAND test_live_enterprise)
    set_tests_properties(live_enterprise PROPERTIES SKIP_RETURN_CODE 77)

    # TLS and authentication against a real TLS server (HOTKV_TEST_TLS_*);
    # skipped without them or when built without HOTKV_USE_SSL.
    add_executable(test_tls tests/test_tls.c)
    target_link_libraries(test_tls PRIVATE hotkv)
    add_test(NAME tls COMMAND test_tls)
    set_tests_properties(tls PROPERTIES SKIP_RETURN_CODE 77)

    # Connects to a remote TLS + password server; skipped unless
    # HOTKV_REMOTE_HOST is set (see the header of tests/test_remote.c).
    add_executable(test_remote tests/test_remote.c)
    target_link_libraries(test_remote PRIVATE hotkv)
    add_test(NAME remote COMMAND test_remote)
    set_tests_properties(remote PROPERTIES SKIP_RETURN_CODE 77)
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------
if(HOTKV_BUILD_EXAMPLES)
    add_executable(example_basic    examples/basic.c)
    add_executable(example_pipeline examples/pipeline.c)

    target_link_libraries(example_basic    PRIVATE hotkv)
    target_link_libraries(example_pipeline PRIVATE hotkv)

    if(HOTKV_USE_SSL)
        add_executable(example_tls examples/tls.c)
        target_link_libraries(example_tls PRIVATE hotkv)
    endif()
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS hotkv
    EXPORT hotkvTargets
    LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME  DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES include/hotkv.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT hotkvTargets
    FILE hotkvTargets.cmake
    NAMESPACE hotkv::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/hotkv
)

# find_package(hotkv) support: `target_link_libraries(app PRIVATE hotkv::hotkv)`.
include(CMakePackageConfigHelpers)
set(HOTKV_WITH_SSL ${HOTKV_USE_SSL})
configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/hotkvConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/hotkvConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/hotkv
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/hotkvConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/hotkvConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/hotkvConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/hotkv
)

install(FILES LICENSE README.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
)

# ---------------------------------------------------------------------------
# pkg-config
#
# The .pc file lives in <libdir>/pkgconfig, so the install prefix is the
# relative path from that directory (../.. for lib/, ../../.. for a multiarch
# lib/x86_64-linux-gnu/). That keeps the file valid wherever the install tree
# is extracted.
# ---------------------------------------------------------------------------
file(RELATIVE_PATH HOTKV_PC_REL_PREFIX
    "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/pkgconfig" "${CMAKE_INSTALL_PREFIX}")

set(HOTKV_PC_CFLAGS "")
if(_hotkv_type STREQUAL "STATIC_LIBRARY")
    string(APPEND HOTKV_PC_CFLAGS " -DHOTKV_STATIC")
endif()
set(HOTKV_PC_REQUIRES "")
if(HOTKV_USE_SSL)
    string(APPEND HOTKV_PC_CFLAGS " -DHOTKV_USE_SSL=1")
    set(HOTKV_PC_REQUIRES "Requires.private: openssl")
endif()
if(WIN32)
    set(HOTKV_PC_LIBS_PRIVATE "-lws2_32")
else()
    set(HOTKV_PC_LIBS_PRIVATE "-lm -pthread")
endif()
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/libhotkv.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/libhotkv.pc
    @ONLY
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/libhotkv.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)
