// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/driver/Objects.rst
// See Documentation/compiler/internals/Backend.rst

#include <backend/backend.hpp>
#include <backend/enum_payloads.hpp>
#include <backend/integer_range.hpp>
#include <backend/lowered_module.hpp>
#include <backend/nullable_value.hpp>
#include <backend/runtime.hpp>
#include <backend/scalar_lowering.hpp>
#include <backend/value_types.hpp>

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include <ast/decl/declarations.hpp>
#include <ast/expr/expressions.hpp>
#include <ast/expr/member_call_expression.hpp>
#include <ast/expr/provider_expressions.hpp>
#include <ast/expr/struct_expressions.hpp>
#include <ast/stmt/do_while_statement.hpp>
#include <ast/stmt/for_statement.hpp>
#include <ast/stmt/loop_control.hpp>
#include <ast/stmt/statements.hpp>
#include <ast/stmt/while_statement.hpp>
#include <cde/cloth_diagnostic.hpp>
#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <semantic/checked_program.hpp>
#include <semantic/target_layout.hpp>
#include <token/numeric_literals.hpp>
#include <token/token.hpp>

namespace backend {
namespace {

using detail::IntegerRange;
using Code          = cde::DiagnosticCode;
using Message       = std::unique_ptr<char, decltype(&LLVMDisposeMessage)>;
using TargetMachine = std::unique_ptr<LLVMOpaqueTargetMachine, decltype(&LLVMDisposeTargetMachine)>;
using TargetData    = std::unique_ptr<LLVMOpaqueTargetData, decltype(&LLVMDisposeTargetData)>;
using MemoryBuffer  = std::unique_ptr<LLVMOpaqueMemoryBuffer, decltype(&LLVMDisposeMemoryBuffer)>;

struct ModuleFunctions {
	const cloth::CheckedProgram                            &program;
	LLVMModuleRef                                           module;
	detail::ValueTypes                                      valueTypes{ module, program };
	detail::EnumPayloads                                    enumPayloads{ module, program, valueTypes };
	std::unordered_map<const cloth::Symbol *, LLVMValueRef> declarations = {};
	std::vector<const cloth::Symbol *>                      pending      = {};

	LLVMValueRef declareFunction (const cloth::Symbol &symbol);
};

LLVMValueRef ModuleFunctions::declareFunction (const cloth::Symbol &symbol) {
	if (const auto it_existing = declarations.find(&symbol); it_existing != declarations.end()) return it_existing->second;

	const bool                    b_entry     = &symbol == program.entryPoint;
	const cloth::DeclarationInfo &di_function = program.declarations.at(&symbol);
	const cloth::ResolvedType     rt_integer(cloth::kInteger);
	const bool                    b_integerResult = di_function.returns.size() == 1 && di_function.returns.front() == rt_integer;
	const bool                    b_voidResult    = di_function.returns.empty();
	if (b_entry && (!di_function.parameters.empty() || !b_integerResult)) {
		throw cde::ClothDiagnostic(Code::UNSUPPORTED_FUNCTION_ABI,
		                           std::format("LLVM lowering requires a parameterless entry point returning {}", cloth::to_string(cloth::kInteger)),
		                           { { .span = symbol.name.get_span(), .text = "unsupported entry-point signature" } });
	}
	const LLVMContextRef     ctx_context = LLVMGetModuleContext(module);
	const LLVMTypeRef        ty_result   = b_voidResult                    ? LLVMVoidTypeInContext(ctx_context) :
	                                       di_function.returns.size() == 1 ? valueTypes.directCallType(di_function.returns.front()) :
	                                                                         nullptr;
	std::vector<LLVMTypeRef> v_parameters;
	for (const cloth::ResolvedType &rt_parameter : di_function.parameters) v_parameters.push_back(valueTypes.directCallType(rt_parameter));
	if (!ty_result || std::ranges::find(v_parameters, nullptr) != v_parameters.end()) {
		throw cde::ClothDiagnostic(Code::UNSUPPORTED_FUNCTION_ABI, "LLVM lowering requires supported parameter types and at most one supported result",
		                           { { .span = symbol.name.get_span(), .text = "unsupported function signature" } });
	}
	const LLVMTypeRef  ty_function = LLVMFunctionType(ty_result, v_parameters.data(), static_cast<unsigned>(v_parameters.size()), false);
	const std::string  s_name      = b_entry ? "main" : "cloth." + symbol.name.get_name_key();
	const LLVMValueRef fn_function = LLVMAddFunction(module, s_name.c_str(), ty_function);
	// The executable object contains all reachable functions. Only its entry point needs external linkage.
	LLVMSetLinkage(fn_function, b_entry ? LLVMExternalLinkage : LLVMInternalLinkage);
	LLVMSetFunctionCallConv(fn_function, LLVMCCallConv);
	declarations.emplace(&symbol, fn_function);
	pending.push_back(&symbol);
	return fn_function;
}

enum class ValueStorage { Direct, ReadOnlySlot, MutableSlot };

struct LoweredValue {
	LLVMValueRef               operand;
	const cloth::ResolvedType &type;
	ValueStorage               storage;
};

struct LoopBlocks {
	LLVMBasicBlockRef continuation;
	LLVMBasicBlockRef exit;
};

class FunctionLowering {
    public:
	FunctionLowering (ModuleFunctions &functions, LLVMBuilderRef builder, const cloth::DeclarationInfo &declaration);
	void lowerFunction (const cloth::CallableSignature &signature, LLVMValueRef function);

    private:
	const LoweredValue         *findLocal (const cloth::Token *declaration) const;
	const LoweredValue         *namedLocal (const cloth::Expression &expression) const;
	LLVMValueRef                readValue (const LoweredValue &value, const cloth::ResolvedType &view);
	LLVMValueRef                storeInEntrySlot (LLVMValueRef value);
	const LoweredValue         &bindLocal (const cloth::Token &name, const cloth::ResolvedType &type, LLVMValueRef value, bool isMutable);
	std::optional<LoweredValue> lowerField (const cloth::FieldAccessExpression &field);

	LLVMValueRef      lowerStatements (const std::vector<cloth::StatementPtr> &statements, const cloth::ResolvedType *resultType = nullptr);
	void              lowerStatement (const cloth::Statement &statement);
	void              lowerReturn (const cloth::Expression *expression);
	LLVMValueRef      lowerIf (const cloth::IfExpression &conditional, const cloth::ExpressionInfo &info);
	void              lowerWhile (const cloth::WhileStatement &loop);
	void              lowerDoWhile (const cloth::DoWhileStatement &loop);
	void              lowerFor (const cloth::ForStatement &loop);
	IntegerRange      lowerRange (const cloth::Expression &expression, const cloth::ResolvedType &type);
	IntegerRange      lowerRangeBounds (const cloth::BinaryExpression &range, const cloth::ResolvedType &type);
	void              lowerIntegerRange (const cloth::ForStatement &loop, IntegerRange range, const cloth::ResolvedType &type);
	void              lowerLoopBody (const cloth::LoopStatement &loop, const cloth::LoopBody &body, LoopBlocks blocks);
	LLVMBasicBlockRef appendBlock (const char *name) const;
	LLVMBasicBlockRef branchToNewBlock (const char *name);
	LLVMBasicBlockRef loopExit (const cloth::LoopStatement &loop) const;
	LLVMBasicBlockRef loopContinuation (const cloth::LoopStatement &loop, const cloth::LoopBody &body, const char *name) const;
	void              branchOnLoopCondition (LLVMValueRef predicate, LLVMBasicBlockRef body, LLVMBasicBlockRef exit);
	void              branchOrUnreachable (LLVMBasicBlockRef target);

	LLVMValueRef lowerExpression (const cloth::Expression &expression);
	LLVMValueRef lowerUntilExit (const std::vector<cloth::ExpressionPtr> &elements);
	LLVMValueRef lowerArrayLiteral (const cloth::ArrayLiteral &literal, const cloth::ExpressionInfo &info);
	LLVMValueRef lowerArrayBuffer (const cloth::Expression &operand, bool snapshotMutable);
	LLVMValueRef tryLowerFixedArrayEquality (const cloth::BinaryExpression &binary, LLVMValueRef left, const cloth::ResolvedType &leftType,
	                                         const cloth::ResolvedType &rightType);
	LLVMValueRef lowerIndex (const cloth::IndexExpression &index, const cloth::ExpressionInfo &info);
	LLVMValueRef lowerArrayOffset (const cloth::Expression &index, LLVMTypeRef arrayType);
	LLVMValueRef arrayElementAddress (LLVMTypeRef arrayType, LLVMValueRef slot, LLVMValueRef offset) const;
	LLVMValueRef lowerStructLiteral (const cloth::StructLiteral &literal, const cloth::ExpressionInfo &info);
	LLVMValueRef lowerAs (const cloth::Expression &expression, const cloth::ResolvedType &destination, LLVMValueRef evaluated = nullptr);
	LLVMValueRef convertValue (LLVMValueRef value, const cloth::ResolvedType &source, const cloth::ResolvedType &destination,
	                           const cloth::ExpressionInfo *cast = nullptr);
	LLVMValueRef lowerBinary (const cloth::BinaryExpression &binary, LLVMValueRef left = nullptr);
	LLVMValueRef lowerRangeMembership (const cloth::BinaryExpression &binary);
	LLVMValueRef lowerNullableFallback (const cloth::ElvisExpression &expression, const cloth::ResolvedType &result);
	LLVMValueRef lowerLogical (const cloth::BinaryExpression &binary, LLVMValueRef left);
	LLVMValueRef lowerCall (const cloth::CallExpression &call);
	LLVMValueRef lowerPrintln (const cloth::CallExpression &call);
	LLVMValueRef lowerExit (const cloth::CallExpression &call, const cloth::BuiltinCall &builtin);

	void                lowerBinding (const cloth::BindingDeclaration &binding);
	void                lowerAssignment (const cloth::AssignmentStatement &assignment);
	void                lowerElementAssignment (const cloth::ElementAssignmentStatement &write);
	void                lowerFieldWrite (const cloth::FieldWriteStatement &write);
	void                storeExpression (const LoweredValue &target, const cloth::Expression &value);
	void                storeCompound (const LoweredValue &target, const cloth::BinaryExpression &compound);
	void                lowerUpdate (const LoweredValue &target, const cloth::Expression &read, cloth::Operator op);
	const LoweredValue &mutableTarget (const cloth::Statement &write, const cloth::Token &name) const;

