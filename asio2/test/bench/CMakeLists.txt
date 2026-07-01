#
# COPYRIGHT (C) 2017-2019, zhllxt
# 
# author   : zhllxt
# email    : 37792738@qq.com
# 
# Distributed under the GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
# (See accompanying file LICENSE or see <http://www.gnu.org/licenses/>)
#

cmake_minimum_required(VERSION 3.11)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/configure.cmake)

add_subdirectory (kcp)
add_subdirectory (rpc)
add_subdirectory (tcp)
add_subdirectory (udp)
add_subdirectory (rdc)
