# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

ifeq ($(OS),Windows_NT)
PYTHON ?= python
else
PYTHON ?= python3
gtest_thread_flags := -pthread
endif

gtest_version := 1.18.0
gtest_sha256 := 6e3191c1455468b3fc35a417fb565c1c5071aee1b7e7f85e30cf48a98d37d8b5
gtest_deps := $(BUILD_DIR)/Tests/deps
gtest_dir := $(gtest_deps)/googletest-$(gtest_version)/googletest
gtest_source := $(gtest_dir)/src/gtest-all.cc

unit_sources := Backend/Unit/llvm_link_test.cpp \
                Backend/Unit/lowering_test.cpp \
                Compiler/Lexer/Unit/token_test.cpp \
                Compiler/Parser/Unit/precedence_test.cpp \
                Compiler/Semantic/Unit/symbol_table_test.cpp \
                Compiler/Semantic/Unit/checked_program_test.cpp \
                Compiler/Semantic/Unit/builtin_calls_test.cpp \
                Compiler/Semantic/Unit/generic_instances_test.cpp \
                Compiler/Semantic/Unit/dispatch_test.cpp \
                Compiler/Semantic/Unit/flow_test.cpp \
                Compiler/Semantic/Unit/ownership_test.cpp \
                Compiler/Semantic/Unit/operator_test.cpp \
                Compiler/Semantic/Unit/storage_layout_test.cpp \
                Compiler/Semantic/Unit/value_semantics_test.cpp \
                Compiler/Semantic/Unit/program_boundaries_test.cpp \
                Diagnostics/Unit/import_test.cpp
unit_obj_dir := $(BUILD_DIR)/obj/Tests
unit_objects := $(addprefix $(unit_obj_dir)/,$(unit_sources:.cpp=.o))
backend_unit_objects := $(filter $(unit_obj_dir)/Backend/%,$(unit_objects))
gtest_objects := $(addprefix $(unit_obj_dir)/googletest/,gtest-all.o gtest_main.o)
unit_cloth_objects := $(filter-out $(BUILD_DIR)/obj/cloth.o,$(objects))
unit_program := $(BUILD_DIR)/Tests/cloth-unit$(exe_suffix)
unit_cxxflags = @compile_flags.txt $(CPPFLAGS) $(CXXFLAGS) $(gtest_thread_flags) -I"$(gtest_dir)/include"

$(gtest_source):
	@"$(PYTHON)" Tests/Support/fetch_googletest.py "$(gtest_deps)" "$(gtest_version)" "$(gtest_sha256)"

$(gtest_objects): $(unit_obj_dir)/googletest/%.o: $(gtest_source) Makefile Tests/Makefile compile_flags.txt
	@$(make_dir)
	$(CXX) $(unit_cxxflags) -I"$(gtest_dir)" -MMD -MP -c "$(gtest_dir)/src/$*.cc" -o "$@"

$(backend_unit_objects): private override CPPFLAGS += $(backend_cppflags)
$(backend_unit_objects): Backend/Kbuild

$(unit_objects): $(unit_obj_dir)/%.o: Tests/%.cpp Makefile Tests/Makefile compile_flags.txt | $(gtest_source)
	@$(make_dir)
	$(CXX) $(unit_cxxflags) -MMD -MP -c "$<" -o "$@"

$(unit_program): $(unit_objects) $(gtest_objects) $(unit_cloth_objects) Makefile Tests/Makefile Compiler/Kbuild Diagnostics/Kbuild Backend/Kbuild
	@$(make_dir)
	$(CXX) $(LDFLAGS) $(unit_objects) $(gtest_objects) $(unit_cloth_objects) $(gtest_thread_flags) $(backend_ldlibs) $(LDLIBS) -o "$@"

-include $(unit_objects:.o=.d) $(gtest_objects:.o=.d)

.PHONY: test test-build test-driver test-lexer test-parser test-diagnostics test-file test-unit
.PHONY: lit-all lit-lexer lit-diagnostics unit-all unit-lexer unit-diagnostics

test-build: $(program) $(unit_program)
test: lit-all unit-all
test-lexer: lit-lexer unit-lexer
test-diagnostics: lit-diagnostics unit-diagnostics

lit-all: test_input := Tests
test-driver: test_input := Tests/Compiler/Driver
lit-lexer: test_input := Tests/Compiler/Lexer
lit-diagnostics: test_input := Tests/Diagnostics
test-file: test_input = $(CASE)
test-file: test_file_flag := --single

lit-all lit-lexer lit-diagnostics test-driver test-file: $(program)
	@"$(PYTHON)" Tests/Support/run_lit.py $(test_file_flag) "$(program)" "$(test_input)" --readobj "$(call llvm_query,--bindir)/llvm-readobj$(exe_suffix)" --cc $(CC)

unit-all: unit_filter := *
unit-lexer: unit_filter := Lexer*.*
test-parser: unit_filter := Parser*.*
unit-diagnostics: unit_filter := Diagnostics*.*
test-unit: unit_filter = $(if $(CASE),$(CASE),*)

unit-all unit-lexer unit-diagnostics test-parser test-unit: $(unit_program)
	@"$(unit_program)" --gtest_fail_if_no_test_selected "--gtest_filter=$(unit_filter)"