	ModuleFunctions                                          &mf_functions;
	LLVMBuilderRef                                            ir_builder;
	const cloth::DeclarationInfo                             &di_function;
	detail::ScalarLowering                                    sl_scalars;
	std::unordered_map<const cloth::Token *, LoweredValue>    m_locals;
	std::unordered_map<const cloth::LoopTarget *, LoopBlocks> m_loopBlocks;
};

FunctionLowering::FunctionLowering (ModuleFunctions &functions, LLVMBuilderRef builder, const cloth::DeclarationInfo &declaration) :
        mf_functions(functions),
        ir_builder(builder),
        di_function(declaration),
        sl_scalars(functions.module, builder, functions.program.layout) {
}

const LoweredValue *FunctionLowering::findLocal (const cloth::Token *declaration) const {
	const auto it_local = m_locals.find(declaration);
	return it_local == m_locals.end() ? nullptr : &it_local->second;
}

const LoweredValue *FunctionLowering::namedLocal (const cloth::Expression &expression) const {
	if (!dynamic_cast<const cloth::NameExpression *>(&expression)) return nullptr;
	return findLocal(mf_functions.program.expressions.at(&expression).binding);
}

LLVMValueRef FunctionLowering::readValue (const LoweredValue &value, const cloth::ResolvedType &view) {
	const LLVMValueRef ir_value = value.storage == ValueStorage::Direct ?
	                                      value.operand :
	                                      LLVMBuildLoad2(ir_builder, mf_functions.valueTypes.localType(value.type), value.operand, "");
	// The checked read can see the payload while the local or field still stores the whole nullable.
	return value.type.nullable() && !view.nullable() ? detail::nullable::payload(ir_builder, ir_value) : ir_value;
}

bool isBuiltinOperation (const cloth::ExpressionInfo &info) {
	return !info.symbol && !info.leftContext && !info.rightContext && !info.argumentContract && !info.dispatch;
}

LLVMValueRef FunctionLowering::lowerAs (const cloth::Expression &expression, const cloth::ResolvedType &destination, LLVMValueRef evaluated) {
	const LLVMValueRef           ir_value  = evaluated ? evaluated : lowerExpression(expression);
	const cloth::ExpressionInfo &ei_source = mf_functions.program.expressions.at(&expression);
	if (!ei_source.fallsThrough) return nullptr;
	return convertValue(ir_value, ei_source.types.front(), destination);
}

LLVMValueRef FunctionLowering::convertValue (LLVMValueRef value, const cloth::ResolvedType &source, const cloth::ResolvedType &destination,
                                             const cloth::ExpressionInfo *cast) {
	if (source == destination) return value;
	if (!destination.nullable()) return cast ? sl_scalars.checkedCast(value, source, destination, *cast) : sl_scalars.convert(value, source, destination);
	const cloth::ResolvedType rt_payload = destination.element();
	if (source == cloth::ResolvedType(cloth::kNull)) return LLVMConstNull(mf_functions.valueTypes.localType(destination));
	if (source == rt_payload) return detail::nullable::pack(ir_builder, value);
	const LLVMValueRef        ir_presence = source.nullable() ? detail::nullable::presence(ir_builder, value) : nullptr;
	const cloth::ResolvedType rt_from     = source.nullable() ? source.element() : source;
	if (ir_presence) {
		value = detail::nullable::payload(ir_builder, value);
		// An absent payload must pass through a checked float-to-integer cast without raising a runtime error.
		if (rt_from.floating() && rt_payload.integer())
			value = LLVMBuildSelect(ir_builder, sl_scalars.predicate(ir_presence), value, LLVMConstNull(LLVMTypeOf(value)), "");
	}
	const LLVMValueRef ir_payload = cast ? sl_scalars.checkedCast(value, rt_from, rt_payload, *cast) : sl_scalars.convert(value, rt_from, rt_payload);
	return detail::nullable::pack(ir_builder, ir_payload, ir_presence);
}

LLVMValueRef FunctionLowering::lowerExpression (const cloth::Expression &expression) {
	const cloth::BodyFacts       &bf_facts       = mf_functions.program;
	const cloth::ExpressionFacts &ef_expressions = bf_facts.expressions;
	const cloth::ExpressionInfo  &ei_value       = ef_expressions.at(&expression);
	const cloth::ResolvedType    *rt_result      = ei_value.resultKind() == cloth::ResultKind::Single ? &ei_value.types.front() : nullptr;
	if (const auto *ie_conditional = dynamic_cast<const cloth::IfExpression *>(&expression)) {
		return lowerIf(*ie_conditional, ei_value);
	}
	if (const auto *ee_fallback = dynamic_cast<const cloth::ElvisExpression *>(&expression); ee_fallback && isBuiltinOperation(ei_value)) {
		if (!ef_expressions.at(ee_fallback->left.get()).fallsThrough) return lowerExpression(*ee_fallback->left);
		if (rt_result && mf_functions.valueTypes.localType(*rt_result)) return lowerNullableFallback(*ee_fallback, *rt_result);
		if (!ei_value.fallsThrough && dynamic_cast<const cloth::NullLiteral *>(ee_fallback->left.get())) return lowerExpression(*ee_fallback->fallback);
	}
	if (const auto *sl_literal = dynamic_cast<const cloth::StructLiteral *>(&expression)) return lowerStructLiteral(*sl_literal, ei_value);
	if (const auto *al_literal = dynamic_cast<const cloth::ArrayLiteral *>(&expression)) return lowerArrayLiteral(*al_literal, ei_value);
	if (const auto *ie_index = dynamic_cast<const cloth::IndexExpression *>(&expression); ie_index && !ei_value.indexed && isBuiltinOperation(ei_value))
		return lowerIndex(*ie_index, ei_value);
	if (const auto *fa_read = dynamic_cast<const cloth::FieldAccessExpression *>(&expression)) {
		if (ei_value.symbol && ei_value.symbol->kind == cloth::SymbolKind::EnumCase)
			return LLVMConstInt(sl_scalars.type(*rt_result), ei_value.symbol->enumCaseOrdinal, false);
		if (ei_value.symbol && std::holds_alternative<const cloth::EnumFieldDeclaration *>(ei_value.symbol->declaration)) {
			const LLVMValueRef ir_tag = lowerExpression(*fa_read->receiver);
			if (!ir_tag) return nullptr;
			const LLVMValueRef ir_field = mf_functions.enumPayloads.read(ir_builder, *ei_value.symbol, ir_tag, fa_read->get_span());
			return readValue({ ir_field, *mf_functions.program.declarations.at(ei_value.symbol).value, ValueStorage::Direct }, *rt_result);
		}
		const std::optional<LoweredValue> opt_field = lowerField(*fa_read);
		return opt_field ? readValue(*opt_field, *rt_result) : nullptr;
	}
	if (dynamic_cast<const cloth::NullLiteral *>(&expression) && rt_result) {
		if (const LLVMTypeRef ty_null = mf_functions.valueTypes.localType(*rt_result)) return LLVMConstNull(ty_null);
	}
	if (dynamic_cast<const cloth::NumberLiteral *>(&expression) || dynamic_cast<const cloth::BooleanLiteral *>(&expression)) {
		return detail::scalarConstant(sl_scalars.type(*rt_result), ei_value.numericConstant);
	}
	if (const auto *cl_literal = dynamic_cast<const cloth::CharacterLiteral *>(&expression)) {
		return LLVMConstInt(sl_scalars.type(*rt_result), cl_literal->value, false);
	}
	if (const LoweredValue *lv_local = namedLocal(expression)) return readValue(*lv_local, *rt_result);
	if (const auto *ce_call = dynamic_cast<const cloth::CallExpression *>(&expression)) {
		return lowerCall(*ce_call);
	}
	if (const auto *ce_cast = dynamic_cast<const cloth::CastExpression *>(&expression); ce_cast && isBuiltinOperation(ei_value)) {
		if (!ei_value.fallsThrough || ei_value.resultKind() == cloth::ResultKind::Empty) {
			lowerExpression(*ce_cast->operand);
			return nullptr;
		}
		const cloth::ResultTypes &v_source = ef_expressions.at(ce_cast->operand.get()).types;
		if (rt_result && mf_functions.valueTypes.localType(*rt_result) && v_source.size() == 1 && mf_functions.valueTypes.localType(v_source.front()))
			return convertValue(lowerExpression(*ce_cast->operand), v_source.front(), *rt_result, &ei_value);
	}
	if (const auto *mc_call = dynamic_cast<const cloth::MemberCallExpression *>(&expression);
	    mc_call && ei_value.builtin && ei_value.builtin->operation == cloth::BuiltinOperation::ToInt) {
		if (!ei_value.fallsThrough) return lowerExpression(*mc_call->receiver);
		return lowerAs(*mc_call->receiver, *rt_result);
	}
	if (const auto *pa_provider = dynamic_cast<const cloth::ProviderAccessExpression *>(&expression)) {
		if (!ei_value.fallsThrough && isBuiltinOperation(ei_value)) return lowerExpression(*pa_provider->receiver);
		if (ei_value.provider && (sl_scalars.type(ei_value.provider->receiver) || ei_value.provider->kind == cloth::ProviderKind::ArraySize) &&
		    !std::holds_alternative<std::monostate>(ei_value.numericConstant)) {
			if (!ei_value.provider->typeReceiver) lowerExpression(*pa_provider->receiver);
			return detail::scalarConstant(sl_scalars.type(*rt_result), ei_value.numericConstant);
		}
	}
	if (const auto *tc_test = dynamic_cast<const cloth::TypeCheckExpression *>(&expression)) {
		if (const auto it_test = bf_facts.valueTests.find(tc_test); it_test != bf_facts.valueTests.end()) return lowerBinary(*it_test->second);
		if (!ei_value.fallsThrough) return lowerExpression(*tc_test->operand);
		if (ei_value.testedType && sl_scalars.type(*ei_value.testedType)) {
			const cloth::ResolvedType &rt_source = ef_expressions.at(tc_test->operand.get()).types.front();
			if (sl_scalars.type(rt_source)) {
				lowerExpression(*tc_test->operand);
				return LLVMConstInt(sl_scalars.type(*rt_result), rt_source == *ei_value.testedType, false);
			}
		}
	}
	if (const auto *ue_unary = dynamic_cast<const cloth::UnaryExpression *>(&expression); ue_unary && isBuiltinOperation(ei_value)) {
		if (!ei_value.fallsThrough) return lowerExpression(*ue_unary->operand);
		if (rt_result && sl_scalars.type(*rt_result)) {
			const LLVMValueRef ir_operand = lowerExpression(*ue_unary->operand);
			switch (ue_unary->op.get_operator()) {
			case cloth::oPlus:
				return ir_operand;
			case cloth::oMinus:
				return rt_result->floating() ? LLVMBuildFNeg(ir_builder, ir_operand, "") : LLVMBuildNeg(ir_builder, ir_operand, "");
			case cloth::oTilde:
				return LLVMBuildNot(ir_builder, ir_operand, "");
			case cloth::oNot:
				return sl_scalars.boolean(LLVMBuildNot(ir_builder, sl_scalars.predicate(ir_operand), ""));
			default:
				break;
			}
		}
	}
	if (const auto *be_binary = dynamic_cast<const cloth::BinaryExpression *>(&expression)) {
		return lowerBinary(*be_binary);
	}
	throw cde::ClothDiagnostic(Code::UNSUPPORTED_LOWERING, "LLVM lowering does not support this expression",
	                           { { expression.get_span(), "unsupported expression" } });
}

LLVMValueRef FunctionLowering::lowerUntilExit (const std::vector<cloth::ExpressionPtr> &elements) {
	for (const cloth::ExpressionPtr &ep_element : elements) {
		lowerExpression(*ep_element);
		if (!mf_functions.program.expressions.at(ep_element.get()).fallsThrough) break;
	}
	return nullptr;
}

LLVMValueRef FunctionLowering::lowerArrayLiteral (const cloth::ArrayLiteral &literal, const cloth::ExpressionInfo &info) {
	struct DynamicElement {
		unsigned     index;
		LLVMValueRef value;
	};

	if (!info.fallsThrough) return lowerUntilExit(literal.elements);
	const cloth::ResolvedType &rt_array = info.types.front();
	const LLVMTypeRef          ty_array = mf_functions.valueTypes.localType(rt_array);
	if (!ty_array)
		throw cde::ClothDiagnostic(Code::UNSUPPORTED_LOWERING, "LLVM lowering requires a fixed array with supported element storage",
		                           { { literal.get_span(), "unsupported array literal" } });
	const cloth::ResolvedType   rt_element = rt_array.element();
	const LLVMTypeRef           ty_element = LLVMGetElementType(ty_array);
	std::vector<LLVMValueRef>   v_constants;
	std::vector<DynamicElement> v_dynamic;
	v_constants.reserve(literal.elements.size());
	for (const cloth::ExpressionPtr &ep_element : literal.elements) {
		LLVMValueRef ir_element = lowerAs(*ep_element, rt_element);
		if (!LLVMIsConstant(ir_element)) {
			v_dynamic.push_back({ static_cast<unsigned>(v_constants.size()), ir_element });
			ir_element = LLVMConstNull(ty_element);
		}
		v_constants.push_back(ir_element);
	}
	// Building one constant avoids retaining a full intermediate array for every literal element.
	LLVMValueRef ir_array = LLVMConstArray2(ty_element, v_constants.data(), v_constants.size());
	for (const DynamicElement &de_element : v_dynamic) ir_array = LLVMBuildInsertValue(ir_builder, ir_array, de_element.value, de_element.index, "");
	return ir_array;
}

LLVMValueRef FunctionLowering::lowerArrayOffset (const cloth::Expression &index, LLVMTypeRef arrayType) {
	const LLVMValueRef ir_index = lowerExpression(index);
	if (!ir_index) return nullptr;
	const cloth::ResolvedType &rt_index = mf_functions.program.expressions.at(&index).types.front();
	return sl_scalars.checkedArrayIndex(ir_index, rt_index.unsignedInteger(), LLVMGetArrayLength2(arrayType));
}

LLVMValueRef FunctionLowering::arrayElementAddress (LLVMTypeRef arrayType, LLVMValueRef slot, LLVMValueRef offset) const {
	LLVMValueRef v_indices[] = { LLVMConstNull(LLVMTypeOf(offset)), offset };
	return LLVMBuildGEP2(ir_builder, arrayType, slot, v_indices, 2, "");
}

LLVMValueRef FunctionLowering::lowerArrayBuffer (const cloth::Expression &operand, bool snapshotMutable) {
	const LoweredValue *lv_local = namedLocal(operand);
	if (lv_local && lv_local->storage != ValueStorage::Direct && (!snapshotMutable || lv_local->storage == ValueStorage::ReadOnlySlot))
		return lv_local->operand;
	return storeInEntrySlot(lowerExpression(operand));
}

LLVMValueRef FunctionLowering::tryLowerFixedArrayEquality (const cloth::BinaryExpression &binary, LLVMValueRef left, const cloth::ResolvedType &leftType,
                                                           const cloth::ResolvedType &rightType) {
	const std::optional<std::uint64_t> opt_leftLength = leftType.arrayExtent(), opt_rightLength = rightType.arrayExtent();
	if (!opt_leftLength || !opt_rightLength) return nullptr;
	const cloth::ResolvedType  rt_element     = leftType.element();
	const cloth::ResolvedType *rt_endpoint    = cloth::semantic::rangeEndpoint(rt_element);
	const bool                 b_integerRange = rt_endpoint && rt_endpoint->integer();
	const bool                 b_nullable     = rt_element.nullable();
	if (!b_integerRange && !sl_scalars.type(b_nullable ? rt_element.element() : rt_element)) return nullptr;
	const std::uint64_t u64_length = *opt_leftLength;
	if (!u64_length || u64_length != *opt_rightLength) {
		// A result known from the extents still observes both operands in source order.
		if (!left) lowerExpression(*binary.left);
		lowerExpression(*binary.right);
		return LLVMConstInt(LLVMInt1TypeInContext(LLVMGetModuleContext(mf_functions.module)), u64_length == *opt_rightLength, false);
	}
	// A computed right operand can change a mutable left local before the first element comparison.
	const LLVMValueRef      ir_leftSlot  = left ? storeInEntrySlot(left) : lowerArrayBuffer(*binary.left, !namedLocal(*binary.right));
	const LLVMValueRef      ir_rightSlot = lowerArrayBuffer(*binary.right, false);
	const LLVMTypeRef       ty_array     = LLVMGetAllocatedType(ir_leftSlot);
	const LLVMTypeRef       ty_element   = LLVMGetElementType(ty_array);
	const LLVMTypeRef       ty_offset    = sl_scalars.type(cloth::ResolvedType(cloth::kUsize));
	const LLVMBasicBlockRef bb_start     = LLVMGetInsertBlock(ir_builder);
	const LLVMBasicBlockRef bb_compare   = branchToNewBlock("array.equal.element");
	const LLVMBasicBlockRef bb_end       = appendBlock("array.equal.end");
	const LLVMValueRef      ir_offset    = LLVMBuildPhi(ir_builder, ty_offset, "");
	const LLVMValueRef      ir_left      = LLVMBuildLoad2(ir_builder, ty_element, arrayElementAddress(ty_array, ir_leftSlot, ir_offset), "");
	const LLVMValueRef      ir_right     = LLVMBuildLoad2(ir_builder, ty_element, arrayElementAddress(ty_array, ir_rightSlot, ir_offset), "");
	const LLVMValueRef      ir_equal     = b_integerRange ? IntegerRange::equalPacked(ir_builder, ir_left, ir_right, b_nullable) :
	                                                        sl_scalars.compare(cloth::oEqual, ir_left, ir_right, rt_element);
	const LLVMValueRef      ir_next      = LLVMBuildAdd(ir_builder, ir_offset, LLVMConstInt(ty_offset, 1, false), "");
	LLVMValueRef            v_offsets[]  = { LLVMConstNull(ty_offset), ir_next };
	LLVMBasicBlockRef       v_blocks[]   = { bb_start, bb_compare };
	LLVMAddIncoming(ir_offset, v_offsets, v_blocks, 2);
	const LLVMValueRef ir_more = LLVMBuildICmp(ir_builder, LLVMIntNE, ir_next, LLVMConstInt(ty_offset, u64_length, false), "");
	LLVMBuildCondBr(ir_builder, LLVMBuildAnd(ir_builder, ir_equal, ir_more, ""), bb_compare, bb_end);
	LLVMPositionBuilderAtEnd(ir_builder, bb_end);
	return ir_equal;
}

LLVMValueRef FunctionLowering::lowerIndex (const cloth::IndexExpression &index, const cloth::ExpressionInfo &info) {
	const cloth::ExpressionFacts &ef_facts = mf_functions.program.expressions;
	const cloth::ExpressionInfo  &ei_array = ef_facts.at(index.array.get());
	const LoweredValue           *lv_local = namedLocal(*index.array);
	// A simple index cannot rebind its receiver. A read-only local keeps its buffer throughout any index expression.
	const bool b_direct =
	        lv_local && lv_local->storage != ValueStorage::Direct &&
	        (lv_local->storage == ValueStorage::ReadOnlySlot || dynamic_cast<const cloth::IntegerLiteral *>(index.index.get()) || namedLocal(*index.index));
	const LLVMValueRef ir_array = b_direct ? nullptr : lowerExpression(*index.array);
	if (!b_direct && !ir_array) return nullptr;
	const LLVMTypeRef  ty_array  = b_direct ? LLVMGetAllocatedType(lv_local->operand) : LLVMTypeOf(ir_array);
	const LLVMValueRef ir_offset = lowerArrayOffset(*index.index, ty_array);
	if (!ir_offset) return nullptr;
	const cloth::ResolvedType rt_element = ei_array.types.front().element();
	if (ir_array && LLVMIsAConstantInt(ir_offset)) {
		const std::uint64_t u64_offset = LLVMConstIntGetZExtValue(ir_offset);
		if (u64_offset < LLVMGetArrayLength2(ty_array) && u64_offset <= UINT_MAX) {
			const LLVMValueRef ir_element = LLVMBuildExtractValue(ir_builder, ir_array, static_cast<unsigned>(u64_offset), "");
			return readValue({ ir_element, rt_element, ValueStorage::Direct }, info.types.front());
		}
	}
	// Keep the receiver's value from before index evaluation, even if the index reassigns the local.
	const LLVMValueRef ir_slot    = b_direct ? lv_local->operand : storeInEntrySlot(ir_array);
	const LLVMValueRef ir_address = arrayElementAddress(ty_array, ir_slot, ir_offset);
	return readValue({ ir_address, rt_element, ValueStorage::ReadOnlySlot }, info.types.front());
}

LLVMValueRef FunctionLowering::lowerStructLiteral (const cloth::StructLiteral &literal, const cloth::ExpressionInfo &info) {
	const cloth::CheckedProgram &cp_program = mf_functions.program;
	if (!info.fallsThrough) return lowerUntilExit(literal.fields);
	const cloth::ResolvedType &rt_struct = info.types.front();
	const LLVMTypeRef          ty_struct = mf_functions.valueTypes.get(rt_struct);
	if (!ty_struct) {
		throw cde::ClothDiagnostic(Code::UNSUPPORTED_LOWERING, "LLVM lowering requires a supported storage type for every struct field",
		                           { { literal.get_span(), "unsupported struct literal" } });
	}
	const cloth::StructLayout &sl_layout = cp_program.structLayouts.at(rt_struct);
	LLVMValueRef               ir_struct = LLVMConstNull(ty_struct);
	for (std::size_t st_index = 0; st_index < literal.fields.size(); ++st_index) {
		const LLVMValueRef ir_field = lowerAs(*literal.fields[st_index], sl_layout.fields.at(st_index).type);
		ir_struct                   = LLVMBuildInsertValue(ir_builder, ir_struct, ir_field, static_cast<unsigned>(st_index), "");
	}
	return ir_struct;
}

std::optional<LoweredValue> FunctionLowering::lowerField (const cloth::FieldAccessExpression &field) {
	struct FieldStep {
		LLVMTypeRef                owner;
		unsigned                   index;
		const cloth::ResolvedType &type;
	};

	const cloth::CheckedProgram &cp_program = mf_functions.program;
	const cloth::Expression     *e_root     = &field;
	std::vector<FieldStep>       v_path;
	while (const auto *fa_step = dynamic_cast<const cloth::FieldAccessExpression *>(e_root)) {
		e_root                                   = fa_step->receiver.get();
		const cloth::ExpressionInfo &ei_receiver = cp_program.expressions.at(e_root);
		if (!ei_receiver.fallsThrough) {
			lowerExpression(*e_root);
			return std::nullopt;
		}
		const cloth::Symbol *sym_field = cp_program.expressions.at(fa_step).symbol;
		const LLVMTypeRef ty_struct = ei_receiver.resultKind() == cloth::ResultKind::Single ? mf_functions.valueTypes.get(ei_receiver.types.front()) :
		                                                                                      nullptr;
		if (!sym_field || !ty_struct || LLVMGetTypeKind(ty_struct) != LLVMStructTypeKind) {
			throw cde::ClothDiagnostic(Code::UNSUPPORTED_LOWERING, "LLVM lowering requires a struct field with a supported storage type",
			                           { { fa_step->get_span(), "unsupported field access" } });
		}
		const cloth::StructLayout &sl_layout = cp_program.structLayouts.at(ei_receiver.types.front());
		const unsigned u_index = LLVMElementAtOffset(LLVMGetModuleDataLayout(mf_functions.module), ty_struct, sl_layout.fieldOffsets.at(sym_field));
		v_path.push_back({ ty_struct, u_index, sl_layout.fields[u_index].type });
		if (!ei_receiver.symbol || !std::holds_alternative<const cloth::StructFieldDeclaration *>(ei_receiver.symbol->declaration)) break;
	}

	const LoweredValue *lv_root    = namedLocal(*e_root);
	const ValueStorage  vs_storage = lv_root ? lv_root->storage : ValueStorage::Direct;
	const bool          b_address  = vs_storage != ValueStorage::Direct;
	LLVMValueRef        ir_operand = lv_root ? lv_root->operand : lowerExpression(*e_root);
	bool                b_nullable = lv_root && lv_root->type.nullable();
	// A mutable root keeps the address of its subobject even when the right-hand side replaces a parent.
	for (const FieldStep &fs_step : std::views::reverse(v_path)) {
		// A presence proof exposes the payload inside the nullable receiver's storage.
		if (b_nullable)
			ir_operand = b_address ? detail::nullable::payloadAddress(ir_builder, fs_step.owner, ir_operand) :
			                         detail::nullable::payload(ir_builder, ir_operand);
		ir_operand = b_address ? LLVMBuildStructGEP2(ir_builder, fs_step.owner, ir_operand, fs_step.index, "") :
		                         LLVMBuildExtractValue(ir_builder, ir_operand, fs_step.index, "");
		b_nullable = fs_step.type.nullable();
	}
	return LoweredValue{ .operand = ir_operand, .type = v_path.front().type, .storage = vs_storage };
}

LLVMValueRef FunctionLowering::lowerLogical (const cloth::BinaryExpression &binary, LLVMValueRef left) {
	const bool b_and = binary.op.get_operator() == cloth::oAnd;
	if (!left) return nullptr;
	// When the whole expression transfers control, there is no short-circuit path past the right operand.
	if (!mf_functions.program.expressions.at(&binary).fallsThrough) return lowerExpression(*binary.right);
	LLVMBasicBlockRef       bb_left  = LLVMGetInsertBlock(ir_builder);
	LLVMBasicBlockRef       bb_right = appendBlock("logical.rhs");
	const LLVMBasicBlockRef bb_end   = appendBlock("logical.end");
	LLVMBuildCondBr(ir_builder, sl_scalars.predicate(left), b_and ? bb_right : bb_end, b_and ? bb_end : bb_right);
	LLVMPositionBuilderAtEnd(ir_builder, bb_right);
	LLVMValueRef ir_right = lowerExpression(*binary.right);
	if (!ir_right) {
		LLVMPositionBuilderAtEnd(ir_builder, bb_end);
		return left;
	}
	bb_right = LLVMGetInsertBlock(ir_builder);
	LLVMBuildBr(ir_builder, bb_end);
	LLVMPositionBuilderAtEnd(ir_builder, bb_end);
	const LLVMValueRef ir_result = LLVMBuildPhi(ir_builder, LLVMTypeOf(left), "");
	LLVMAddIncoming(ir_result, &left, &bb_left, 1);
	LLVMAddIncoming(ir_result, &ir_right, &bb_right, 1);
	return ir_result;
}

LLVMValueRef FunctionLowering::lowerNullableFallback (const cloth::ElvisExpression &expression, const cloth::ResolvedType &result) {
	if (dynamic_cast<const cloth::NullLiteral *>(expression.left.get())) return lowerAs(*expression.fallback, result);
	const cloth::ResolvedType &rt_left = mf_functions.program.expressions.at(expression.left.get()).types.front();
	const LLVMValueRef         ir_left = lowerExpression(*expression.left);
	if (!rt_left.nullable()) return convertValue(ir_left, rt_left, result);
	const LLVMValueRef      ir_present = result.nullable() ? ir_left : detail::nullable::payload(ir_builder, ir_left);
	const LLVMBasicBlockRef bb_left    = LLVMGetInsertBlock(ir_builder);
	const LLVMBasicBlockRef bb_missing = appendBlock("nullable.fallback");
	const LLVMBasicBlockRef bb_end     = appendBlock("nullable.end");
	LLVMBuildCondBr(ir_builder, sl_scalars.predicate(detail::nullable::presence(ir_builder, ir_left)), bb_end, bb_missing);
	LLVMPositionBuilderAtEnd(ir_builder, bb_missing);
	LLVMValueRef      ir_fallback = lowerAs(*expression.fallback, result);
	LLVMBasicBlockRef bb_exit     = LLVMGetInsertBlock(ir_builder);
	if (ir_fallback) LLVMBuildBr(ir_builder, bb_end);
	LLVMPositionBuilderAtEnd(ir_builder, bb_end);
	if (!ir_fallback) return ir_present;
	const LLVMValueRef ir_result  = LLVMBuildPhi(ir_builder, LLVMTypeOf(ir_present), "");
	LLVMValueRef       v_values[] = { ir_present, ir_fallback };
	LLVMBasicBlockRef  v_blocks[] = { bb_left, bb_exit };
	LLVMAddIncoming(ir_result, v_values, v_blocks, 2);
	return ir_result;
}

LLVMValueRef FunctionLowering::lowerRangeMembership (const cloth::BinaryExpression &binary) {
	const cloth::ExpressionFacts &ef_facts      = mf_functions.program.expressions;
	const cloth::ResolvedType    &rt_left       = ef_facts.at(binary.left.get()).types.front();
	const bool                    b_rangeOnLeft = cloth::semantic::rangeEndpoint(rt_left) != nullptr;
	const cloth::ResolvedType    &rt_member     = b_rangeOnLeft ? ef_facts.at(binary.right.get()).types.front() : rt_left;
	if (!rt_member.integer())
		throw cde::ClothDiagnostic(Code::UNSUPPORTED_LOWERING, "LLVM range membership requires integer endpoints",
		                           { { binary.get_span(), "unsupported range membership" } });

	// Equality permits either order. Evaluate the member on its written side of the range.
	LLVMValueRef       ir_value = b_rangeOnLeft ? nullptr : lowerExpression(*binary.left);
	const IntegerRange range    = lowerRange(b_rangeOnLeft ? *binary.left : *binary.right, rt_member);
	if (b_rangeOnLeft) ir_value = lowerExpression(*binary.right);
	const LLVMValueRef ir_lower  = sl_scalars.compare(cloth::oLessEqual, range.start, ir_value, rt_member);
	const LLVMValueRef ir_upper  = sl_scalars.withinRangeEnd(ir_value, range.end, range.inclusive, rt_member);
	const LLVMValueRef ir_member = LLVMBuildAnd(ir_builder, ir_lower, ir_upper, "");
	return sl_scalars.boolean(binary.op.get_operator() == cloth::oNotEqual ? LLVMBuildNot(ir_builder, ir_member, "") : ir_member);
}

LLVMValueRef FunctionLowering::lowerBinary (const cloth::BinaryExpression &binary, LLVMValueRef left) {
	const cloth::ExpressionFacts &ef_facts  = mf_functions.program.expressions;
	const cloth::ExpressionInfo  &ei_binary = ef_facts.at(&binary);
	const cloth::Operator         o_op      = binary.op.get_operator();
	if (ei_binary.range && isBuiltinOperation(ei_binary)) {
		switch (*ei_binary.range) {
		case cloth::RangeOperation::Construct: {
			const cloth::ResolvedType *rt_endpoint = cloth::semantic::rangeEndpoint(ei_binary.types.front());
			if (rt_endpoint && rt_endpoint->integer()) return lowerRangeBounds(binary, *rt_endpoint).pack(ir_builder);
			break;
		}
		case cloth::RangeOperation::Membership:
			return lowerRangeMembership(binary);
		case cloth::RangeOperation::Equal: {
			const cloth::ResolvedType &rt_left     = ef_facts.at(binary.left.get()).types.front();
			const cloth::ResolvedType &rt_right    = ef_facts.at(binary.right.get()).types.front();
			const cloth::ResolvedType *rt_endpoint = cloth::semantic::rangeEndpoint(rt_left);
			if (!rt_endpoint || !rt_endpoint->integer()) break;
			LLVMValueRef ir_equal;
			if (rt_left.nullable() || rt_right.nullable()) {
				const cloth::ResolvedType &rt_nullable = rt_left.nullable() ? rt_left : rt_right;
				const LLVMValueRef         ir_left     = lowerAs(*binary.left, rt_nullable);
				const LLVMValueRef         ir_right    = lowerAs(*binary.right, rt_nullable);
				ir_equal                               = IntegerRange::equalPacked(ir_builder, ir_left, ir_right, true);
			} else {
				const IntegerRange range_left  = lowerRange(*binary.left, *rt_endpoint);
				const IntegerRange range_right = lowerRange(*binary.right, *rt_endpoint);
				ir_equal                       = range_left.equal(ir_builder, range_right);
			}
			return sl_scalars.boolean(o_op == cloth::oNotEqual ? LLVMBuildNot(ir_builder, ir_equal, "") : ir_equal);
		}
		}
	}
	if (isBuiltinOperation(ei_binary) && !ei_binary.range) {
		if (o_op == cloth::oAnd || o_op == cloth::oOr) return lowerLogical(binary, left ? left : lowerExpression(*binary.left));
		const cloth::ExpressionInfo &ei_left = ef_facts.at(binary.left.get());
		if (!ei_binary.fallsThrough) {
			// The checker can record an exit before it selects an operand type for the operation.
			if (!left) lowerExpression(*binary.left);
			if (ei_left.fallsThrough) lowerExpression(*binary.right);
			return nullptr;
		}
		const cloth::ExpressionInfo &ei_right = ef_facts.at(binary.right.get());
		if ((o_op == cloth::oEqual || o_op == cloth::oNotEqual) && !ei_binary.callsElementEquality && !ei_binary.elementEquality &&
		    ei_left.resultKind() == cloth::ResultKind::Single && ei_right.resultKind() == cloth::ResultKind::Single) {
			if (const LLVMValueRef ir_equal = tryLowerFixedArrayEquality(binary, left, ei_left.types.front(), ei_right.types.front()))
				return sl_scalars.boolean(o_op == cloth::oEqual ? ir_equal : LLVMBuildNot(ir_builder, ir_equal, ""));
		}
		if (ei_binary.comparisonType && ei_binary.comparisonType->nullable() && (o_op == cloth::oEqual || o_op == cloth::oNotEqual)) {
			const cloth::ResolvedType &rt_operand = *ei_binary.comparisonType;
			const cloth::ResolvedType  rt_null(cloth::kNull);
			const bool                 b_leftNull     = ei_left.types.front() == rt_null;
			const bool                 b_comparesNull = b_leftNull || ei_right.types.front() == rt_null;
			if ((b_comparesNull && mf_functions.valueTypes.localType(rt_operand)) || sl_scalars.type(rt_operand.element())) {
				const LLVMValueRef ir_left  = lowerAs(*binary.left, rt_operand, left);
				const LLVMValueRef ir_right = lowerAs(*binary.right, rt_operand);
				if (!b_comparesNull) return sl_scalars.binary(o_op, ir_left, ir_right, rt_operand);
				const LLVMValueRef ir_flag = detail::nullable::presence(ir_builder, b_leftNull ? ir_right : ir_left);
				return o_op == cloth::oNotEqual ? ir_flag : sl_scalars.boolean(LLVMBuildIsNull(ir_builder, ir_flag, ""));
			}
		}
		if ((!cloth::isComparison(o_op) || ei_binary.booleanEquality || ei_binary.comparisonType) &&
		    ei_binary.resultKind() == cloth::ResultKind::Single && ei_left.resultKind() == cloth::ResultKind::Single &&
		    ei_right.resultKind() == cloth::ResultKind::Single && sl_scalars.type(ei_left.types.front()) && sl_scalars.type(ei_right.types.front())) {
			// Boolean equality uses the boolean result type to normalize both inputs.
			const cloth::ResolvedType &rt_operand = ei_binary.comparisonType ? *ei_binary.comparisonType : ei_binary.types.front();
			const LLVMValueRef         ir_left    = lowerAs(*binary.left, rt_operand, left);
			const LLVMValueRef         ir_right   = lowerAs(*binary.right, rt_operand);
			return sl_scalars.binary(o_op, ir_left, ir_right, rt_operand);
		}
	}
	throw cde::ClothDiagnostic(Code::UNSUPPORTED_LOWERING,
	                           std::format("LLVM lowering does not support binary operator '{}' for these operands", binary.op.get_lexeme()),
	                           { { .span = binary.get_span(), .text = "unsupported binary expression" } });
}

LLVMValueRef FunctionLowering::lowerCall (const cloth::CallExpression &call) {
	const cloth::ExpressionInfo &ei_call = mf_functions.program.expressions.at(&call);
	if (ei_call.builtin) {
		switch (ei_call.builtin->operation) {
		case cloth::BuiltinOperation::Println:
			return lowerPrintln(call);
		case cloth::BuiltinOperation::Exit:
			return lowerExit(call, *ei_call.builtin);
		default:
			break;
		}
	}
	const cloth::Symbol *sym_callee = ei_call.symbol;
	if (!sym_callee || sym_callee->kind != cloth::SymbolKind::Function || ei_call.instance || ei_call.dispatch) {
		throw cde::ClothDiagnostic(Code::UNSUPPORTED_LOWERING, "LLVM lowering requires a direct call to a non-generic free function",
		                           { { call.get_span(), "unsupported call" } });
	}
	if (!ei_call.omittedParameters.empty()) {
		throw cde::ClothDiagnostic(Code::UNSUPPORTED_LOWERING, "LLVM lowering requires an explicit argument for each parameter",
		                           { { call.get_span(), "omitted argument" } });
	}
	// Validate the callee's ABI before its arguments when evaluation can reach the call.
	const LLVMValueRef        fn_callee = ei_call.fallsThrough ? mf_functions.declareFunction(*sym_callee) : nullptr;
	std::vector<LLVMValueRef> v_arguments(call.suppliedArguments.size());
	// Evaluate in source order, then pass each value in its checked parameter position.
	for (std::size_t st_index = 0; st_index < call.suppliedArguments.size(); ++st_index) {
		const std::size_t  st_parameter = ei_call.parameterForArgument.at(st_index);
		const LLVMValueRef ir_argument =
		        lowerAs(*call.suppliedArguments[st_index].expression, mf_functions.program.declarations.at(sym_callee).parameters.at(st_parameter));
		if (!ir_argument) break;
		v_arguments.at(st_parameter) = ir_argument;
	}
	if (!fn_callee) return nullptr;
	return LLVMBuildCall2(ir_builder, LLVMGlobalGetValueType(fn_callee), fn_callee, v_arguments.data(), static_cast<unsigned>(v_arguments.size()), "");
}

LLVMValueRef FunctionLowering::lowerPrintln (const cloth::CallExpression &call) {
	if (call.suppliedArguments.size() == 1) {
		const cloth::Expression &e_argument = *call.suppliedArguments.front().expression;
		const cloth::BodyFacts  &bf_facts   = mf_functions.program;
		if (!bf_facts.expressions.at(&e_argument).fallsThrough) return lowerExpression(e_argument);

		const cloth::ProviderSelection &ps_text = bf_facts.textConversions.at(&e_argument);
		if (ps_text.kind == cloth::ProviderKind::Format && !ps_text.callsFormatter && ps_text.receiver == cloth::ResolvedType(cloth::kInteger)) {
			LLVMValueRef       ir_argument = lowerExpression(e_argument);
			const LLVMValueRef fn_println  = detail::declarePrintlnInt32(mf_functions.module);
			return LLVMBuildCall2(ir_builder, LLVMGlobalGetValueType(fn_println), fn_println, &ir_argument, 1, "");
		}
	}
	throw cde::ClothDiagnostic(Code::UNSUPPORTED_LOWERING,
	                           std::format("LLVM lowering supports '{}' with one signed 32-bit {} argument", call.name.get_lexeme(),
	                                       cloth::to_string(cloth::kInteger)),
	                           { { call.get_span(), "unsupported output call" } });
}

LLVMValueRef FunctionLowering::lowerExit (const cloth::CallExpression &call, const cloth::BuiltinCall &builtin) {
	LLVMValueRef ir_status = lowerAs(*call.suppliedArguments.front().expression, builtin.parameters.front());
	if (!ir_status) return nullptr;
	if (call.suppliedArguments.size() > 1) {
		const cloth::Expression &e_message = *call.suppliedArguments[1].expression;
		if (!mf_functions.program.expressions.at(&e_message).fallsThrough) return lowerExpression(e_message);
		throw cde::ClothDiagnostic(Code::UNSUPPORTED_LOWERING,
		                           std::format("LLVM lowering does not support the message argument to '{}'", call.name.get_lexeme()),
		                           { { e_message.get_span(), "unsupported message argument" } });
	}
	const LLVMValueRef fn_exit = detail::declareExit(mf_functions.module);
	LLVMBuildCall2(ir_builder, LLVMGlobalGetValueType(fn_exit), fn_exit, &ir_status, 1, "");
	LLVMBuildUnreachable(ir_builder);
	return nullptr;
}

LLVMValueRef FunctionLowering::storeInEntrySlot (LLVMValueRef value) {
	const LLVMValueRef ir_slot = detail::allocateEntrySlot(ir_builder, LLVMTypeOf(value));
	LLVMBuildStore(ir_builder, value, ir_slot);
	return ir_slot;
}

const LoweredValue &FunctionLowering::bindLocal (const cloth::Token &name, const cloth::ResolvedType &type, LLVMValueRef value, bool isMutable) {
	const ValueStorage vs_storage = isMutable ? ValueStorage::MutableSlot : type.array() ? ValueStorage::ReadOnlySlot : ValueStorage::Direct;
	if (vs_storage != ValueStorage::Direct) value = storeInEntrySlot(value);
	return m_locals.emplace(&name, LoweredValue{ value, type, vs_storage }).first->second;
}

void FunctionLowering::lowerBinding (const cloth::BindingDeclaration &binding) {
	const cloth::BodyFacts &bf_facts = mf_functions.program;
	if (!bf_facts.controlFlow.at(&binding).fallsThrough) {
		// An inferred local has no type when the initializer leaves before producing a value.
		lowerExpression(*binding.initializer);
		return;
	}
	const auto it_type = bf_facts.localTypes.find(&binding.name);
	if (it_type == bf_facts.localTypes.end() || !mf_functions.valueTypes.localType(it_type->second)) {
		throw cde::ClothDiagnostic(Code::UNSUPPORTED_LOWERING, "LLVM lowering requires a supported storage type for this local",
		                           { { binding.type ? binding.type->get_span() : binding.initializer->get_span(), "unsupported local type" } });
	}
	if (const LLVMValueRef ir_value = lowerAs(*binding.initializer, it_type->second)) {
		bindLocal(binding.name, it_type->second, ir_value, binding.isMutable);
	}
}

const LoweredValue &FunctionLowering::mutableTarget (const cloth::Statement &write, const cloth::Token &name) const {
	const cloth::BodyFacts &bf_facts  = mf_functions.program;
	const auto              it_target = bf_facts.references.find(&name);
	const LoweredValue     *lv_target = it_target == bf_facts.references.end() ? nullptr : findLocal(it_target->second);
	if (!lv_target || lv_target->storage != ValueStorage::MutableSlot || bf_facts.writes.contains(&write)) {
		throw cde::ClothDiagnostic(Code::UNSUPPORTED_LOWERING, "LLVM lowering requires a built-in update on a mutable local",
		                           { { name.get_span(), "unsupported assignment target" } });
	}
	return *lv_target;
}

void FunctionLowering::lowerAssignment (const cloth::AssignmentStatement &assignment) {
	// The checker records a compound assignment's read and arithmetic on its synthetic binary expression.
	storeExpression(mutableTarget(assignment, assignment.name), *assignment.value);
}

void FunctionLowering::lowerElementAssignment (const cloth::ElementAssignmentStatement &write) {
	const cloth::IndexExpression &ie_target = *write.target;
	if (!mf_functions.program.expressions.at(ie_target.array.get()).fallsThrough) {
		lowerExpression(*ie_target.array);
		return;
	}
	const LoweredValue *lv_array = namedLocal(*ie_target.array);
	if (!lv_array || lv_array->storage != ValueStorage::MutableSlot || !lv_array->type.array())
		throw cde::ClothDiagnostic(Code::UNSUPPORTED_LOWERING, "LLVM lowering requires a mutable fixed-size array local for this assignment",
		                           { { ie_target.get_span(), "unsupported array assignment target" } });
	const LLVMTypeRef  ty_array  = LLVMGetAllocatedType(lv_array->operand);
	const LLVMValueRef ir_offset = lowerArrayOffset(*ie_target.index, ty_array);
	if (!ir_offset) return;
	const LLVMValueRef        ir_address = arrayElementAddress(ty_array, lv_array->operand, ir_offset);
	const cloth::ResolvedType rt_element = lv_array->type.element();
	const LoweredValue        lv_element{ ir_address, rt_element, ValueStorage::MutableSlot };
	if (write.compound) return storeCompound(lv_element, *write.compound);
	if (write.value) return storeExpression(lv_element, *write.value);
	lowerUpdate(lv_element, ie_target, write.op.get_operator());
}

void FunctionLowering::lowerFieldWrite (const cloth::FieldWriteStatement &write) {
	const auto *fa_field = dynamic_cast<const cloth::FieldAccessExpression *>(write.target.get());
	if (fa_field && !mf_functions.program.writes.contains(&write)) {
		const std::optional<LoweredValue> opt_field = lowerField(*fa_field);
		if (!opt_field) return;
		if (opt_field->storage == ValueStorage::MutableSlot) {
			if (write.compound) return storeCompound(*opt_field, *write.compound);
			if (write.value) return storeExpression(*opt_field, *write.value);
			return lowerUpdate(*opt_field, *write.target, write.op.get_operator());
		}
	}
	throw cde::ClothDiagnostic(Code::UNSUPPORTED_LOWERING, "LLVM lowering requires a built-in write to a mutable local struct field",
	                           { { write.target->get_span(), "unsupported field assignment target" } });
}

void FunctionLowering::storeExpression (const LoweredValue &target, const cloth::Expression &value) {
	if (const LLVMValueRef ir_value = lowerAs(value, target.type)) LLVMBuildStore(ir_builder, ir_value, target.operand);
}

void FunctionLowering::storeCompound (const LoweredValue &target, const cloth::BinaryExpression &compound) {
	// Capture the element or field before the right operand can change the target.
	const cloth::ExpressionFacts &ef_facts = mf_functions.program.expressions;
	const LLVMValueRef            ir_old   = readValue(target, ef_facts.at(compound.left.get()).types.front());
	if (const LLVMValueRef ir_new = lowerBinary(compound, ir_old))
		LLVMBuildStore(ir_builder, convertValue(ir_new, ef_facts.at(&compound).types.front(), target.type), target.operand);
}

void FunctionLowering::lowerUpdate (const LoweredValue &target, const cloth::Expression &read, cloth::Operator op) {
	const cloth::ResolvedType &rt_read  = mf_functions.program.expressions.at(&read).types.front();
	const LLVMValueRef         ir_old   = readValue(target, rt_read);
	const LLVMTypeRef          ty_value = LLVMTypeOf(ir_old);
	const LLVMValueRef         ir_one   = rt_read.floating() ? LLVMConstReal(ty_value, 1.) : LLVMConstInt(ty_value, 1, false);
	const cloth::Operator      o_op     = op == cloth::oPlusPlus ? cloth::oPlus : cloth::oMinus;
	const LLVMValueRef         ir_new   = sl_scalars.binary(o_op, ir_old, ir_one, rt_read);
	LLVMBuildStore(ir_builder, convertValue(ir_new, rt_read, target.type), target.operand);
}

void FunctionLowering::lowerReturn (const cloth::Expression *expression) {
	const LLVMValueRef ir_value = !expression                 ? nullptr :
	                              di_function.returns.empty() ? lowerExpression(*expression) :
	                                                            lowerAs(*expression, di_function.returns.front());
	if (expression && !mf_functions.program.expressions.at(expression).fallsThrough) return;
	if (ir_value && LLVMGetTypeKind(LLVMTypeOf(ir_value)) != LLVMVoidTypeKind) {
		LLVMBuildRet(ir_builder, ir_value);
	} else {
		LLVMBuildRetVoid(ir_builder);
	}
}

LLVMValueRef FunctionLowering::lowerIf (const cloth::IfExpression &conditional, const cloth::ExpressionInfo &info) {
	const cloth::ResolvedType *rt_result = info.resultKind() == cloth::ResultKind::Single ? &info.types.front() : nullptr;
	const LLVMTypeRef          ty_result = rt_result ? mf_functions.valueTypes.localType(*rt_result) : nullptr;
	if (info.resultKind() == cloth::ResultKind::Multiple || (rt_result && !ty_result)) {
		throw cde::ClothDiagnostic(Code::UNSUPPORTED_LOWERING, "LLVM lowering requires at most one supported value from a conditional",
		                           { { conditional.get_span(), "unsupported conditional result" } });
	}
	const LLVMBasicBlockRef bb_start = LLVMGetInsertBlock(ir_builder);
	const LLVMBasicBlockRef bb_end   = info.fallsThrough ? appendBlock("if.end") : nullptr;
	LLVMValueRef            ir_phi   = nullptr;
	if (ty_result) {
		LLVMPositionBuilderAtEnd(ir_builder, bb_end);
		ir_phi = LLVMBuildPhi(ir_builder, ty_result, "");
		LLVMPositionBuilderAtEnd(ir_builder, bb_start);
	}
	const auto lowerArm = [&] (const std::vector<cloth::StatementPtr> &statements) {
		LLVMValueRef ir_value = lowerStatements(statements, rt_result);
		// A nested conditional or trapping operation can move the end of this arm.
		LLVMBasicBlockRef bb_exit = LLVMGetInsertBlock(ir_builder);
		if (LLVMGetBasicBlockTerminator(bb_exit)) return;
		branchOrUnreachable(bb_end);
		if (ir_phi) LLVMAddIncoming(ir_phi, &ir_value, &bb_exit, 1);
	};
	for (const cloth::ConditionalBranch &cb_branch : conditional.branches) {
		const LLVMValueRef ir_condition = lowerExpression(*cb_branch.condition);
		if (!ir_condition) break;
		const LLVMBasicBlockRef bb_then = appendBlock("if.then");
		const LLVMBasicBlockRef bb_else = appendBlock("if.else");
		LLVMBuildCondBr(ir_builder, sl_scalars.predicate(ir_condition), bb_then, bb_else);
		LLVMPositionBuilderAtEnd(ir_builder, bb_then);
		lowerArm(cb_branch.statements);
		LLVMPositionBuilderAtEnd(ir_builder, bb_else);
	}
	if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ir_builder))) {
		if (conditional.otherwise)
			lowerArm(*conditional.otherwise);
		else
			branchOrUnreachable(bb_end);
	}
	if (bb_end) LLVMPositionBuilderAtEnd(ir_builder, bb_end);
	return ir_phi;
}

