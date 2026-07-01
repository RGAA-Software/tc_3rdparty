#
# Copyright (c) 2017-2023 zhllxt
#
# author   : zhllxt
# email    : 37792738@qq.com
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#

cmake_minimum_required (VERSION 3.11)

# remove last end of "/"
string(REGEX REPLACE "/$" "" CMAKELISTS_DIR_PATH ${CMAKE_CURRENT_SOURCE_DIR})

# get current relative dir name and set target name
string(REGEX REPLACE ".*/(.*)" "\\1" CMAKELISTS_DIR_NAME ${CMAKELISTS_DIR_PATH})

# set root directory
set(ASIO2_ROOT_DIR ${CMAKELISTS_DIR_PATH} CACHE PATH "asio2 root directory" FORCE)
if(NOT ASIO2_ROOT_DIR)
    message(FATAL_ERROR "asio2 root directory is not set!")
endif()
message(STATUS "ASIO2_ROOT_DIR = ${ASIO2_ROOT_DIR}")

#-------------------------------------------------------------------------------
function (DoGroupSources curdir rootdir foldername folder)
    file (GLOB children RELATIVE ${ASIO2_ROOT_DIR}/${curdir} ${ASIO2_ROOT_DIR}/${curdir}/*)
    foreach (child ${children})
        if (IS_DIRECTORY ${ASIO2_ROOT_DIR}/${curdir}/${child})
            DoGroupSources (${curdir}/${child} ${rootdir} ${foldername} ${folder})
        elseif (${child} STREQUAL "CMakeLists.txt")
            source_group("" FILES ${ASIO2_ROOT_DIR}/${curdir}/${child})
        else()
            string (REGEX REPLACE ^${rootdir} ${folder} groupname ${curdir})
            string (REPLACE "/" "\\" groupname ${groupname})
            source_group (${foldername}${groupname} FILES ${ASIO2_ROOT_DIR}/${curdir}/${child})
        endif()
    endforeach()
endfunction()

function (GroupSources curdir folder)
    string (FIND ${curdir} "/" Separator)
    if(${Separator} EQUAL -1)
        set(Separator 0)
    else ()
        math(EXPR Separator "${Separator} + 1")
    endif()
    string (SUBSTRING ${curdir} ${Separator} -1 foldername)
    DoGroupSources (${curdir} ${curdir} ${foldername} ${folder})
endfunction()

#-------------------------------------------------------------------------------
# project
#-------------------------------------------------------------------------------
project (asio2 VERSION 3.0 LANGUAGES CXX)

include(GNUInstallDirs)

set_property (GLOBAL PROPERTY USE_FOLDERS ON)

option(BUILD_EXAMPLES "asio2: Build examples" OFF)
option(BUILD_TESTS "asio2: Build tests" OFF)

set(ASIO2_DISABLE_HEADER_ONLY OFF CACHE BOOL "asio2: Disable standalone asio (use boost::asio instead)")
set(ASIO2_ENABLE_SSL OFF CACHE BOOL "asio2: Enable SSL/TLS support (link prebuilt OpenSSL)")

set(OPENSSL_LIBS_FILE "")
set(GENERAL_LIBS_FILE "")
set(OPENSSL_INCS_DIR "")
set(OPENSSL_LIBS_DIR "")

if(NOT CMAKE_INSTALL_INCLUDEDIR)
    set(CMAKE_INSTALL_INCLUDEDIR "include" CACHE STRING "cmake_install_includedir" FORCE)
endif()

if(NOT CMAKE_INSTALL_LIBDIR)
    set(CMAKE_INSTALL_LIBDIR "lib" CACHE STRING "cmake_install_libdir" FORCE)
endif()

set(ASIO2_ARCH "x64")
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(ASIO2_ARCH "x86")
endif()
message(STATUS "asio2: target architecture = ${ASIO2_ARCH}")

set(OPENSSL_INCS_DIR /3rd/openssl/include)

IF (CMAKE_SYSTEM_NAME MATCHES "Linux")
    set(OPENSSL_LIBS_FILE libssl.a libcrypto.a)
    set(GENERAL_LIBS_FILE -lpthread -lrt -ldl)
    set(OPENSSL_LIBS_DIR /3rd/openssl/prebuilt/linux/${ASIO2_ARCH})
ELSEIF (CMAKE_SYSTEM_NAME MATCHES "Windows")
    set(OPENSSL_LIBS_FILE libssl.lib libcrypto.lib Crypt32.lib)
    set(GENERAL_LIBS_FILE ws2_32 mswsock)
    set(OPENSSL_LIBS_DIR /3rd/openssl/prebuilt/windows/${ASIO2_ARCH})
ELSEIF (CMAKE_SYSTEM_NAME MATCHES "FreeBSD")
    set(OPENSSL_LIBS_FILE libssl.a libcrypto.a)
    set(GENERAL_LIBS_FILE -lpthread -lrt -ldl)
    set(OPENSSL_LIBS_DIR /3rd/openssl/prebuilt/freebsd/${ASIO2_ARCH})
ELSEIF (CMAKE_SYSTEM_NAME MATCHES "Darwin")
    set(OPENSSL_LIBS_FILE libssl.a libcrypto.a)
    set(GENERAL_LIBS_FILE -lpthread -ldl)
    set(OPENSSL_LIBS_DIR /3rd/openssl/prebuilt/mac)
ELSE ()
    set(OPENSSL_LIBS_FILE libssl.a libcrypto.a)
    set(GENERAL_LIBS_FILE -lpthread -lrt -ldl)
    set(OPENSSL_LIBS_DIR /3rd/openssl/prebuilt/default)
ENDIF ()

set(ASIO2_OPENSSL_INCS_DIR ${OPENSSL_INCS_DIR} CACHE PATH "asio2 openssl include dir" FORCE)
set(ASIO2_OPENSSL_LIBS_DIR ${OPENSSL_LIBS_DIR} CACHE PATH "asio2 openssl lib dir" FORCE)

message(STATUS "ASIO2_OPENSSL_INCS_DIR = ${ASIO2_OPENSSL_INCS_DIR}")
message(STATUS "ASIO2_OPENSSL_LIBS_DIR = ${ASIO2_OPENSSL_LIBS_DIR}")
message(STATUS "OPENSSL_LIBS_FILE = ${OPENSSL_LIBS_FILE}")
message(STATUS "GENERAL_LIBS_FILE = ${GENERAL_LIBS_FILE}")
message(STATUS "CMAKE_INSTALL_INCLUDEDIR = ${CMAKE_INSTALL_INCLUDEDIR}")
message(STATUS "CMAKE_INSTALL_LIBDIR = ${CMAKE_INSTALL_LIBDIR}")

# ---------------------------------------------------------------------------------------
# interface config
# ---------------------------------------------------------------------------------------
add_library(asio2 INTERFACE)
add_library(asio2::asio2 ALIAS asio2)

set_target_properties(asio2 PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    EXPORT_NAME asio2
)

target_include_directories(asio2 INTERFACE
    $<BUILD_INTERFACE:${ASIO2_ROOT_DIR}/include/>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/asio2/include/>
)

if(NOT ASIO2_DISABLE_HEADER_ONLY)
    target_include_directories(asio2 INTERFACE
        $<BUILD_INTERFACE:${ASIO2_ROOT_DIR}/3rd/>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/asio2/3rd/>
    )
else()
    target_compile_definitions(asio2 INTERFACE ASIO2_DISABLE_HEADER_ONLY)
    message(STATUS "asio2: ASIO2_DISABLE_HEADER_ONLY macro will defined")
    message(STATUS "asio2: standalone asio disabled (use boost::asio)")
endif()

if(ASIO2_ENABLE_SSL)
    target_compile_definitions(asio2 INTERFACE ASIO2_ENABLE_SSL)
    target_include_directories(asio2 INTERFACE
        $<BUILD_INTERFACE:${ASIO2_ROOT_DIR}/${ASIO2_OPENSSL_INCS_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/asio2/${ASIO2_OPENSSL_INCS_DIR}>
    )
    target_link_directories(asio2 INTERFACE
        $<BUILD_INTERFACE:${ASIO2_ROOT_DIR}/${ASIO2_OPENSSL_LIBS_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/asio2/${ASIO2_OPENSSL_LIBS_DIR}>
    )
    target_link_libraries(asio2 INTERFACE ${OPENSSL_LIBS_FILE})
    message(STATUS "asio2: ASIO2_ENABLE_SSL macro will defined")
    message(STATUS "asio2: SSL/TLS support enabled (linked with prebuilt OpenSSL)")
endif()

find_package(Threads REQUIRED)
target_link_libraries(asio2 INTERFACE
    Threads::Threads
    ${GENERAL_LIBS_FILE}
)

set_target_properties(asio2 PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_compile_features(asio2 INTERFACE cxx_std_17)

#-------------------------------------------------------------------------------
install(
    DIRECTORY ${ASIO2_ROOT_DIR}/include
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/asio2
)

install(
    DIRECTORY ${ASIO2_ROOT_DIR}/3rd
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/asio2
)

include(CMakePackageConfigHelpers)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/asio2-config-version.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY AnyNewerVersion
)

configure_package_config_file(
    ${ASIO2_ROOT_DIR}/cmake/asio2-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/asio2-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/asio2
    NO_SET_AND_CHECK_MACRO
    NO_CHECK_REQUIRED_COMPONENTS_MACRO
)

install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/asio2-config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/asio2-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/asio2
)

set(ASIO2_INSTALL_INCLUDES
    ${CMAKE_INSTALL_INCLUDEDIR}/asio2/include/
)

if(NOT ASIO2_DISABLE_HEADER_ONLY)
    list(APPEND ASIO2_INSTALL_INCLUDES
        ${CMAKE_INSTALL_INCLUDEDIR}/asio2/3rd/
    )
endif()

if(ASIO2_ENABLE_SSL)
    list(APPEND ASIO2_INSTALL_INCLUDES
        ${CMAKE_INSTALL_INCLUDEDIR}/asio2/3rd/openssl/include/
    )
endif()

install(
    TARGETS asio2
    EXPORT asio2-targets
    INCLUDES DESTINATION ${ASIO2_INSTALL_INCLUDES}
)

install(
    EXPORT asio2-targets
    NAMESPACE asio2::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/asio2
)

export(
    EXPORT asio2-targets
    FILE ${CMAKE_CURRENT_BINARY_DIR}/asio2-targets.cmake
    NAMESPACE asio2::
)
export(PACKAGE asio2)

file (GLOB_RECURSE  ASIO2_FILES  ${ASIO2_ROOT_DIR}/include/asio2/*.*)
file (GLOB_RECURSE   ASIO_FILES  ${ASIO2_ROOT_DIR}/3rd/asio/*.*)

# exclude : asio/impl/src.cpp
list (REMOVE_ITEM    ASIO_FILES  ${ASIO2_ROOT_DIR}/3rd/asio/impl/src.cpp)

enable_testing()

if(BUILD_EXAMPLES)
    add_subdirectory(example)
endif()

if(BUILD_TESTS)
    add_subdirectory (test)
endif()
