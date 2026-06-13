cmake_minimum_required(VERSION 3.22)

include_directories(${GR_PROJECT_PATH}/tc_3rdparty/)
include_directories(${GR_PROJECT_PATH}/tc_3rdparty/eventbus/src)
include_directories(${GR_PROJECT_PATH}/tc_3rdparty/stb)
add_subdirectory(eventbus)
if(WIN32)

	if(DirectXTex_ENABLED)
		add_subdirectory(DirectXTex)
	endif()

	# qwindowkit
	#set(QWINDOWKIT_BUILD_EXAMPLES OFF)
	#add_subdirectory(qwindowkit/qmsetup)
	#add_subdirectory(qwindowkit)

	#libyuv
	add_subdirectory(libyuv)
endif()

set(LEVELDB_BUILD_TESTS OFF CACHE BOOL "LEVELDB_BUILD_TESTS OFF" FORCE)
set(LEVELDB_BUILD_BENCHMARKS OFF CACHE BOOL "LEVELDB_BUILD_TESTS OFF" FORCE)
add_subdirectory(leveldb)
add_subdirectory(sqlite35)
#add_subdirectory(sqlite_orm)