LLVMBasicBlockRef FunctionLowering::appendBlock (const char *name) const {
	const LLVMContextRef ctx_context = LLVMGetModuleContext(mf_functions.module);
	const LLVMValueRef   fn_function = LLVMGetBasicBlockParent(LLVMGetInsertBlock(ir_builder));
	return LLVMAppendBasicBlockInContext(ctx_context, fn_function, name);
}

LLVMBasicBlockRef FunctionLowering::branchToNewBlock (const char *name) {
	const LLVMBasicBlockRef bb_next = appendBlock(name);
	LLVMBuildBr(ir_builder, bb_next);
	LLVMPositionBuilderAtEnd(ir_builder, bb_next);
	return bb_next;
}

LLVMBasicBlockRef FunctionLowering::loopExit (const cloth::LoopStatement &loop) const {
	return mf_functions.program.controlFlow.at(&loop).fallsThrough ? appendBlock("loop.end") : nullptr;
}

void FunctionLowering::branchOnLoopCondition (LLVMValueRef predicate, LLVMBasicBlockRef body, LLVMBasicBlockRef exit) {
	if (exit)
		LLVMBuildCondBr(ir_builder, predicate, body, exit);
	else
		LLVMBuildBr(ir_builder, body);
}

void FunctionLowering::branchOrUnreachable (LLVMBasicBlockRef target) {
	// A statically dead arm can contain a jump whose destination the checker ruled out.
	if (target)
		LLVMBuildBr(ir_builder, target);
	else
		LLVMBuildUnreachable(ir_builder);
}

