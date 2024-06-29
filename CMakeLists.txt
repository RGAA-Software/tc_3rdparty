cmake_minimum_required(VERSION 3.22)

include_directories(${CMAKE_SOURCE_DIR}/tc_3rdparty/)
include_directories(${CMAKE_SOURCE_DIR}/tc_3rdparty/eventbus/src)
add_subdirectory(eventbus)
if(WIN32)
	add_subdirectory(DirectXTex)
endif()
add_subdirectory(leveldb)