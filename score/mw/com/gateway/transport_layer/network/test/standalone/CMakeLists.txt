# *******************************************************************************
# Copyright (c) 2026 Contributors to the Eclipse Foundation
#
# See the NOTICE file(s) distributed with this work for additional
# information regarding copyright ownership.
#
# This program and the accompanying materials are made available under the
# terms of the Apache License Version 2.0 which is available at
# https://www.apache.org/licenses/LICENSE-2.0
#
# SPDX-License-Identifier: Apache-2.0
# *******************************************************************************
#
# Standalone verification harness for the S-CORE independent parts of the network transport (wire format,
# RpcChannel, ZenohPicoBus, CycloneDdsBus). The production build is Bazel (see ../../BUILD); this harness exists so
# the middleware backends can be exercised against real zenoh-pico / Cyclone DDS without the full S-CORE toolchain.
#
#   cmake -S . -B build -DGTEST_SOURCE_DIR=<googletest> -DCMAKE_PREFIX_PATH=<zenoh-pico;cyclonedds install>
#   cmake --build build && ctest --test-dir build --output-on-failure
#
cmake_minimum_required(VERSION 3.16)
project(score_gateway_network_transport_standalone LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SANITIZE_THREAD "Build with ThreadSanitizer" OFF)
option(SANITIZE_ADDRESS "Build with AddressSanitizer and UBSan" OFF)
# By default a backend whose library is not found is skipped with a warning. REQUIRE_<backend>=ON makes it an error.
option(REQUIRE_ZENOH_PICO "Fail if zenoh-pico is not found" OFF)
option(REQUIRE_CYCLONE_DDS "Fail if Cyclone DDS is not found" OFF)
set(GTEST_SOURCE_DIR "" CACHE PATH "googletest source tree")

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../../.." ABSOLUTE)
set(NET_DIR "${REPO_ROOT}/score/mw/com/gateway/transport_layer/network")

set(STRICT_WARNINGS -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wold-style-cast
                    -Wnon-virtual-dtor -Woverloaded-virtual -Werror)

if(SANITIZE_THREAD)
  add_compile_options(-fsanitize=thread -g -O1)
  add_link_options(-fsanitize=thread)
endif()
if(SANITIZE_ADDRESS)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1)
  add_link_options(-fsanitize=address,undefined)
endif()

find_package(Threads REQUIRED)

if(NOT GTEST_SOURCE_DIR)
  message(FATAL_ERROR "Set GTEST_SOURCE_DIR to a googletest checkout")
endif()
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
add_subdirectory("${GTEST_SOURCE_DIR}" googletest EXCLUDE_FROM_ALL)

enable_testing()

# --- core: wire format + RpcChannel (no middleware, no S-CORE) -------------------------------------------------
add_library(network_core STATIC
  "${NET_DIR}/core/wire_format.cpp"
  "${NET_DIR}/core/rpc_channel.cpp"
  "${NET_DIR}/core/type_id_check.cpp"
  "${NET_DIR}/core/loopback_bus.cpp")
target_include_directories(network_core PUBLIC "${REPO_ROOT}")
target_compile_options(network_core PRIVATE ${STRICT_WARNINGS})
target_link_libraries(network_core PUBLIC Threads::Threads)

add_executable(network_core_test
  "${NET_DIR}/core/wire_format_test.cpp"
  "${NET_DIR}/core/rpc_channel_test.cpp"
  "${NET_DIR}/core/type_id_check_test.cpp")
target_compile_options(network_core_test PRIVATE ${STRICT_WARNINGS})
target_link_libraries(network_core_test PRIVATE network_core GTest::gtest_main)
add_test(NAME network_core_test COMMAND network_core_test)

