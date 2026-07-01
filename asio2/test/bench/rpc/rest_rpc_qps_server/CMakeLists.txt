#
# COPYRIGHT (C) 2017-2021, zhllxt
# 
# author   : zhllxt
# email    : 37792738@qq.com
# 
# Distributed under the GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
# (See accompanying file LICENSE or see <http://www.gnu.org/licenses/>)
#

cmake_minimum_required (VERSION 3.11)

set(PROJECT_NAME rest_rpc_qps_server)
set(TARGET_NAME bench_${PROJECT_NAME})

add_executable (
    ${TARGET_NAME}
    ${PROJECT_NAME}.cpp
)

target_include_directories(${TARGET_NAME} PRIVATE ${ASIO2_ROOT_DIR}/test/bench/rpc/rest_rpc-master/include)
target_include_directories(${TARGET_NAME} PRIVATE ${ASIO2_ROOT_DIR}/test/bench/rpc/rest_rpc-master/include/rest_rpc)
target_include_directories(${TARGET_NAME} PRIVATE ${ASIO2_ROOT_DIR}/test/bench/rpc/rest_rpc-master/thirdparty/msgpack-c/include)
target_include_directories(${TARGET_NAME} PRIVATE ${ASIO2_ROOT_DIR}/asio)

configure(${TARGET_NAME})
