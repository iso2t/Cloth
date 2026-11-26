# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

migrator := $(BUILD_DIR)/cloth-migrate-semicolons$(exe_suffix)
migration_objects := $(addprefix $(BUILD_DIR)/obj/migration/,main.o semicolons.o structure.o)
migration_frontend := $(addprefix $(BUILD_DIR)/obj/,$(filter ast/% lexer/% parser/% token/% file/source_file.o file/atomic_file.o util/unicode_identifier.o,$(obj-y))) \
                      $(addprefix $(BUILD_DIR)/obj/Diagnostics/,$(diagnostics-obj-y)) $(musubi_object) $(utf8proc_object)

.PHONY: migrate-semicolons
migrate-semicolons: $(migrator)

$(migrator): $(migration_objects) $(migration_frontend) Compiler/migration.mk
	$(CXX) $(LDFLAGS) $(driver_ldflags) $(migration_objects) $(migration_frontend) $(LDLIBS) -o "$@"

-include $(migration_objects:.o=.d)
