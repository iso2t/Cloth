// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <gtest/gtest.h>

#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm/Config/llvm-config.h>

namespace {

TEST (BackendLLVM, LoadsMatchingLibraryAndNativeTarget) {
	unsigned u_major = 0;
	LLVMGetVersion(&u_major, nullptr, nullptr);
	ASSERT_EQ(u_major, static_cast<unsigned>(LLVM_VERSION_MAJOR));
	ASSERT_EQ(LLVMInitializeNativeTarget(), 0);
	EXPECT_EQ(LLVMInitializeNativeAsmPrinter(), 0);
}

}
