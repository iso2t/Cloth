// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/internals/Backend.rst
// See Documentation/language/expressions/Access.rst

#include <backend/string_value.hpp>

#include <climits>
#include <string_view>
#include <vector>

#include <ast/expr/expressions.hpp>
#include <diagnostics.hpp>
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/Types.h>

namespace backend::detail::string_value {
namespace {
enum Field : unsigned { Data, ByteLength, FieldCount };

LLVMValueRef literalBytes (LLVMContextRef context, std::string_view bytes) {
	// LLVM 18 takes an unsigned length for each constant string. Keep larger literals contiguous in a packed global.
	std::vector<LLVMValueRef> v_chunks;
	while (bytes.size() >= UINT_MAX) {
		v_chunks.push_back(LLVMConstStringInContext(context, bytes.data(), UINT_MAX, true));
		bytes.remove_prefix(UINT_MAX);
	}
	const LLVMValueRef ir_tail = LLVMConstStringInContext(context, bytes.data(), static_cast<unsigned>(bytes.size()), false);
	if (v_chunks.empty()) return ir_tail;
	v_chunks.push_back(ir_tail);
	return LLVMConstStructInContext(context, v_chunks.data(), static_cast<unsigned>(v_chunks.size()), true);
}
}

LLVMTypeRef type (LLVMModuleRef module) {
	const LLVMContextRef ctx_context = LLVMGetModuleContext(module);
	LLVMTypeRef          v_fields[]  = { LLVMPointerTypeInContext(ctx_context, 0), LLVMIntPtrTypeInContext(ctx_context, LLVMGetModuleDataLayout(module)) };
	return LLVMStructTypeInContext(ctx_context, v_fields, FieldCount, false);
}

LLVMValueRef literal (LLVMModuleRef module, const cloth::StringLiteral &value) {
	const LLVMTypeRef ty_length = LLVMStructGetTypeAtIndex(type(module), ByteLength);
	if (value.value.size() >= LLVMConstIntGetZExtValue(LLVMConstAllOnes(ty_length)))
		throw cde::ClothDiagnostic(cde::DiagnosticCode::UnsupportedLowering, "String literal storage exceeds the target address space",
		                           { { .span = value.getSpan(), .text = "string literal is too large" } });
	return constant(module, value.value);
}

LLVMValueRef constant (LLVMModuleRef module, std::string_view text) {
	const LLVMTypeRef    ty_length   = LLVMStructGetTypeAtIndex(type(module), ByteLength);
	const LLVMContextRef ctx_context = LLVMGetModuleContext(module);
	const LLVMValueRef   ir_bytes    = literalBytes(ctx_context, text);
	const LLVMValueRef   ir_data     = LLVMAddGlobal(module, LLVMTypeOf(ir_bytes), "cloth.string.data");
	LLVMSetInitializer(ir_data, ir_bytes);
	LLVMSetGlobalConstant(ir_data, true);
	LLVMSetLinkage(ir_data, LLVMPrivateLinkage);
	LLVMSetUnnamedAddress(ir_data, LLVMGlobalUnnamedAddr);
	LLVMValueRef v_fields[] = { ir_data, LLVMConstInt(ty_length, text.size(), false) };
	return LLVMConstStructInContext(ctx_context, v_fields, FieldCount, false);
}

Bytes bytes (LLVMBuilderRef builder, LLVMValueRef value) {
	return { .data = LLVMBuildExtractValue(builder, value, Data, ""), .length = LLVMBuildExtractValue(builder, value, ByteLength, "") };
}

LLVMValueRef scalarCount (LLVMBuilderRef builder, LLVMValueRef value) {
	const Bytes             by_text     = bytes(builder, value);
	const LLVMTypeRef       ty_word     = LLVMTypeOf(by_text.length);
	const LLVMContextRef    ctx_context = LLVMGetTypeContext(ty_word);
	const LLVMTypeRef       ty_byte     = LLVMInt8TypeInContext(ctx_context);
	const LLVMBasicBlockRef bb_start    = LLVMGetInsertBlock(builder);
	const LLVMValueRef      fn_caller   = LLVMGetBasicBlockParent(bb_start);
	const LLVMBasicBlockRef bb_test     = LLVMAppendBasicBlockInContext(ctx_context, fn_caller, "string.size.test");
	const LLVMBasicBlockRef bb_byte     = LLVMAppendBasicBlockInContext(ctx_context, fn_caller, "string.size.byte");
	const LLVMBasicBlockRef bb_end      = LLVMAppendBasicBlockInContext(ctx_context, fn_caller, "string.size.end");
	LLVMBuildBr(builder, bb_test);
	LLVMPositionBuilderAtEnd(builder, bb_test);
	LLVMValueRef       ir_index = LLVMBuildPhi(builder, ty_word, "");
	const LLVMValueRef ir_count = LLVMBuildPhi(builder, ty_word, "");
	LLVMBuildCondBr(builder, LLVMBuildICmp(builder, LLVMIntULT, ir_index, by_text.length, ""), bb_byte, bb_end);
	LLVMPositionBuilderAtEnd(builder, bb_byte);
	const LLVMValueRef ir_address = LLVMBuildGEP2(builder, ty_byte, by_text.data, &ir_index, 1, "");
	const LLVMValueRef ir_byte    = LLVMBuildLoad2(builder, ty_byte, ir_address, "");
	// Every scalar in valid UTF-8 has exactly one byte outside the continuation range 0x80..0xBF.
	const LLVMValueRef ir_prefix   = LLVMBuildAnd(builder, ir_byte, LLVMConstInt(ty_byte, 0xC0, false), "");
	const LLVMValueRef ir_leading  = LLVMBuildICmp(builder, LLVMIntNE, ir_prefix, LLVMConstInt(ty_byte, 0x80, false), "");
	LLVMValueRef       v_indices[] = { LLVMConstNull(ty_word), LLVMBuildAdd(builder, ir_index, LLVMConstInt(ty_word, 1, false), "") };
	LLVMValueRef       v_counts[]  = { LLVMConstNull(ty_word), LLVMBuildAdd(builder, ir_count, LLVMBuildZExt(builder, ir_leading, ty_word, ""), "") };
	LLVMBasicBlockRef  v_blocks[]  = { bb_start, bb_byte };
	LLVMAddIncoming(ir_index, v_indices, v_blocks, 2);
	LLVMAddIncoming(ir_count, v_counts, v_blocks, 2);
	LLVMBuildBr(builder, bb_test);
	LLVMPositionBuilderAtEnd(builder, bb_end);
	return ir_count;
}
}
