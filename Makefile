# =============================================================================
# HGCSim 编译入口
#   依赖安装：bash install_deps.sh        运行：bash scripts/*.sh（见 README）
#
#   make build                          构建（未修改的原始源码使用自带 build 名）
#   make build BUILD_NAME=<build名>     按给定 build 名构建（改过源码或写了 C++ 插件后必须指定）
#   make clean                          清除所有 build（删除 build/ 目录）
#   make help                           查看全部目标与可选变量
# =============================================================================
SHELL := /bin/bash
.DEFAULT_GOAL := help
.NOTPARALLEL:
.PHONY: help build rebuild test info clean

ROOT      := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
BUILD_DIR := $(ROOT)/build

# ---- 可在命令行覆盖的变量（make build 变量=值）----
BUILD_NAME ?=
BUILD_TYPE ?= Release
JOBS       ?= $(shell nproc 2>/dev/null || echo 2)
TEST       ?= 1
VERBOSE    ?= 0
CMAKE      ?= cmake
CTEST      ?= ctest
CMAKE_ARGS ?=
PYTHON     ?= python3

ifeq ($(filter $(BUILD_TYPE),Release Debug RelWithDebInfo),)
  $(error BUILD_TYPE 只能是 Release、Debug 或 RelWithDebInfo（当前为 "$(BUILD_TYPE)"）)
endif
ifeq ($(filter $(TEST),0 1),)
  $(error TEST 只能是 0 或 1（当前为 "$(TEST)"）)
endif

# 只有在命令行或环境变量中显式给出 CXX 时才指定编译器，否则由 CMake 自动选择
ifneq ($(filter environment command,$(firstword $(origin CXX))),)
  CMAKE_CXX_ARG := -DCMAKE_CXX_COMPILER=$(CXX)
endif

help:
	@echo "HGCSim 编译（依赖安装：bash install_deps.sh；运行：bash scripts/run_frontend.sh 等，见 README）"
	@echo
	@echo "目标："
	@echo "  make build                       构建 build/（未修改的原始源码使用自带 build 名）"
	@echo "  make build BUILD_NAME=<build名>  按给定 build 名构建；修改过源码或在 plugins/student/ 下"
	@echo "                                   写了 C++ 调度插件后必须指定，且不能使用保留名"
	@echo "  make rebuild [BUILD_NAME=...]    先清除再完整构建（等价于 make clean 后 make build）"
	@echo "  make clean                       清除所有 build（删除 build/ 目录）"
	@echo "  make test                        对现有 build 运行 C++ 单元测试（不重新编译）"
	@echo "  make info                        显示当前 build 名、编译时间、插件列表，以及之后源码是否又被修改"
	@echo "  make help                        显示本说明（直接运行 make 也显示本说明）"
	@echo
	@echo "可选变量（写在目标后面，例如 make build BUILD_NAME=my_v1 JOBS=4）："
	@echo "  BUILD_NAME=<名称>   build 名：字母或数字开头，只含字母、数字、. _ -，长度 1~40（默认：空=自动）"
	@echo "  BUILD_TYPE=<类型>   Release（默认）| Debug | RelWithDebInfo"
	@echo "  JOBS=<N>            并行编译任务数（默认：CPU 核数 $(JOBS)）"
	@echo "  TEST=0|1            构建后是否运行 C++ 单元测试（默认 1）"
	@echo "  VERBOSE=0|1         显示完整编译命令（默认 0）"
	@echo "  CXX=<编译器>        指定 C++ 编译器，例如 CXX=g++-9；更换编译器时请用 make rebuild"
	@echo "  CMAKE_ARGS=\"...\"    追加传给 cmake 配置阶段的参数"

build:
	@command -v $(CMAKE) >/dev/null 2>&1 || { echo "错误：找不到 cmake，请先运行 bash install_deps.sh" >&2; exit 1; }
	@command -v c++ >/dev/null 2>&1 || command -v g++ >/dev/null 2>&1 || [ -n "$(CMAKE_CXX_ARG)" ] \
	  || { echo "错误：找不到 C++ 编译器，请先运行 bash install_deps.sh" >&2; exit 1; }
	@echo "==> [1/3] 配置：build 名 = $(if $(BUILD_NAME),$(BUILD_NAME),（未指定，原始源码自动使用自带名称）)，类型 = $(BUILD_TYPE)"
	@$(CMAKE) -S "$(ROOT)" -B "$(BUILD_DIR)" -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) '-DHGCSIM_BUILD_NAME=$(BUILD_NAME)' \
	  $(CMAKE_CXX_ARG) $(CMAKE_ARGS) \
	  || { echo >&2; echo "配置失败：请根据上面的提示修正后重试，例如 make build BUILD_NAME=<build名>" >&2; exit 1; }
	@echo "==> [2/3] 编译：$(JOBS) 个并行任务"
	@$(CMAKE) --build "$(BUILD_DIR)" -j $(JOBS) $(if $(filter 1,$(VERBOSE)),--verbose,)
ifeq ($(TEST),1)
	@echo "==> [3/3] C++ 单元测试"
	@cd "$(BUILD_DIR)" && $(CTEST) --output-on-failure
else
	@echo "==> [3/3] 跳过单元测试（TEST=0）"
endif
	@name=$$(sed -n 's/.*"name": "\(.*\)".*/\1/p' "$(BUILD_DIR)/build_info.json" | head -1); \
	  echo; echo "编译完成，当前 build 名：$$name"; \
	  echo "如果前端正在运行，请先停止（Ctrl+C）再重新运行 bash scripts/run_frontend.sh 以加载新 build。"

rebuild: clean build

test:
	@[ -f "$(BUILD_DIR)/CTestTestfile.cmake" ] || { echo "还没有 build，请先运行 make build" >&2; exit 1; }
	@cd "$(BUILD_DIR)" && $(CTEST) --output-on-failure

info:
	@[ -f "$(BUILD_DIR)/build_info.json" ] || { echo "还没有 build，请先运行 make build" >&2; exit 1; }
	@cd "$(ROOT)/frontend" && $(PYTHON) -m hgcsim_frontend.buildinfo

clean:
	@if [ -d "$(BUILD_DIR)" ]; then rm -rf "$(BUILD_DIR)" && echo "已删除 build/"; else echo "没有 build/，无需清除"; fi
