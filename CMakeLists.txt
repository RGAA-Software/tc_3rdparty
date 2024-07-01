cmake_minimum_required(VERSION 3.22)

include_directories(${CMAKE_SOURCE_DIR}/tc_3rdparty/)
include_directories(${CMAKE_SOURCE_DIR}/tc_3rdparty/eventbus/src)
include_directories(${CMAKE_SOURCE_DIR}/tc_3rdparty/stb)
add_subdirectory(eventbus)
if(WIN32)
	add_subdirectory(DirectXTex)
endif()

set(LEVELDB_BUILD_TESTS OFF CACHE BOOL "LEVELDB_BUILD_TESTS OFF" FORCE)
set(LEVELDB_BUILD_BENCHMARKS OFF CACHE BOOL "LEVELDB_BUILD_TESTS OFF" FORCE)
add_subdirectory(leveldb)
add_subdirectory(sqlite_orm)