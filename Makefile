CXX ?= g++
CXXFLAGS ?= -Wall -Wextra -g
BUILD_DIR ?= build

.DEFAULT_GOAL := all
.DELETE_ON_ERROR:

include Compiler/Kbuild

ifeq ($(OS),Windows_NT)
exe_suffix := .exe
make_dir = powershell.exe -NoProfile -Command "[void][System.IO.Directory]::CreateDirectory('$(@D)')"
run_path = $(subst /,\,$(abspath $<))
else
make_dir = mkdir -p "$(@D)"
run_path = $(abspath $<)
endif

objects := $(addprefix $(BUILD_DIR)/obj/,$(obj-y))
program := $(BUILD_DIR)/clothc$(exe_suffix)

.PHONY: all run
all: $(program)

run: $(program)
	"$(run_path)"

$(program): $(objects) Makefile Compiler/Kbuild
	$(CXX) $(LDFLAGS) $(objects) $(LDLIBS) -o "$@"

$(BUILD_DIR)/obj/%.o: Compiler/src/%.cpp Makefile compile_flags.txt
	@$(make_dir)
	$(CXX) @compile_flags.txt $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c "$<" -o "$@"

-include $(objects:.o=.d)
