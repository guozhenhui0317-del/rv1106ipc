cmake_minimum_required(VERSION 3.5)
project(gzh_ipc LANGUAGES C CXX)

# These paths must point to RV1106 target files, not host/x86 libraries.
set(ROCKCHIP_SDK_ROOT "" CACHE PATH "RV1106 SDK staging/sysroot root")
set(FFMPEG_ROOT "" CACHE PATH "RV1106 FFmpeg 4.x installation prefix")
set(EASYLOGGER_ROOT "" CACHE PATH "EasyLogger source root")
set(ROCKCHIP_INCLUDE_DIR "${ROCKCHIP_SDK_ROOT}/usr/include" CACHE PATH "Rockchip include directory")
set(ROCKCHIP_SYSUTILS_INCLUDE_DIR "${ROCKCHIP_INCLUDE_DIR}" CACHE PATH "Directory containing rk_gpio.h and rk_pwm.h")
set(ROCKCHIP_LIB_DIR "${ROCKCHIP_SDK_ROOT}/usr/lib" CACHE PATH "Rockchip target library directory")

foreach(required ROCKCHIP_SDK_ROOT FFMPEG_ROOT EASYLOGGER_ROOT)
  if("${${required}}" STREQUAL "")
    message(FATAL_ERROR "${required} is required; see README.md")
  endif()
endforeach()

# Fail at configure time with a useful message instead of reporting missing headers midway through compilation.
set(REQUIRED_EXTERNAL_FILES
  "${ROCKCHIP_INCLUDE_DIR}/rk_mpi_sys.h"
  "${ROCKCHIP_INCLUDE_DIR}/rockiva/rockiva_ba_api.h"
  "${ROCKCHIP_SYSUTILS_INCLUDE_DIR}/rk_gpio.h"
  "${ROCKCHIP_SYSUTILS_INCLUDE_DIR}/rk_pwm.h"
  "${FFMPEG_ROOT}/include/libavformat/avformat.h"
  "${EASYLOGGER_ROOT}/easylogger/inc/elog.h"
  "${EASYLOGGER_ROOT}/easylogger/src/elog.c"
  "${EASYLOGGER_ROOT}/easylogger/src/elog_utils.c"
)
foreach(required_file IN LISTS REQUIRED_EXTERNAL_FILES)
  if(NOT EXISTS "${required_file}")
    message(FATAL_ERROR "Required target file not found: ${required_file}")
  endif()
endforeach()

set(APP_SOURCES
  src/main.cpp
  src/logger.cpp
  src/elog_port.c
  src/ffmpeg_publisher.cpp
  src/media.cpp
  common/common.c
  common/param/dictionary.c
  common/param/iniparser.c
  common/param/param.c
  common/isp/rv1106/isp.c
  common/rockiva/rockiva.c
  ${EASYLOGGER_ROOT}/easylogger/src/elog.c
  ${EASYLOGGER_ROOT}/easylogger/src/elog_utils.c
)

link_directories(${ROCKCHIP_LIB_DIR} ${FFMPEG_ROOT}/lib)
add_executable(gzh_ipc ${APP_SOURCES})
set_target_properties(gzh_ipc PROPERTIES
  C_STANDARD 99
  C_STANDARD_REQUIRED YES
  CXX_STANDARD 11
  CXX_STANDARD_REQUIRED YES
)
target_compile_definitions(gzh_ipc PRIVATE ELOG_OUTPUT_ENABLE ISP_HW_V32)
target_compile_options(gzh_ipc PRIVATE -Wall -Wextra -Wno-unused-parameter)

target_include_directories(gzh_ipc PRIVATE
  include
  common
  common/param
  common/isp/rv1106
  common/rockiva
  ${ROCKCHIP_SYSUTILS_INCLUDE_DIR}
  ${ROCKCHIP_INCLUDE_DIR}
  ${ROCKCHIP_INCLUDE_DIR}/rkaiq
  ${ROCKCHIP_INCLUDE_DIR}/rkaiq/uAPI
  ${ROCKCHIP_INCLUDE_DIR}/rkaiq/uAPI2
  ${ROCKCHIP_INCLUDE_DIR}/rkaiq/algos
  ${ROCKCHIP_INCLUDE_DIR}/rkaiq/common
  ${ROCKCHIP_INCLUDE_DIR}/rkaiq/xcore
  ${ROCKCHIP_INCLUDE_DIR}/rkaiq/iq_parser
  ${ROCKCHIP_INCLUDE_DIR}/rkaiq/iq_parser_v2
  ${ROCKCHIP_INCLUDE_DIR}/libdrm
  ${FFMPEG_ROOT}/include
  ${EASYLOGGER_ROOT}/easylogger/inc
)

# Rockchip SDK: MPI/Rockit, MPP, ISP/IQ, RockIVA/NPU, RGA and system utilities.
# FFmpeg 4.x: avformat provides RTSP/RTMP/FLV; avcodec/avutil provide codec metadata and timestamps.
# EasyLogger is compiled from EASYLOGGER_ROOT above; no prebuilt EasyLogger library is needed.
target_link_libraries(gzh_ipc PRIVATE
  pthread m
  rockit rockchip_mpp rkaiq rockiva rknnmrt rga rksysutils
  avformat avcodec avutil
)

install(TARGETS gzh_ipc RUNTIME DESTINATION bin)
install(FILES rv1106_sc3336.ini DESTINATION share/gzh_ipc)