void FunctionLowering::lowerLoopBody (const cloth::LoopStatement &loop, const cloth::LoopBody &body, LoopBlocks blocks) {
	const cloth::LoopTarget *lt_target = loop.get_loop_target().get();
	m_loopBlocks.emplace(lt_target, blocks);
	lowerStatements(body.statements);
	m_loopBlocks.erase(lt_target);
	if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ir_builder))) branchOrUnreachable(blocks.continuation);
}

void FunctionLowering::lowerWhile (const cloth::WhileStatement &loop) {
	const LLVMBasicBlockRef bb_test      = branchToNewBlock("while.cond");
	const LLVMValueRef      ir_condition = lowerExpression(*loop.condition);
	if (!ir_condition) return;

	const LLVMBasicBlockRef bb_body = appendBlock("while.body");
	const LoopBlocks        lb_blocks{ bb_test, loopExit(loop) };
	branchOnLoopCondition(sl_scalars.predicate(ir_condition), bb_body, lb_blocks.exit);
	LLVMPositionBuilderAtEnd(ir_builder, bb_body);
	lowerLoopBody(loop, loop.body, lb_blocks);
	if (lb_blocks.exit) LLVMPositionBuilderAtEnd(ir_builder, lb_blocks.exit);
}

LLVMBasicBlockRef FunctionLowering::loopContinuation (const cloth::LoopStatement &loop, const cloth::LoopBody &body, const char *name) const {
	const cloth::LoopTarget *lt_target = loop.get_loop_target().get();
	for (const cloth::StatementPtr &sp_statement : body.statements) {
		const cloth::ControlFlow &cf_flow = mf_functions.program.controlFlow.at(sp_statement.get());
		if (cf_flow.continues.contains(lt_target)) break;
		if (!cf_flow.fallsThrough) return nullptr;
	}
	return appendBlock(name);
}

