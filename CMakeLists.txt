cmake_minimum_required(VERSION 3.16)
project(HGCSim VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2")
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

include_directories(${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/third_party)

# =============================================================================
# build 名称与审查
#   未经修改的原始源码：build 名自动取为保留名（见下）
#   修改过源码或添加了 plugins/student/*.cpp：必须指定 build 名
#     make build BUILD_NAME=<名称>    （等价于 cmake -DHGCSIM_BUILD_NAME=<名称>，见仓库根目录 Makefile）
#   名称规则：字母或数字开头，仅含字母、数字、. _ -，长度 1~40，不得与保留名重复（不区分大小写）
# =============================================================================
include(${CMAKE_SOURCE_DIR}/cmake/fingerprint.cmake)
set(HGCSIM_BUILD_NAME "" CACHE STRING "build 名称（留空表示使用原始 build 名）")
set(HGC_RESERVED_NAMES default)
set(HGC_PRISTINE_NAME default)
string(REPLACE ";" ", " HGC_RESERVED_TEXT "${HGC_RESERVED_NAMES}")
hgc_source_fingerprint(${CMAKE_SOURCE_DIR} HGC_SOURCE_FP)
hgc_student_fingerprint(${CMAKE_SOURCE_DIR} HGC_STUDENT_FP)
set(HGC_PRISTINE_FP "")
if(EXISTS ${CMAKE_SOURCE_DIR}/scripts/pristine.fingerprint)
  file(READ ${CMAKE_SOURCE_DIR}/scripts/pristine.fingerprint HGC_PRISTINE_FP)
  string(STRIP "${HGC_PRISTINE_FP}" HGC_PRISTINE_FP)
endif()
file(GLOB HGC_STUDENT_SOURCES ${CMAKE_SOURCE_DIR}/plugins/student/*.cpp)
set(HGC_MODIFIED OFF)
if(NOT HGC_SOURCE_FP STREQUAL HGC_PRISTINE_FP OR HGC_STUDENT_SOURCES)
  set(HGC_MODIFIED ON)
endif()
if(HGCSIM_BUILD_NAME STREQUAL "")
  if(HGC_MODIFIED)
    message(FATAL_ERROR
      "\n检测到源码已修改或存在自定义调度插件（plugins/student/*.cpp），必须为这次 build 指定名称：\n"
      "    make build BUILD_NAME=<名称>\n"
      "名称不能使用保留名：${HGC_RESERVED_TEXT}\n")
  endif()
  set(HGC_BUILD_NAME ${HGC_PRISTINE_NAME})
else()
  if(NOT HGCSIM_BUILD_NAME MATCHES "^[A-Za-z0-9][A-Za-z0-9._-]*$")
    message(FATAL_ERROR "\nbuild 名 '${HGCSIM_BUILD_NAME}' 不合法：只能包含字母、数字、. _ -，且以字母或数字开头\n")
  endif()
  string(LENGTH "${HGCSIM_BUILD_NAME}" _len)
  if(_len GREATER 40)
    message(FATAL_ERROR "\nbuild 名过长（${_len} 个字符，上限 40）\n")
  endif()
  string(TOLOWER "${HGCSIM_BUILD_NAME}" _lname)
  foreach(r ${HGC_RESERVED_NAMES})
    if(_lname STREQUAL r)
      message(FATAL_ERROR "\nbuild 名 '${HGCSIM_BUILD_NAME}' 与自带 build 的保留名 '${r}' 冲突，请换一个名称（保留名：${HGC_RESERVED_TEXT}）\n")
    endif()
  endforeach()
  set(HGC_BUILD_NAME ${HGCSIM_BUILD_NAME})
endif()
string(TIMESTAMP HGC_BUILD_TIME "%Y-%m-%d %H:%M:%S")
set(HGC_PLUGIN_LIST "example_scheduler")
foreach(src ${HGC_STUDENT_SOURCES})
  get_filename_component(_n ${src} NAME_WE)
  list(APPEND HGC_PLUGIN_LIST ${_n})
endforeach()
string(REPLACE ";" "," HGC_PLUGIN_CSV "${HGC_PLUGIN_LIST}")
string(REPLACE ";" "," HGC_RESERVED_CSV "${HGC_RESERVED_NAMES}")
configure_file(${CMAKE_SOURCE_DIR}/cmake/build_info.h.in ${CMAKE_BINARY_DIR}/generated/hgcsim_build_info.h @ONLY)
configure_file(${CMAKE_SOURCE_DIR}/cmake/build_info.json.in ${CMAKE_BINARY_DIR}/build_info.json @ONLY)
if(HGC_MODIFIED)
  set(_state "源码或插件已修改")
else()
  set(_state "原始源码")
endif()
message(STATUS "HGCSim build 名称: ${HGC_BUILD_NAME}（${_state}；调度插件: ${HGC_PLUGIN_CSV}）")

# ---- 模拟器核心（静态库）----
add_library(hgcsim_core STATIC
  src/memory.cpp
  src/interconnect.cpp
  src/devices.cpp
  src/system.cpp
  src/workload.cpp
  src/runtime.cpp)
target_link_libraries(hgcsim_core PUBLIC ${CMAKE_DL_LIBS})

# ---- C ABI 共享库（供 Python ctypes 调用）----
add_library(hgcsim SHARED src/c_api.cpp)
target_include_directories(hgcsim PRIVATE ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(hgcsim PRIVATE hgcsim_core)

# ---- 命令行 ----
add_executable(hgcsim_cli src/cli.cpp)
target_link_libraries(hgcsim_cli PRIVATE hgcsim_core)

# ---- 调度插件 ----
add_library(example_scheduler MODULE plugins/example_scheduler.cpp)
set_target_properties(example_scheduler PROPERTIES
  PREFIX "lib" LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins)

# 同学自己的插件：把 plugins/student/*.cpp 自动编译为 build/plugins/lib<name>.so
foreach(src ${HGC_STUDENT_SOURCES})
  get_filename_component(name ${src} NAME_WE)
  add_library(${name} MODULE ${src})
  set_target_properties(${name} PROPERTIES PREFIX "lib" LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins)
endforeach()

# ---- 测试 ----
enable_testing()
add_executable(hgcsim_tests tests/test_main.cpp)
target_link_libraries(hgcsim_tests PRIVATE hgcsim_core)
add_test(NAME unit_tests COMMAND hgcsim_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_test(NAME cli_run COMMAND hgcsim_cli run configs/systems/default_pc.json configs/workloads/B2.json
         configs/strategies/naive_gpu.json WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
