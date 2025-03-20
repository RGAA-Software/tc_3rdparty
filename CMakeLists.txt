cmake_minimum_required(VERSION 3.22)

include_directories(${CMAKE_SOURCE_DIR}/tc_3rdparty/)
include_directories(${CMAKE_SOURCE_DIR}/tc_3rdparty/eventbus/src)
include_directories(${CMAKE_SOURCE_DIR}/tc_3rdparty/stb)
add_subdirectory(eventbus)
if(WIN32)
	add_subdirectory(DirectXTex)

	include_directories(${CMAKE_SOURCE_DIR}/deps/tc_3rdparty/abseil-cpp)
	#set(protobuf_MSVC_STATIC_RUNTIME OFF CACHE BOOL "disable protobuf_MSVC_STATIC_RUNTIME" FORCE)
	set(protobuf_BUILD_TESTS OFF CACHE BOOL "disable protobuf_BUILD_TESTS" FORCE)
	set(protobuf_INSTALL OFF)
	set(utf8_range_ENABLE_INSTALL OFF)
	# git tag: 20250127.0
	add_subdirectory(abseil-cpp)
	# git tag: v29.3
	add_subdirectory(protobuf)

endif()

set(LEVELDB_BUILD_TESTS OFF CACHE BOOL "LEVELDB_BUILD_TESTS OFF" FORCE)
set(LEVELDB_BUILD_BENCHMARKS OFF CACHE BOOL "LEVELDB_BUILD_TESTS OFF" FORCE)
add_subdirectory(leveldb)
add_subdirectory(sqlite_orm)
add_subdirectory(opus)
add_subdirectory(libyuv)

set(QWINDOWKIT_BUILD_EXAMPLES OFF)
add_subdirectory(qwindowkit/qmsetup)
add_subdirectory(qwindowkit)