void FunctionLowering::lowerDoWhile (const cloth::DoWhileStatement &loop) {
	const LLVMBasicBlockRef bb_body = branchToNewBlock("do.body");
	const LoopBlocks        lb_blocks{ loopContinuation(loop, loop.body, "do.cond"), loopExit(loop) };
	lowerLoopBody(loop, loop.body, lb_blocks);
	if (lb_blocks.continuation) {
		LLVMPositionBuilderAtEnd(ir_builder, lb_blocks.continuation);
		if (const LLVMValueRef ir_condition = lowerExpression(*loop.condition))
			branchOnLoopCondition(sl_scalars.predicate(ir_condition), bb_body, lb_blocks.exit);
	}
	if (lb_blocks.exit) LLVMPositionBuilderAtEnd(ir_builder, lb_blocks.exit);
}

void FunctionLowering::lowerFor (const cloth::ForStatement &loop) {
	const cloth::BodyFacts      &bf_facts  = mf_functions.program;
	const cloth::Expression     &e_source  = loop.source.get_expression();
	const cloth::ExpressionInfo &ei_source = bf_facts.expressions.at(&e_source);
	if (!ei_source.fallsThrough) {
		// An exit from the source precedes the binding and any jumps belonging to this loop.
		lowerExpression(e_source);
		return;
	}
	const cloth::Iteration &it_loop = bf_facts.iterations.at(&loop);
	if (it_loop.kind == cloth::IterationKind::Range) {
		if (!it_loop.element.integer() || bf_facts.localTypes.at(&loop.binding.name) != it_loop.element ||
		    !mf_functions.valueTypes.localType(ei_source.types.front()))
			throw cde::ClothDiagnostic(Code::UNSUPPORTED_LOWERING, "LLVM range iteration requires a built-in integer range",
			                           { { loop.source.get_span(), "unsupported range loop" } });
		return lowerIntegerRange(loop, lowerRange(e_source, it_loop.element), it_loop.element);
	}
	const LLVMTypeRef ty_array = it_loop.kind == cloth::IterationKind::Array && !loop.stepper ? mf_functions.valueTypes.localType(ei_source.types.front()) :
	                                                                                            nullptr;
	if (!ty_array)
		throw cde::ClothDiagnostic(Code::UNSUPPORTED_LOWERING,
		                           "LLVM iteration requires a one-dimensional fixed-size array with supported element storage",
		                           { { loop.source.get_span(), "unsupported iteration source" } });
	const std::uint64_t u64_length = LLVMGetArrayLength2(ty_array);
	if (!u64_length) {
		lowerExpression(e_source);
		return;
	}
	const cloth::ResolvedType &rt_binding = bf_facts.localTypes.at(&loop.binding.name);
	if (!mf_functions.valueTypes.get(rt_binding))
		throw cde::ClothDiagnostic(Code::UNSUPPORTED_LOWERING, "LLVM lowering requires a supported storage type for this loop binding",
		                           { { loop.binding.get_span(), "unsupported loop binding type" } });

	// Capture the input before the body can modify it. A read-only local already holds an independent buffer.
	const LLVMValueRef      ir_buffer = lowerArrayBuffer(e_source, true);
	LLVMBasicBlockRef       bb_start  = LLVMGetInsertBlock(ir_builder);
	const LLVMBasicBlockRef bb_body   = branchToNewBlock("for.body");
	LoopBlocks              lb_blocks{ loopContinuation(loop, loop.body, "for.next"), loopExit(loop) };
	const LLVMTypeRef       ty_index = sl_scalars.type(cloth::ResolvedType(cloth::kUsize));
	const LLVMValueRef      ir_index = LLVMBuildPhi(ir_builder, ty_index, "");
	LLVMValueRef            ir_zero  = LLVMConstNull(ty_index);
	LLVMAddIncoming(ir_index, &ir_zero, &bb_start, 1);
	const LLVMValueRef ir_element = LLVMBuildLoad2(ir_builder, LLVMGetElementType(ty_array), arrayElementAddress(ty_array, ir_buffer, ir_index), "");
	bindLocal(loop.binding.name, rt_binding, convertValue(ir_element, it_loop.element, rt_binding), true);
	lowerLoopBody(loop, loop.body, lb_blocks);
	if (lb_blocks.continuation) {
		LLVMPositionBuilderAtEnd(ir_builder, lb_blocks.continuation);
		LLVMValueRef ir_next = LLVMBuildAdd(ir_builder, ir_index, LLVMConstInt(ty_index, 1, false), "");
		LLVMAddIncoming(ir_index, &ir_next, &lb_blocks.continuation, 1);
		const LLVMValueRef ir_more = LLVMBuildICmp(ir_builder, LLVMIntULT, ir_next, LLVMConstInt(ty_index, u64_length, false), "");
		branchOnLoopCondition(ir_more, bb_body, lb_blocks.exit);
	}
	if (lb_blocks.exit) LLVMPositionBuilderAtEnd(ir_builder, lb_blocks.exit);
}

