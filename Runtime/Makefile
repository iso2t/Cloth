# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Package the runtime under the same target triple reported by cloth --print-target.
RUNTIME_CC ?= $(CC)
RUNTIME_AR ?= $(AR)
runtime_arch := $(firstword $(subst -, ,$(cloth_toolchain_triple)))
runtime_triple := $(cloth_toolchain_triple)
ifneq ($(filter x86_64 amd64 i386 i486 i586 i686,$(runtime_arch)),)
runtime_host_bits := $(if $(filter x86_64 amd64,$(runtime_arch)),64,32)
RUNTIME_TARGET_BITS ?= $(runtime_host_bits)
ifeq ($(filter 32 64,$(RUNTIME_TARGET_BITS)),)
$(error RUNTIME_TARGET_BITS must be 32 or 64)
endif
runtime_cflags := -m$(RUNTIME_TARGET_BITS)
ifneq ($(RUNTIME_TARGET_BITS),$(runtime_host_bits))
runtime_triple := $(patsubst $(runtime_arch)-%,$(if $(filter 32,$(RUNTIME_TARGET_BITS)),i686,x86_64)-%,$(cloth_toolchain_triple))
endif
else ifneq ($(origin RUNTIME_TARGET_BITS),undefined)
$(error Target width overrides for the packaged runtime require an x86 toolchain)
endif

runtime_dir := $(BUILD_DIR)/obj/runtime-lib/$(runtime_triple)
runtime_members := $(addprefix $(runtime_dir)/,$(runtime-obj-y)) $(runtime_dir)/utf8proc.o
runtime_archive := $(BUILD_DIR)/lib/cloth/$(runtime_triple)/libcloth_runtime.a

.PHONY: runtime
runtime: $(runtime_archive)

$(runtime_archive): $(runtime_members) Runtime/Kbuild
	@$(make_dir)
ifeq ($(OS),Windows_NT)
	@powershell.exe -NoProfile -Command "[System.IO.File]::Delete('$@')"
else
	@$(RM) "$@"
endif
	$(RUNTIME_AR) rcs "$@" $(runtime_members)

$(runtime_dir)/%.o: Runtime/src/%.c Runtime/Kbuild Runtime/Makefile
	@$(make_dir)
	$(RUNTIME_CC) $(runtime_cflags) -std=c11 -IRuntime/include -IThirdParty/utf8proc -DUTF8PROC_STATIC $(CPPFLAGS) $(CFLAGS) -MMD -MP -c "$<" -o "$@"

$(runtime_dir)/utf8proc.o: ThirdParty/utf8proc/utf8proc.c Runtime/Makefile
	@$(make_dir)
	$(RUNTIME_CC) $(runtime_cflags) -std=c11 -DUTF8PROC_STATIC $(CPPFLAGS) $(CFLAGS) -MMD -MP -c "$<" -o "$@"

-include $(runtime_members:.o=.d)
