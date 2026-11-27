// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <backend/enum_payloads.hpp>
#include <backend/growable_array.hpp>
#include <backend/nullable_value.hpp>
#include <backend/scalar_lowering.hpp>
#include <backend/string_value.hpp>
#include <backend/value_types.hpp>

#include <cstddef>
#include <string>
#include <variant>
#include <vector>

#include <ast/expr/expression.hpp>
#include <ast/expr/expressions.hpp>
#include <ast/expr/struct_expressions.hpp>
#include <diagnostics.hpp>
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/Types.h>
#include <semantic/checked_program.hpp>
#include <semantic/symbol_table.hpp>
#include <token/token.hpp>

namespace backend::detail {

EnumPayloads::EnumPayloads (LLVMModuleRef module, const cloth::CheckedProgram &program, ValueTypes &types) :
        mod_module(module),
        cp_program(program),
        vt_types(types) {
}

LLVMValueRef EnumPayloads::constant (const cloth::Expression &value, const cloth::ResolvedType &destination) {
	const cloth::ExpressionInfo &ei_value = cp_program.expressions.at(&value);
	const LLVMTypeRef            ty_value = vt_types.localType(destination);
	if (destination.nullable()) {
		if (dynamic_cast<const cloth::NullLiteral *>(&value)) return LLVMConstNull(ty_value);
		if (const auto *ce_cast = dynamic_cast<const cloth::CastExpression *>(&value);
		    ce_cast && !ei_value.symbol && std::holds_alternative<std::monostate>(ei_value.numericConstant))
			return constant(*ce_cast->operand, destination);
		return nullable::presentConstant(constant(value, destination.element()));
	}
	if (!std::holds_alternative<std::monostate>(ei_value.numericConstant)) {
		const cloth::ResolvedType &rt_source = ei_value.types.front();
		if (destination.convertsIntegerToBool(rt_source.nullable() ? rt_source.element() : rt_source))
			return LLVMConstInt(ty_value, ei_value.integerTruth().value(), false);
		return scalarConstant(ty_value, ei_value.numericConstant);
	}
	if (const auto *cl_literal = dynamic_cast<const cloth::CharacterLiteral *>(&value)) return LLVMConstInt(ty_value, cl_literal->value, false);
	if (const auto *sl_literal = dynamic_cast<const cloth::StringLiteral *>(&value)) return string_value::literal(mod_module, *sl_literal);
	if (const auto *al_literal = dynamic_cast<const cloth::ArrayLiteral *>(&value)) {
		if (growable_array::ownsBuffer(destination))
			throw cde::ClothDiagnostic(cde::DiagnosticCode::UnsupportedLowering,
			                           "LLVM lowering requires a fixed-size array literal in an enum payload",
			                           { { .span = value.getSpan(), .text = "growable array initializer" } });
		const cloth::ResolvedType rt_element = destination.element();
		std::vector<LLVMValueRef> v_elements;
		v_elements.reserve(al_literal->elements.size());
		for (const cloth::ExpressionPtr &ep_element : al_literal->elements) v_elements.push_back(constant(*ep_element, rt_element));
		return LLVMConstArray2(LLVMGetElementType(ty_value), v_elements.data(), v_elements.size());
	}
	if (const auto *sl_literal = dynamic_cast<const cloth::StructLiteral *>(&value)) {
		if (sl_literal->fields.empty()) return LLVMConstNull(ty_value);
		const cloth::StructLayout &sl_layout = cp_program.structLayouts.at(destination);
		std::vector<LLVMValueRef>  v_fields;
		const unsigned             u_count = LLVMCountStructElementTypes(ty_value);
		v_fields.reserve(u_count);
		for (std::size_t st_index = 0; st_index < sl_literal->fields.size(); ++st_index)
			v_fields.push_back(constant(*sl_literal->fields[st_index], sl_layout.fields.at(st_index).type));
		// A fresh literal has no earlier array shapes. The current fields supply their own presence.
		while (v_fields.size() < u_count) v_fields.push_back(LLVMConstNull(LLVMStructGetTypeAtIndex(ty_value, static_cast<unsigned>(v_fields.size()))));
		return LLVMConstNamedStruct(ty_value, v_fields.data(), static_cast<unsigned>(v_fields.size()));
	}
	if (const auto *ce_cast = dynamic_cast<const cloth::CastExpression *>(&value);
	    ce_cast && !ei_value.symbol && ei_value.types == cp_program.expressions.at(ce_cast->operand.get()).types)
		return constant(*ce_cast->operand, destination);
	throw cde::ClothDiagnostic(cde::DiagnosticCode::UnsupportedLowering, "LLVM lowering requires a constant value for this enum payload",
	                           { { .span = value.getSpan(), .text = "unsupported payload initializer" } });
}

LLVMValueRef EnumPayloads::table (const cloth::Symbol &field, const cloth::TokenSpan &use) {
	if (const auto it_table = m_tables.find(&field); it_table != m_tables.end()) return it_table->second;
	const cloth::DeclarationInfo &di_field = cp_program.declarations.at(&field);
	const cloth::ResolvedType    &rt_field = di_field.value.value();
	const LLVMTypeRef             ty_field = vt_types.localType(rt_field);
	if (!ty_field || (rt_field.array() && growable_array::ownsBuffer(rt_field)))
		throw cde::ClothDiagnostic(cde::DiagnosticCode::UnsupportedLowering,
		                           "LLVM lowering does not support enum payload type '" + rt_field.spelling() + "'",
		                           { { .span = use, .text = "unsupported enum payload field" } });

	std::vector<LLVMValueRef> v_values;
	for (const cloth::Expression *e_value : cp_program.enumFieldValues(field)) v_values.push_back(constant(*e_value, rt_field));
	const LLVMValueRef ir_values = LLVMConstArray2(ty_field, v_values.data(), v_values.size());
	const std::string  s_name    = "cloth.enum." + di_field.owner->name.getNameKey() + "." + field.name.getNameKey();
	const LLVMValueRef ir_table  = LLVMAddGlobal(mod_module, LLVMTypeOf(ir_values), s_name.c_str());
	LLVMSetLinkage(ir_table, LLVMPrivateLinkage);
	LLVMSetGlobalConstant(ir_table, true);
	LLVMSetUnnamedAddress(ir_table, LLVMGlobalUnnamedAddr);
	LLVMSetInitializer(ir_table, ir_values);
	m_tables.emplace(&field, ir_table);
	return ir_table;
}

LLVMValueRef EnumPayloads::read (LLVMBuilderRef builder, const cloth::Symbol &field, LLVMValueRef tag, const cloth::TokenSpan &use) {
	const LLVMValueRef ir_table = table(field, use);
	const LLVMTypeRef  ty_table = LLVMGlobalGetValueType(ir_table);
	const LLVMTypeRef  ty_index = LLVMIntPtrTypeInContext(LLVMGetModuleContext(mod_module), LLVMGetModuleDataLayout(mod_module));
	// GEP sign-extends narrow indices. A case tag is an unsigned ordinal on both targets.
	LLVMValueRef       indices[]  = { LLVMConstNull(ty_index), LLVMBuildIntCast2(builder, tag, ty_index, false, "") };
	const LLVMValueRef ir_address = LLVMBuildInBoundsGEP2(builder, ty_table, ir_table, indices, 2, "");
	return LLVMBuildLoad2(builder, LLVMGetElementType(ty_table), ir_address, "");
}
}