IntegerRange FunctionLowering::lowerRange (const cloth::Expression &expression, const cloth::ResolvedType &type) {
	const cloth::ExpressionInfo &ei_range = mf_functions.program.expressions.at(&expression);
	// An inline range supplies the endpoints directly, including for integers wider than a machine word.
	if (const auto *be_range = dynamic_cast<const cloth::BinaryExpression *>(&expression);
	    be_range && ei_range.range == cloth::RangeOperation::Construct && isBuiltinOperation(ei_range))
		return lowerRangeBounds(*be_range, type);
	return IntegerRange::unpack(ir_builder, lowerExpression(expression));
}

IntegerRange FunctionLowering::lowerRangeBounds (const cloth::BinaryExpression &range, const cloth::ResolvedType &type) {
	const LLVMValueRef ir_start = lowerAs(*range.left, type);
	const LLVMValueRef ir_end   = lowerAs(*range.right, type);
	const LLVMTypeRef  ty_flag  = LLVMInt1TypeInContext(LLVMGetModuleContext(mf_functions.module));
	return { ir_start, ir_end, LLVMConstInt(ty_flag, range.op.get_operator() == cloth::oRangeInclusive, false) };
}

void FunctionLowering::lowerIntegerRange (const cloth::ForStatement &loop, IntegerRange range, const cloth::ResolvedType &type) {
	const cloth::Operator          o_step    = loop.stepper ? loop.stepper->op.get_operator() : cloth::oPlusPlus;
	const cloth::BinaryExpression *be_update = loop.stepper ? loop.stepper->compound.get() : nullptr;
	if (loop.stepper) {
		bool b_supported = o_step == cloth::oAssign || o_step == cloth::oPlusPlus || o_step == cloth::oMinusMinus;
		if (be_update) {
			const cloth::ExpressionInfo &ei_update = mf_functions.program.expressions.at(be_update);
			b_supported                            = isBuiltinOperation(ei_update) &&
			              (!ei_update.fallsThrough || (ei_update.resultKind() == cloth::ResultKind::Single && ei_update.types.front() == type));
		}
		if (!b_supported || mf_functions.program.references.at(&loop.stepper->target) != &loop.binding.name)
			throw cde::ClothDiagnostic(Code::UNSUPPORTED_LOWERING,
			                           "LLVM integer range steps support assignment, postfix updates and built-in compound assignments",
			                           { { loop.stepper->span, "unsupported range step" } });
	}

	LLVMValueRef ir_lower = range.start, ir_upper = range.end;
	if (loop.stepper) {
		// An explicit step can traverse bounds written in either order. The written end controls exclusion.
		const LLVMValueRef ir_ascending = sl_scalars.compare(cloth::oLessEqual, range.start, range.end, type);
		ir_lower                        = LLVMBuildSelect(ir_builder, ir_ascending, range.start, range.end, "");
		ir_upper                        = LLVMBuildSelect(ir_builder, ir_ascending, range.end, range.start, "");
	}
	const LLVMValueRef ir_nonempty = sl_scalars.withinRangeEnd(ir_lower, ir_upper, range.inclusive, type);
	if (LLVMIsNull(ir_nonempty)) return;

	LLVMBasicBlockRef       bb_entry = LLVMGetInsertBlock(ir_builder);
	const LLVMBasicBlockRef bb_body  = appendBlock("range.body");
	LoopBlocks              lb_blocks{ loopContinuation(loop, loop.body, "range.next"), loopExit(loop) };
	branchOnLoopCondition(ir_nonempty, bb_body, lb_blocks.exit);
	LLVMPositionBuilderAtEnd(ir_builder, bb_body);
	const LLVMValueRef ir_counter = LLVMBuildPhi(ir_builder, LLVMTypeOf(range.start), "");
	LLVMAddIncoming(ir_counter, &range.start, &bb_entry, 1);
	const LoweredValue &lv_counter = bindLocal(loop.binding.name, type, ir_counter, true);
	lowerLoopBody(loop, loop.body, lb_blocks);
	if (lb_blocks.continuation) {
		LLVMPositionBuilderAtEnd(ir_builder, lb_blocks.continuation);
		// Capture the counter left by the body before the right operand can change it.
		const LLVMValueRef       ir_current = readValue(lv_counter, type);
		const cloth::Expression *e_value    = loop.stepper ? loop.stepper->value.get() : nullptr;
		const LLVMValueRef       ir_amount  = e_value ? lowerAs(*e_value, type) : LLVMConstInt(LLVMTypeOf(ir_current), 1, false);
		if (ir_amount) {
			const cloth::Operator o_operation = be_update ? be_update->op.get_operator() : o_step;
			auto [ir_next, ir_fits]           = sl_scalars.integerRangeStep(o_operation, ir_current, ir_amount, type);
			const LLVMValueRef ir_lowerTest   = sl_scalars.compare(cloth::oLessEqual, ir_lower, ir_next, type);
			const LLVMValueRef ir_upperTest   = loop.stepper ? sl_scalars.compare(cloth::oLessEqual, ir_next, ir_upper, type) :
			                                                   sl_scalars.withinRangeEnd(ir_next, ir_upper, range.inclusive, type);
			LLVMValueRef       ir_inRange     = LLVMBuildAnd(ir_builder, ir_lowerTest, ir_upperTest, "");
			if (loop.stepper) {
				const LLVMValueRef ir_notEnd = sl_scalars.compare(cloth::oNotEqual, ir_next, range.end, type);
				ir_inRange = LLVMBuildAnd(ir_builder, ir_inRange, LLVMBuildOr(ir_builder, range.inclusive, ir_notEnd, ""), "");
			}
			const LLVMValueRef ir_repeat = LLVMBuildAnd(ir_builder, ir_fits, ir_inRange, "");
			if (e_value) {
				const bool         b_additive   = o_operation == cloth::oPlus || o_operation == cloth::oMinus;
				const LLVMValueRef ir_unchanged = b_additive ? LLVMBuildIsNull(ir_builder, ir_amount, "") :
				                                               sl_scalars.compare(cloth::oEqual, ir_current, ir_next, type);
				sl_scalars.guardRangeProgress(ir_unchanged, ir_repeat);
			}
			LLVMBasicBlockRef bb_backedge = LLVMGetInsertBlock(ir_builder);
			LLVMAddIncoming(ir_counter, &ir_next, &bb_backedge, 1);
			branchOnLoopCondition(ir_repeat, bb_body, lb_blocks.exit);
		}
	}
	if (lb_blocks.exit) LLVMPositionBuilderAtEnd(ir_builder, lb_blocks.exit);
}

