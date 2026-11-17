# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

CXX ?= g++
CXXFLAGS ?= -Wall -Wextra -g
BUILD_DIR ?= build

.DEFAULT_GOAL := all
.DELETE_ON_ERROR:

include Compiler/Kbuild
include Diagnostics/Kbuild

ifeq ($(OS),Windows_NT)
ifneq ($(filter default undefined,$(origin CC)),)
CC := gcc
endif
exe_suffix := .exe
make_dir = powershell.exe -NoProfile -Command "[void][System.IO.Directory]::CreateDirectory('$(@D)')"
run_path = $(subst /,\,$(abspath $<))
else
make_dir = mkdir -p "$(@D)"
run_path = $(abspath $<)
endif

CC ?= cc
musubi_object := $(BUILD_DIR)/obj/ThirdParty/musubi/musubi_impl.o
objects := $(addprefix $(BUILD_DIR)/obj/,$(obj-y)) \
           $(addprefix $(BUILD_DIR)/obj/Diagnostics/,$(diagnostics-obj-y)) \
           $(musubi_object)
program := $(BUILD_DIR)/cloth$(exe_suffix)

.PHONY: all run
all: $(program)

run: $(program)
	"$(run_path)"

$(program): $(objects) Makefile Compiler/Kbuild Diagnostics/Kbuild
	$(CXX) $(LDFLAGS) $(objects) $(LDLIBS) -o "$@"

$(BUILD_DIR)/obj/%.o: Compiler/src/%.cpp Makefile compile_flags.txt
	@$(make_dir)
	$(CXX) @compile_flags.txt $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c "$<" -o "$@"

$(BUILD_DIR)/obj/Diagnostics/diagnostic_reporter.o: private override CPPFLAGS += -IThirdParty/musubi -DMU_API=

$(BUILD_DIR)/obj/Diagnostics/%.o: Diagnostics/src/%.cpp Makefile compile_flags.txt
	@$(make_dir)
	$(CXX) @compile_flags.txt $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c "$<" -o "$@"

$(musubi_object): ThirdParty/musubi/src/musubi_impl.c Makefile
	@$(make_dir)
	$(CC) -std=c11 $(CPPFLAGS) $(CFLAGS) -DMU_API= -MMD -MP -c "$<" -o "$@"

-include $(objects:.o=.d)