# --- zenoh-pico backend ---------------------------------------------------------------------------------------
# The exported target carries the platform defines (ZENOH_LINUX, ...) the headers need.
find_package(zenohpico CONFIG QUIET)
if(zenohpico_FOUND AND EXISTS "${NET_DIR}/zenoh/zenoh_pico_bus.cpp")
  add_library(zenoh_pico_bus STATIC "${NET_DIR}/zenoh/zenoh_pico_bus.cpp")
  target_include_directories(zenoh_pico_bus PUBLIC "${REPO_ROOT}")
  # score/mw/log stand-in (writes to stderr); the Bazel target uses baselibs.
  target_include_directories(zenoh_pico_bus PRIVATE "${NET_DIR}/test/standalone/stub")
  target_compile_options(zenoh_pico_bus PRIVATE ${STRICT_WARNINGS})
  target_link_libraries(zenoh_pico_bus PUBLIC network_core zenohpico::zenohpico_shared)
  set(HAVE_ZENOH_PICO ON)
elseif(REQUIRE_ZENOH_PICO)
  message(FATAL_ERROR "zenoh-pico not found (REQUIRE_ZENOH_PICO=ON): add its install prefix to CMAKE_PREFIX_PATH")
else()
  message(WARNING "zenoh-pico not found: ZenohPicoBus is not built")
endif()

# --- Cyclone DDS backend --------------------------------------------------------------------------------------
find_package(CycloneDDS CONFIG QUIET)
if(CycloneDDS_FOUND AND EXISTS "${NET_DIR}/dds/cyclone_dds_bus.cpp")
  add_library(cyclone_dds_bus STATIC
    "${NET_DIR}/dds/cyclone_dds_bus.cpp"
    "${NET_DIR}/dds/generated/gateway_frame.c")
  target_include_directories(cyclone_dds_bus PUBLIC "${REPO_ROOT}" "${NET_DIR}/dds/generated")
  set_source_files_properties("${NET_DIR}/dds/cyclone_dds_bus.cpp" PROPERTIES COMPILE_OPTIONS "${STRICT_WARNINGS}")
  target_link_libraries(cyclone_dds_bus PUBLIC network_core CycloneDDS::ddsc)
  set(HAVE_CYCLONE_DDS ON)
elseif(REQUIRE_CYCLONE_DDS)
  message(FATAL_ERROR "Cyclone DDS not found (REQUIRE_CYCLONE_DDS=ON): add its install prefix to CMAKE_PREFIX_PATH")
else()
  message(WARNING "Cyclone DDS not found: CycloneDdsBus is not built")
endif()

# --- integration tests: real middleware, two processes ------------------------------------------------------
if((HAVE_ZENOH_PICO OR HAVE_CYCLONE_DDS) AND EXISTS "${NET_DIR}/test/standalone/bus_peer.cpp")
  add_executable(bus_peer "${NET_DIR}/test/standalone/bus_peer.cpp")
  target_compile_options(bus_peer PRIVATE ${STRICT_WARNINGS})
  target_link_libraries(bus_peer PRIVATE network_core)
  if(HAVE_ZENOH_PICO)
    target_compile_definitions(bus_peer PRIVATE HAVE_ZENOH_PICO=1)
    target_link_libraries(bus_peer PRIVATE zenoh_pico_bus)
  endif()
  if(HAVE_CYCLONE_DDS)
    target_compile_definitions(bus_peer PRIVATE HAVE_CYCLONE_DDS=1)
    target_link_libraries(bus_peer PRIVATE cyclone_dds_bus)
  endif()

  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  if(HAVE_ZENOH_PICO)
    add_test(NAME zenoh_pico_two_process
             COMMAND Python3::Interpreter "${NET_DIR}/test/standalone/run_two_process.py" $<TARGET_FILE:bus_peer> zenoh)
    set_tests_properties(zenoh_pico_two_process PROPERTIES TIMEOUT 120)
  endif()
  if(HAVE_CYCLONE_DDS)
    add_test(NAME cyclone_dds_two_process
             COMMAND Python3::Interpreter "${NET_DIR}/test/standalone/run_two_process.py" $<TARGET_FILE:bus_peer> dds)
    set_tests_properties(cyclone_dds_two_process PROPERTIES TIMEOUT 120)
  endif()
endif()
