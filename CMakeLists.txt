cmake_minimum_required(VERSION 3.22)

add_subdirectory(eventbus)

if(WIN32)
	add_subdirectory(DirectXTex)
endif()