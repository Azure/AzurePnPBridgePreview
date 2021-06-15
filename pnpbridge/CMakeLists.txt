# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

cmake_minimum_required(VERSION 2.8.11)

project(azure_iot_pnp_bridge)

if (POLICY CMP0042)
    cmake_policy(SET CMP0042 NEW)
endif()

#set(run_e2e_tests OFF CACHE BOOL "skip e2e tests" FORCE)
# TODO: Skip azure c sdk unit tests

#set(run_unittests OFF CACHE BOOL "skip unit tests" FORCE)
set(skip_samples ON CACHE BOOL "skip building samples" FORCE)

#do not add or build any tests of the dependencies
set(original_run_e2e_tests ${run_e2e_tests})
set(original_run_unittests ${run_unittests})

set(run_e2e_tests OFF)
set(run_unittests OFF)

set(compileOption_C "" CACHE STRING "passes a string to the command line of the C compiler")
set(compileOption_CXX "" CACHE STRING "passes a string to the command line of the C++ compiler")
set(linkerOption "" CACHE STRING "passes a string to the shared and exe linker options of the C compiler")

# Include the common build rules for the C SDK
include("${CMAKE_CURRENT_LIST_DIR}/deps/azure-iot-sdk-c-pnp/c-utility/configs/azure_iot_build_rules.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/deps/azure-iot-sdk-c-pnp/c-utility/configs/azure_c_shared_utilityFunctions.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/deps/azure-iot-sdk-c-pnp/configs/azure_iot_sdksFunctions.cmake")

#set(use_installed_dependencies OFF)

if(${original_run_e2e_tests} OR ${original_run_unittests})
    set(SHARED_UTIL_REAL_TEST_FOLDER ${CMAKE_CURRENT_LIST_DIR}/deps/azure-iot-sdk-c-pnp/c-utility/tests/real_test_files CACHE INTERNAL "this is what needs to be included when doing test sources" FORCE)
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/deps/azure-iot-sdk-c-pnp/c-utility/testtools)
	enable_testing()
endif()

# Now add the pnp C sdk with test dir flags disabled
add_subdirectory(deps/azure-iot-sdk-c-pnp)

set(run_e2e_tests ${original_run_e2e_tests})
set(run_unittests ${original_run_unittests})

option(run_e2e_tests "set run_e2e_tests to ON to run e2e tests (default is OFF)" OFF)
option(run_unittests "set run_unittests to ON to run unittests (default is OFF)" OFF)
option(run_int_tests "set run_int_tests to ON to integration tests (default is OFF)." OFF)

# Enable IoT SDK to act as a module for Edge
if(${use_edge_modules})
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DUSE_EDGE_MODULES")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DUSE_EDGE_MODULES")
endif()

# Enable optional trusted cert
if(${use_sample_trusted_cert})
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DSET_TRUSTED_CERT")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DSET_TRUSTED_CERT")
endif()

#Enable DPS Provisioning
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DUSE_PROV_MODULE_FULL")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DUSE_PROV_MODULE_FULL")

include (CTest)

if (MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W4 /wd4232")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /W4 /wd4232")
    # Make warning as error
    add_definitions(/WX)
else()
    # Make warning as error
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Werror")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Werror")
endif()

set_platform_files("${CMAKE_CURRENT_LIST_DIR}/deps/azure-iot-sdk-c-pnp/c-utility")

IF(WIN32)
    # windows needs this define
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
endif()

add_subdirectory(src)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

#add_subdirectory(deps)
