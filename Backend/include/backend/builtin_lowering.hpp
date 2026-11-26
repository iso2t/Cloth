// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

#include <llvm-c/Types.h>
#include <semantic/checked_program.hpp>

namespace backend::detail {

class ValueTypes;
class ScalarLowering;

class BuiltinLowering {
    public:
	BuiltinLowering (LLVMModuleRef module, LLVMBuilderRef builder, ValueTypes &types, ScalarLowering &scalars);
	LLVMValueRef method (const cloth::BuiltinCall &call, const cloth::ResolvedType &receiverType, LLVMValueRef receiver,
	                     const std::vector<LLVMValueRef> &args, const cloth::ResolvedType &result);
	LLVMValueRef formatScalar (const cloth::ResolvedType &type, LLVMValueRef value);
	LLVMValueRef floatAbs (LLVMValueRef value);
	LLVMValueRef integerZeros (LLVMValueRef value, bool leading);
	LLVMValueRef integerText (const cloth::ResolvedType &type, LLVMValueRef value, LLVMValueRef radix);
	LLVMValueRef equalText (LLVMValueRef left, LLVMValueRef right, bool optional);
	LLVMValueRef enumName (const cloth::ResolvedType &type, LLVMValueRef tag);
	LLVMValueRef join (std::span<const LLVMValueRef> parts);
	LLVMValueRef address (LLVMValueRef value);
	LLVMValueRef call (const char *name, LLVMTypeRef result, std::initializer_list<LLVMValueRef> arguments);
	LLVMValueRef optionalText (const char *name, LLVMValueRef argument);
	LLVMValueRef number (unsigned bits, std::uint64_t value) const;
	void         guard (LLVMValueRef failure, const char *message);

    private:
	LLVMValueRef    output (const char *name, LLVMTypeRef result, std::initializer_list<LLVMValueRef> arguments);
	LLVMValueRef    invoke (const char *name, LLVMTypeRef result, std::vector<LLVMValueRef> arguments);
	LLVMValueRef    numeric (cloth::BuiltinOperation op, const cloth::ResolvedType &type, LLVMValueRef value, const std::vector<LLVMValueRef> &args);
	LLVMModuleRef   mod_module;
	LLVMBuilderRef  ir_builder;
	ValueTypes     &vt_types;
	ScalarLowering &sl_scalars;
};
}
