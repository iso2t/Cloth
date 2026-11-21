// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <llvm-c/Core.h>
#include <semantic/checked_program.hpp>

namespace backend::detail {

// Returns null for types whose representation belongs to another lowering path.
LLVMTypeRef scalarType (LLVMContextRef context, cloth::TargetLayout layout, const cloth::ResolvedType &type);

class ScalarLowering {
    public:
	ScalarLowering (LLVMModuleRef module, LLVMBuilderRef builder, cloth::TargetLayout layout);
	[[nodiscard]] LLVMTypeRef  type (const cloth::ResolvedType &resolved) const;
	[[nodiscard]] LLVMValueRef constant (const cloth::ResolvedType &resolved, const cloth::NumericConstant &value) const;
	LLVMValueRef               convert (LLVMValueRef value, const cloth::ResolvedType &source, const cloth::ResolvedType &destination) const;
	LLVMValueRef               checkedCast (LLVMValueRef value, const cloth::ResolvedType &source, const cloth::ExpressionInfo &cast) const;
	LLVMValueRef               predicate (LLVMValueRef integer) const;
	LLVMValueRef               boolean (LLVMValueRef predicate) const;
	LLVMValueRef               binary (cloth::Operator op, LLVMValueRef left, LLVMValueRef right, const cloth::ResolvedType &operandType) const;

    private:
	void         trapIf (LLVMValueRef condition) const;
	void         guardDivisor (LLVMValueRef dividend, LLVMValueRef divisor, bool isUnsigned) const;
	LLVMValueRef integerPower (LLVMValueRef base, LLVMValueRef exponent, bool isUnsigned) const;
	LLVMValueRef floatToInteger (LLVMValueRef value, LLVMTypeRef target, bool isUnsigned) const;
	LLVMValueRef convertHalf (LLVMValueRef value) const;
	LLVMValueRef extendHalf (LLVMValueRef value) const;
	LLVMValueRef roundHalf (LLVMValueRef value) const;

	LLVMModuleRef       mod_module;
	LLVMBuilderRef      ir_builder;
	cloth::TargetLayout tl_layout;
};
}
