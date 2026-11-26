// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/language/expressions/Access.rst

#include <backend/string_value.hpp>
#include <backend/value_types.hpp>

#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/Types.h>
#include <semantic/checked_program.hpp>

namespace backend::detail {

LLVMTypeRef ValueTypes::metadataType () const {
	const LLVMContextRef context  = LLVMGetModuleContext(mod_module);
	const LLVMTypeRef    word     = LLVMIntPtrTypeInContext(context, LLVMGetModuleDataLayout(mod_module));
	const LLVMTypeRef    byte     = LLVMInt8TypeInContext(context);
	LLVMTypeRef          fields[] = { string_value::type(mod_module), LLVMPointerTypeInContext(context, 0), word, word, word, byte, byte, byte, byte };
	return LLVMStructTypeInContext(context, fields, 9, false);
}

LLVMValueRef ValueTypes::metadata (const cloth::ResolvedType &type) {
	if (const auto found = m_metadata.find(type); found != m_metadata.end()) return found->second;
	const LLVMContextRef context = LLVMGetModuleContext(mod_module);
	const LLVMTypeRef    word    = LLVMIntPtrTypeInContext(context, LLVMGetModuleDataLayout(mod_module));
	const LLVMTypeRef    byte    = LLVMInt8TypeInContext(context);
	const auto           subject = type.nullable() ? type.element() : type;
	unsigned             rank    = 0;
	for (auto cell = subject; cell.array() || cell.nullable(); cell = cell.element()) rank += cell.array();
	const bool         numeric  = subject.integer() || subject.floating();
	const auto         width    = numeric ? (subject.width ? subject.width : cp_program.layout.sizeBits) : 0;
	const LLVMTypeRef  pointer  = LLVMPointerTypeInContext(context, 0);
	LLVMValueRef       fields[] = { string_value::constant(mod_module, type.spelling()),
		                        subject.array() ? metadata(subject.element()) : LLVMConstNull(pointer),
		                        LLVMConstInt(word, rank, false),
		                        LLVMConstInt(word, subject.arrayExtent().value_or(0), false),
		                        LLVMConstInt(word, width, false),
		                        LLVMConstInt(byte, subject.array(), false),
		                        LLVMConstInt(byte, subject.arrayExtent().has_value(), false),
		                        LLVMConstInt(byte, subject.unsignedInteger(), false),
		                        LLVMConstInt(byte, numeric, false) };
	const LLVMValueRef value    = LLVMConstStructInContext(context, fields, 9, false);
	const LLVMValueRef global   = LLVMAddGlobal(mod_module, metadataType(), "cloth.type");
	LLVMSetInitializer(global, value);
	LLVMSetLinkage(global, LLVMPrivateLinkage);
	LLVMSetGlobalConstant(global, true);
	m_metadata.emplace(type, global);
	return global;
}
}
