cmake_minimum_required(VERSION 3.22)

project(sqlite35)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(${PROJECT_NAME} STATIC sqlite3.c)