LLVMValueRef FunctionLowering::lowerStatements (const std::vector<cloth::StatementPtr> &statements, const cloth::ResolvedType *resultType) {
	bool b_fallsThrough = true;
	for (const cloth::StatementPtr &sp_statement : statements) {
		if (!b_fallsThrough) {
			throw cde::ClothDiagnostic(Code::UNSUPPORTED_LOWERING, "LLVM lowering found an unreachable statement",
			                           { { sp_statement->get_span(), "unreachable statement" } });
		}
		b_fallsThrough = mf_functions.program.controlFlow.at(sp_statement.get()).fallsThrough;
		if (resultType && b_fallsThrough && &sp_statement == &statements.back()) {
			// The checker requires a trailing expression in each continuing value arm.
			const auto &es_tail = static_cast<const cloth::ExpressionStatement &>(*sp_statement);
			return lowerAs(*es_tail.expression, *resultType);
		}
		lowerStatement(*sp_statement);
	}
	return nullptr;
}

void FunctionLowering::lowerStatement (const cloth::Statement &statement) {
	if (const auto *bd_binding = dynamic_cast<const cloth::BindingDeclaration *>(&statement)) return lowerBinding(*bd_binding);
	if (const auto *as_write = dynamic_cast<const cloth::AssignmentStatement *>(&statement)) return lowerAssignment(*as_write);
	if (const auto *ea_write = dynamic_cast<const cloth::ElementAssignmentStatement *>(&statement);
	    ea_write && !mf_functions.program.writes.contains(ea_write))
		return lowerElementAssignment(*ea_write);
	if (const auto *fw_write = dynamic_cast<const cloth::FieldWriteStatement *>(&statement)) return lowerFieldWrite(*fw_write);
	if (const auto *us_update = dynamic_cast<const cloth::UpdateStatement *>(&statement))
		return lowerUpdate(mutableTarget(*us_update, us_update->name), *us_update->target, us_update->op.get_operator());
	if (const auto *rs_return = dynamic_cast<const cloth::ReturnStatement *>(&statement)) return lowerReturn(rs_return->value.get());
	if (const auto *ws_loop = dynamic_cast<const cloth::WhileStatement *>(&statement)) return lowerWhile(*ws_loop);
	if (const auto *ds_loop = dynamic_cast<const cloth::DoWhileStatement *>(&statement)) return lowerDoWhile(*ds_loop);
	if (const auto *fs_loop = dynamic_cast<const cloth::ForStatement *>(&statement)) return lowerFor(*fs_loop);
	if (const auto *lj_jump = dynamic_cast<const cloth::LoopJumpStatement *>(&statement)) {
		const LoopBlocks &lb_blocks = m_loopBlocks.at(mf_functions.program.jumpTargets.at(lj_jump));
		return branchOrUnreachable(lj_jump->isContinue() ? lb_blocks.continuation : lb_blocks.exit);
	}
	if (const auto *es_statement = dynamic_cast<const cloth::ExpressionStatement *>(&statement)) {
		lowerExpression(*es_statement->expression);
		return;
	}
	throw cde::ClothDiagnostic(Code::UNSUPPORTED_LOWERING, "LLVM lowering does not support this statement",
	                           { { statement.get_span(), "unsupported statement" } });
}

