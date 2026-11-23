// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <cstdint>

#include <llvm-c/Core.h>
#include <semantic/checked_program.hpp>

namespace backend::detail {

// Returns null for types whose representation belongs to another lowering path.
LLVMTypeRef scalarType (LLVMContextRef context, cloth::TargetLayout layout, const cloth::ResolvedType &type);
LLVMValueRef scalarConstant (LLVMTypeRef type, const cloth::NumericConstant &value);

class ScalarLowering {
    public:
	ScalarLowering (LLVMModuleRef module, LLVMBuilderRef builder, cloth::TargetLayout layout);
	[[nodiscard]] LLVMTypeRef  type (const cloth::ResolvedType &resolved) const;
	LLVMValueRef               convert (LLVMValueRef value, const cloth::ResolvedType &source, const cloth::ResolvedType &destination) const;
	LLVMValueRef               checkedCast (LLVMValueRef value, const cloth::ResolvedType &source, const cloth::ResolvedType &destination,
	                                        const cloth::ExpressionInfo &cast) const;
	LLVMValueRef               predicate (LLVMValueRef integer) const;
	LLVMValueRef               boolean (LLVMValueRef predicate) const;
	LLVMValueRef               checkedArrayIndex (LLVMValueRef index, bool isUnsigned, std::uint64_t length) const;
	// Returns an LLVM i1. Nullable scalars support equality and inequality.
	LLVMValueRef               compare (cloth::Operator op, LLVMValueRef left, LLVMValueRef right, const cloth::ResolvedType &operandType) const;
	LLVMValueRef               binary (cloth::Operator op, LLVMValueRef left, LLVMValueRef right, const cloth::ResolvedType &operandType) const;

    private:
	using FailureDeclaration = LLVMValueRef (*)(LLVMModuleRef module);
	void         failIf (LLVMValueRef condition, FailureDeclaration declareFailure) const;
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
