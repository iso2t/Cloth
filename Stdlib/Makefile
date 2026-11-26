# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

stdlib_source := Stdlib/StandardLibrary/src
stdlib_package := $(BUILD_DIR)/lib/cloth/stdlib
stdlib_manifest := $(BUILD_DIR)/obj/stdlib-modules.mk
-include $(stdlib_manifest)

# List directories after their contents so obsolete modules can be pruned first.
stdlib_tree = $(wildcard $(1)*.co $(1)*.clib) $(foreach dir,$(filter %/,$(wildcard $(1)*/)),$(call stdlib_tree,$(dir)) $(dir))
stdlib_entries := $(patsubst $(stdlib_source)/%,%,$(call stdlib_tree,$(stdlib_source)/))
stdlib_files := $(addprefix $(stdlib_package)/,$(filter-out %/,$(stdlib_entries)))
stdlib_stale := $(addprefix $(stdlib_package)/,$(filter-out $(stdlib_entries),$(packaged_stdlib_entries)))

# Also package the sources when building the driver directly or as a test prerequisite.
$(program): | stdlib

.PHONY: stdlib
stdlib: private export CLOTH_PACKAGED_MODULES := $(stdlib_entries)
stdlib: $(stdlib_files)
ifneq ($(stdlib_entries),$(packaged_stdlib_entries))
ifeq ($(OS),Windows_NT)
	@powershell.exe -NoProfile -Command "[void][System.IO.Directory]::CreateDirectory('$(BUILD_DIR)/obj')"
	@powershell.exe -NoProfile -Command "[System.IO.File]::WriteAllText('$(stdlib_manifest).tmp', 'packaged_stdlib_entries := ' + [Environment]::GetEnvironmentVariable('CLOTH_PACKAGED_MODULES'))"
	@powershell.exe -NoProfile -Command "Move-Item -LiteralPath '$(stdlib_manifest).tmp' -Destination '$(stdlib_manifest)' -Force -ErrorAction Stop"
else
	@mkdir -p "$(BUILD_DIR)/obj"
	@printf 'packaged_stdlib_entries := %s\n' "$$CLOTH_PACKAGED_MODULES" > "$(stdlib_manifest).tmp"
	@mv -f "$(stdlib_manifest).tmp" "$(stdlib_manifest)"
endif
endif

$(stdlib_files): $(stdlib_package)/%: $(stdlib_source)/%
	@$(make_dir)
ifeq ($(OS),Windows_NT)
	@powershell.exe -NoProfile -Command "Copy-Item -LiteralPath '$<' -Destination '$@' -Force -ErrorAction Stop"
else
	@cp "$<" "$@"
endif

ifneq ($(strip $(stdlib_stale)),)
# A case-only rename can name the same file on disk. Remove the old spelling
# before copying its replacement, even when both have the same timestamp.
.PHONY: prune-stdlib
stdlib $(stdlib_files): prune-stdlib
prune-stdlib:
ifeq ($(OS),Windows_NT)
	@powershell.exe -NoProfile -Command "$(foreach path,$(stdlib_stale),if (Test-Path -LiteralPath '$(path)') { (Get-Item -Force -LiteralPath '$(path)').Delete() };)"
else
	$(RM) -d $(foreach path,$(stdlib_stale),"$(path)")
endif
endif