void FunctionLowering::lowerFunction (const cloth::CallableSignature &signature, LLVMValueRef function) {
	for (std::size_t st_index = 0; st_index < signature.parameters.size(); ++st_index) {
		const cloth::ParameterDeclaration &pd_parameter = signature.parameters[st_index];
		bindLocal(pd_parameter.name, di_function.parameters.at(st_index), LLVMGetParam(function, static_cast<unsigned>(st_index)),
		          pd_parameter.isMutable());
	}
	const cloth::FunctionBody &fb_body = *signature.executableBody();
	if (const cloth::ExpressionBody *eb_body = std::get_if<cloth::ExpressionBody>(&fb_body.value)) {
		return lowerReturn(eb_body->expression.get());
	}
	if (const cloth::BlockBody *bb_body = std::get_if<cloth::BlockBody>(&fb_body.value)) {
		lowerStatements(bb_body->statements);
		if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ir_builder))) return;
		if (LLVMGetTypeKind(LLVMGetReturnType(LLVMGlobalGetValueType(function))) == LLVMVoidTypeKind) {
			LLVMBuildRetVoid(ir_builder);
			return;
		}
	}
	throw cde::ClothDiagnostic(Code::UNSUPPORTED_LOWERING, "LLVM lowering could not emit a return for this function body",
	                           { { .span = fb_body.get_span(), .text = "unsupported function body" } });
}

void lowerEntryPoint (const cloth::CheckedProgram &program, LLVMModuleRef module) {
	if (!program.entryPoint) {
		throw cde::ClothDiagnostic(Code::INVALID_COMPILATION_REQUEST, "LLVM entry-point lowering requires a checked executable");
	}
	const LLVMContextRef ctx_context = LLVMGetModuleContext(module);
	ModuleFunctions      mf_functions{ program, module };
	mf_functions.declareFunction(*program.entryPoint);
	const std::unique_ptr<LLVMOpaqueBuilder, decltype(&LLVMDisposeBuilder)> ir_builder(LLVMCreateBuilderInContext(ctx_context), LLVMDisposeBuilder);
	// Lowering a call can append a new declaration. Each symbol is queued once, including cycles.
	for (std::size_t st_index = 0; st_index < mf_functions.pending.size(); ++st_index) {
		const cloth::Symbol &sym_function = *mf_functions.pending[st_index];
		const LLVMValueRef   fn_function  = mf_functions.declarations.at(&sym_function);
		LLVMPositionBuilderAtEnd(ir_builder.get(), LLVMAppendBasicBlockInContext(ctx_context, fn_function, "entry"));
		FunctionLowering fl_function(mf_functions, ir_builder.get(), program.declarations.at(&sym_function));
		fl_function.lowerFunction(*std::get<const cloth::CallableSignature *>(sym_function.declaration), fn_function);
	}
}

void checkLLVMResult (LLVMBool failed, char *message, Code code, const char *operation) {
	const Message msg_detail(message, LLVMDisposeMessage);
	if (failed) {
		throw cde::ClothDiagnostic(code, msg_detail && *msg_detail ? std::format("{}\n{}", operation, msg_detail.get()) : std::string(operation));
	}
}

std::string toolchainTriple (unsigned sizeBits) {
	std::string s_triple(CLOTH_TOOLCHAIN_TRIPLE);
#if defined(__i386__) || defined(__x86_64__)
	// Only replace the architecture when switching widths. The vendor, OS, and ABI come from Cloth's toolchain.
	const unsigned u_archBits = s_triple.starts_with("x86_64") || s_triple.starts_with("amd64") ? 64u : 32u;
	if (sizeBits != u_archBits) s_triple.replace(0, s_triple.find('-'), sizeBits == 32 ? "i686" : "x86_64");
#else
	if (sizeBits != sizeof(void *) * CHAR_BIT) {
		throw cde::ClothDiagnostic(Code::UNSUPPORTED_TARGET, std::format("The {} toolchain has no configured {}-bit target", s_triple, sizeBits));
	}
#endif
	return s_triple;
}

TargetMachine createTargetMachine (const std::string_view targetTriple, const unsigned sizeBits) {
	static const bool b_initialized = LLVMInitializeNativeTarget() == 0 && LLVMInitializeNativeAsmPrinter() == 0;
	if (!b_initialized) throw cde::ClothDiagnostic(Code::LLVM_INITIALIZATION, "LLVM could not initialize the native target and assembly printer");

	if (targetTriple.find('\0') != std::string_view::npos) throw cde::ClothDiagnostic(Code::LLVM_TARGET, "A target triple contains a null byte");
	const std::string s_requested = targetTriple.empty() ? toolchainTriple(sizeBits) : std::string(targetTriple);
	const Message     s_triple(LLVMNormalizeTargetTriple(s_requested.c_str()), LLVMDisposeMessage);
	LLVMTargetRef     t_target = nullptr;
	char             *s_error  = nullptr;
	const LLVMBool    b_failed = LLVMGetTargetFromTriple(s_triple.get(), &t_target, &s_error);
	checkLLVMResult(b_failed, s_error, Code::LLVM_TARGET, "LLVM target lookup failed");

	// References to constant data must also link into position-independent executables.
	TargetMachine tm_machine(LLVMCreateTargetMachine(t_target, s_triple.get(), "", "", LLVMCodeGenLevelNone, LLVMRelocPIC, LLVMCodeModelDefault),
	                         LLVMDisposeTargetMachine);
	if (!tm_machine) throw cde::ClothDiagnostic(Code::LLVM_TARGET, std::format("LLVM could not create a target machine for '{}'", s_triple.get()));
	return tm_machine;
}

void configureModule (LLVMModuleRef module, LLVMTargetMachineRef machine, const cloth::TargetLayout &checkedLayout) {
	const Message    s_triple(LLVMGetTargetMachineTriple(machine), LLVMDisposeMessage);
	const TargetData td_layout(LLVMCreateTargetDataLayout(machine), LLVMDisposeTargetData);
	const unsigned   u_pointerBits = LLVMPointerSize(td_layout.get()) * CHAR_BIT;
	if (u_pointerBits != checkedLayout.sizeBits) {
		throw cde::ClothDiagnostic(Code::ABI_LAYOUT, std::format("Target '{}' uses {}-bit pointers. The program was checked for {}-bit pointers",
		                                                         s_triple.get(), u_pointerBits, checkedLayout.sizeBits));
	}
	// Check the native integer alignment classes and the stride of the widest Cloth integer.
	for (const unsigned u_width : { 8u, 16u, 32u, 64u, 128u, cloth::MAX_INTEGER_BITS }) {
		const LLVMTypeRef          ty_int = LLVMIntTypeInContext(LLVMGetModuleContext(module), u_width);
		const cloth::StorageLayout sl_actual{ LLVMABISizeOfType(td_layout.get(), ty_int), LLVMABIAlignmentOfType(td_layout.get(), ty_int) };
		if (const cloth::StorageLayout sl_expected = checkedLayout.integerStorage(u_width); sl_actual != sl_expected) {
			throw cde::ClothDiagnostic(
			        Code::ABI_LAYOUT,
			        std::format("Target '{}' gives i{} size {} and alignment {}. The checked layout requires size {} and alignment {}",
			                    s_triple.get(), u_width, sl_actual.size, sl_actual.alignment, sl_expected.size, sl_expected.alignment));
		}
	}
	LLVMSetTarget(module, s_triple.get());
	LLVMSetModuleDataLayout(module, td_layout.get());
}

}

LoweredModule::LoweredModule (const cloth::CheckedProgram &program, std::string_view targetTriple) :
        ctx_context(LLVMContextCreate(), LLVMContextDispose),
        tm_machine(createTargetMachine(targetTriple, program.layout.sizeBits)),
        mod_module(LLVMModuleCreateWithNameInContext("cloth", ctx_context.get()), LLVMDisposeModule) {
	configureModule(mod_module.get(), tm_machine.get(), program.layout);
	lowerEntryPoint(program, mod_module.get());
}

LLVMModuleRef LoweredModule::get () const {
	return mod_module.get();
}

void LoweredModule::verify () const {
	char          *s_error  = nullptr;
	const LLVMBool b_failed = LLVMVerifyModule(get(), LLVMReturnStatusAction, &s_error);
	checkLLVMResult(b_failed, s_error, Code::INVALID_IR, "LLVM module verification failed");
}

std::vector<std::byte> LoweredModule::emitObject () {
	verify();

	char               *s_error   = nullptr;
	LLVMMemoryBufferRef mb_output = nullptr;
	const LLVMBool      b_failed  = LLVMTargetMachineEmitToMemoryBuffer(tm_machine.get(), get(), LLVMObjectFile, &s_error, &mb_output);
	const MemoryBuffer  mb_object(mb_output, LLVMDisposeMemoryBuffer);
	checkLLVMResult(b_failed, s_error, Code::LLVM_OBJECT, "LLVM object emission failed");
	if (!mb_object || LLVMGetBufferSize(mb_object.get()) == 0) {
		throw cde::ClothDiagnostic(Code::LLVM_OBJECT, "LLVM returned an empty object");
	}
	const auto *p_bytes = reinterpret_cast<const std::byte *>(LLVMGetBufferStart(mb_object.get()));
	return { p_bytes, p_bytes + LLVMGetBufferSize(mb_object.get()) };
}

std::vector<std::byte> emitObject (const cloth::CheckedProgram &program, std::string_view targetTriple) {
	return LoweredModule(program, targetTriple).emitObject();
}

}
