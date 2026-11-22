// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <backend/backend.hpp>
#include <backend/enum_payloads.hpp>
#include <backend/lowered_module.hpp>
#include <backend/nullable_value.hpp>
#include <backend/runtime.hpp>
#include <backend/scalar_lowering.hpp>
#include <backend/value_types.hpp>

#include <algorithm>
#include <climits>
#include <cstddef>
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

struct LoweredValue {
	LLVMValueRef               operand;
	const cloth::ResolvedType &type;
	bool                       isAddress;
};

struct LoopBlocks {
	LLVMBasicBlockRef condition;
	LLVMBasicBlockRef exit;
};

class FunctionLowering {
    public:
	FunctionLowering (ModuleFunctions &functions, LLVMBuilderRef builder, const cloth::DeclarationInfo &declaration);
	void lowerFunction (const cloth::CallableSignature &signature, LLVMValueRef function);

    private:
	const LoweredValue         *findLocal (const cloth::Token *declaration) const;
	LLVMValueRef                readValue (const LoweredValue &value, const cloth::ResolvedType &view);
	void                        bindLocal (const cloth::Token &name, const cloth::ResolvedType &type, LLVMValueRef value, bool isMutable);
	std::optional<LoweredValue> lowerField (const cloth::FieldAccessExpression &field);

	LLVMValueRef      lowerStatements (const std::vector<cloth::StatementPtr> &statements, const cloth::ResolvedType *resultType = nullptr);
	void              lowerStatement (const cloth::Statement &statement);
	void              lowerReturn (const cloth::Expression *expression);
	LLVMValueRef      lowerIf (const cloth::IfExpression &conditional, const cloth::ExpressionInfo &info);
	void              lowerWhile (const cloth::WhileStatement &loop);
	void              lowerDoWhile (const cloth::DoWhileStatement &loop);
	void              lowerLoopBody (const cloth::LoopStatement &loop, const cloth::LoopBody &body, LoopBlocks blocks);
	LLVMBasicBlockRef appendBlock (const char *name) const;
	LLVMBasicBlockRef branchToNewBlock (const char *name);
	LLVMBasicBlockRef loopExit (const cloth::LoopStatement &loop) const;
	void              branchOnLoopCondition (LLVMValueRef condition, LLVMBasicBlockRef body, LLVMBasicBlockRef exit);
	void              branchOrUnreachable (LLVMBasicBlockRef target);

	LLVMValueRef lowerExpression (const cloth::Expression &expression);
	LLVMValueRef lowerStructLiteral (const cloth::StructLiteral &literal, const cloth::ExpressionInfo &info);
	LLVMValueRef lowerAs (const cloth::Expression &expression, const cloth::ResolvedType &destination);
	LLVMValueRef convertValue (LLVMValueRef value, const cloth::ResolvedType &source, const cloth::ResolvedType &destination,
	                           const cloth::ExpressionInfo *cast = nullptr);
	LLVMValueRef lowerBinary (const cloth::BinaryExpression &binary);
	LLVMValueRef lowerNullableEquality (const cloth::BinaryExpression &binary, const cloth::ResolvedType &operandType);
	LLVMValueRef lowerNullableFallback (const cloth::ElvisExpression &expression, const cloth::ResolvedType &result);
	LLVMValueRef lowerLogical (const cloth::BinaryExpression &binary);
	LLVMValueRef lowerCall (const cloth::CallExpression &call);
	LLVMValueRef lowerPrintln (const cloth::CallExpression &call);
	LLVMValueRef lowerExit (const cloth::CallExpression &call, const cloth::BuiltinCall &builtin);

	void                lowerBinding (const cloth::BindingDeclaration &binding);
	void                lowerAssignment (const cloth::AssignmentStatement &assignment);
	void                lowerFieldWrite (const cloth::FieldWriteStatement &write);
	void                storeExpression (const LoweredValue &target, const cloth::Expression &value);
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
	const decltype(m_locals)::const_iterator it_local = m_locals.find(declaration);
	return it_local == m_locals.end() ? nullptr : &it_local->second;
}

LLVMValueRef FunctionLowering::readValue (const LoweredValue &value, const cloth::ResolvedType &view) {
	const LLVMValueRef ir_value = value.isAddress ? LLVMBuildLoad2(ir_builder, mf_functions.valueTypes.get(value.type), value.operand, "") : value.operand;
	// The checked read can see the payload while the local or field still stores the whole nullable.
	return value.type.nullable() && !view.nullable() ? detail::nullable::payload(ir_builder, ir_value) : ir_value;
}

bool isBuiltinOperation (const cloth::ExpressionInfo &info) {
	return !info.symbol && !info.leftContext && !info.rightContext && !info.argumentContract && !info.dispatch;
}

LLVMValueRef FunctionLowering::lowerAs (const cloth::Expression &expression, const cloth::ResolvedType &destination) {
	const LLVMValueRef           ir_value  = lowerExpression(expression);
	const cloth::ExpressionInfo &ei_source = mf_functions.program.expressions.at(&expression);
	if (!ei_source.fallsThrough) return nullptr;
	return convertValue(ir_value, ei_source.types.front(), destination);
}

LLVMValueRef FunctionLowering::convertValue (LLVMValueRef value, const cloth::ResolvedType &source, const cloth::ResolvedType &destination,
                                             const cloth::ExpressionInfo *cast) {
	if (source == destination) return value;
	if (!destination.nullable()) return cast ? sl_scalars.checkedCast(value, source, destination, *cast) : sl_scalars.convert(value, source, destination);
	const cloth::ResolvedType rt_payload = destination.element();
	if (source == cloth::ResolvedType(cloth::kNull)) return LLVMConstNull(detail::nullable::type(sl_scalars.type(rt_payload)));
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
	if (const cloth::ElvisExpression *ee_fallback = dynamic_cast<const cloth::ElvisExpression *>(&expression);
	    ee_fallback && isBuiltinOperation(ei_value)) {
		if (!ef_expressions.at(ee_fallback->left.get()).fallsThrough) return lowerExpression(*ee_fallback->left);
		if (rt_result && sl_scalars.type(rt_result->nullable() ? rt_result->element() : *rt_result))
			return lowerNullableFallback(*ee_fallback, *rt_result);
		if (!ei_value.fallsThrough && dynamic_cast<const cloth::NullLiteral *>(ee_fallback->left.get())) return lowerExpression(*ee_fallback->fallback);
	}
	if (const cloth::StructLiteral *sl_literal = dynamic_cast<const cloth::StructLiteral *>(&expression)) return lowerStructLiteral(*sl_literal, ei_value);
	if (const cloth::FieldAccessExpression *fa_read = dynamic_cast<const cloth::FieldAccessExpression *>(&expression)) {
		if (ei_value.symbol && ei_value.symbol->kind == cloth::SymbolKind::EnumCase)
			return LLVMConstInt(sl_scalars.type(*rt_result), ei_value.symbol->enumCaseOrdinal, false);
		if (ei_value.symbol && std::holds_alternative<const cloth::EnumFieldDeclaration *>(ei_value.symbol->declaration)) {
			const LLVMValueRef ir_tag = lowerExpression(*fa_read->receiver);
			if (!ir_tag) return nullptr;
			const LLVMValueRef ir_field = mf_functions.enumPayloads.read(ir_builder, *ei_value.symbol, ir_tag, fa_read->get_span());
			return readValue({ ir_field, *mf_functions.program.declarations.at(ei_value.symbol).value, false }, *rt_result);
		}
		const std::optional<LoweredValue> opt_field = lowerField(*fa_read);
		return opt_field ? readValue(*opt_field, *rt_result) : nullptr;
	}
	if (dynamic_cast<const cloth::NullLiteral *>(&expression) && rt_result) {
		if (const LLVMTypeRef ty_null = mf_functions.valueTypes.get(*rt_result)) return LLVMConstNull(ty_null);
	}
	if (dynamic_cast<const cloth::NumberLiteral *>(&expression) || dynamic_cast<const cloth::BooleanLiteral *>(&expression)) {
		return detail::scalarConstant(sl_scalars.type(*rt_result), ei_value.numericConstant);
	}
	if (const auto *cl_literal = dynamic_cast<const cloth::CharacterLiteral *>(&expression)) {
		return LLVMConstInt(sl_scalars.type(*rt_result), cl_literal->value, false);
	}
	if (dynamic_cast<const cloth::NameExpression *>(&expression)) {
		if (const LoweredValue *lv_local = findLocal(ei_value.binding)) return readValue(*lv_local, *rt_result);
	}
	if (const auto *ce_call = dynamic_cast<const cloth::CallExpression *>(&expression)) {
		return lowerCall(*ce_call);
	}
	if (const auto *ce_cast = dynamic_cast<const cloth::CastExpression *>(&expression); ce_cast && isBuiltinOperation(ei_value)) {
		if (!ei_value.fallsThrough || ei_value.resultKind() == cloth::ResultKind::Empty) {
			lowerExpression(*ce_cast->operand);
			return nullptr;
		}
		const cloth::ResultTypes &v_source = ef_expressions.at(ce_cast->operand.get()).types;
		if (rt_result && mf_functions.valueTypes.directCallType(*rt_result) && v_source.size() == 1 &&
		    (mf_functions.valueTypes.directCallType(v_source.front()) || v_source.front() == cloth::ResolvedType(cloth::kNull)))
			return convertValue(lowerExpression(*ce_cast->operand), v_source.front(), *rt_result, &ei_value);
	}
	if (const auto *mc_call = dynamic_cast<const cloth::MemberCallExpression *>(&expression);
	    mc_call && ei_value.builtin && ei_value.builtin->operation == cloth::BuiltinOperation::ToInt) {
		if (!ei_value.fallsThrough) return lowerExpression(*mc_call->receiver);
		return lowerAs(*mc_call->receiver, *rt_result);
	}
	if (const cloth::ProviderAccessExpression *pa_provider = dynamic_cast<const cloth::ProviderAccessExpression *>(&expression)) {
		if (!ei_value.fallsThrough && isBuiltinOperation(ei_value)) return lowerExpression(*pa_provider->receiver);
		if (ei_value.provider && sl_scalars.type(ei_value.provider->receiver) && !std::holds_alternative<std::monostate>(ei_value.numericConstant)) {
			if (!ei_value.provider->typeReceiver) lowerExpression(*pa_provider->receiver);
			return detail::scalarConstant(sl_scalars.type(*rt_result), ei_value.numericConstant);
		}
	}
	if (const cloth::TypeCheckExpression *tc_test = dynamic_cast<const cloth::TypeCheckExpression *>(&expression)) {
		if (!ei_value.fallsThrough) {
			const decltype(bf_facts.valueTests)::const_iterator it_test = bf_facts.valueTests.find(tc_test);
			if (it_test != bf_facts.valueTests.end()) return lowerBinary(*it_test->second);
			return lowerExpression(*tc_test->operand);
		}
		if (ei_value.testedType && sl_scalars.type(*ei_value.testedType)) {
			const cloth::ResolvedType &rt_source = ef_expressions.at(tc_test->operand.get()).types.front();
			if (sl_scalars.type(rt_source)) {
				lowerExpression(*tc_test->operand);
				return LLVMConstInt(sl_scalars.type(*rt_result), rt_source == *ei_value.testedType, false);
			}
		}
	}
	if (const cloth::UnaryExpression *ue_unary = dynamic_cast<const cloth::UnaryExpression *>(&expression); ue_unary && isBuiltinOperation(ei_value)) {
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

LLVMValueRef FunctionLowering::lowerStructLiteral (const cloth::StructLiteral &literal, const cloth::ExpressionInfo &info) {
	const cloth::CheckedProgram &cp_program = mf_functions.program;
	if (!info.fallsThrough) {
		// A field may transfer control before the literal produces a struct value.
		for (const cloth::ExpressionPtr &field : literal.fields) {
			lowerExpression(*field);
			if (!cp_program.expressions.at(field.get()).fallsThrough) break;
		}
		return nullptr;
	}
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
	while (const cloth::FieldAccessExpression *fa_step = dynamic_cast<const cloth::FieldAccessExpression *>(e_root)) {
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

	const LoweredValue *lv_root    = dynamic_cast<const cloth::NameExpression *>(e_root) ? findLocal(cp_program.expressions.at(e_root).binding) : nullptr;
	const bool          b_address  = lv_root && lv_root->isAddress;
	LLVMValueRef        ir_operand = lv_root ? lv_root->operand : lowerExpression(*e_root);
	// A mutable root keeps the address of its subobject even when the right-hand side replaces a parent.
	for (const FieldStep &fs_step : std::views::reverse(v_path)) {
		ir_operand = b_address ? LLVMBuildStructGEP2(ir_builder, fs_step.owner, ir_operand, fs_step.index, "") :
		                         LLVMBuildExtractValue(ir_builder, ir_operand, fs_step.index, "");
	}
	return LoweredValue{ ir_operand, v_path.front().type, b_address };
}

LLVMValueRef FunctionLowering::lowerLogical (const cloth::BinaryExpression &binary) {
	const bool   b_and   = binary.op.get_operator() == cloth::oAnd;
	LLVMValueRef ir_left = lowerExpression(*binary.left);
	if (!ir_left) return nullptr;
	// When the whole expression transfers control, there is no short-circuit path past the right operand.
	if (!mf_functions.program.expressions.at(&binary).fallsThrough) return lowerExpression(*binary.right);
	LLVMBasicBlockRef       bb_left  = LLVMGetInsertBlock(ir_builder);
	LLVMBasicBlockRef       bb_right = appendBlock("logical.rhs");
	const LLVMBasicBlockRef bb_end   = appendBlock("logical.end");
	LLVMBuildCondBr(ir_builder, sl_scalars.predicate(ir_left), b_and ? bb_right : bb_end, b_and ? bb_end : bb_right);
	LLVMPositionBuilderAtEnd(ir_builder, bb_right);
	LLVMValueRef ir_right = lowerExpression(*binary.right);
	if (!ir_right) {
		LLVMPositionBuilderAtEnd(ir_builder, bb_end);
		return ir_left;
	}
	bb_right = LLVMGetInsertBlock(ir_builder);
	LLVMBuildBr(ir_builder, bb_end);
	LLVMPositionBuilderAtEnd(ir_builder, bb_end);
	const LLVMValueRef ir_result = LLVMBuildPhi(ir_builder, LLVMTypeOf(ir_left), "");
	LLVMAddIncoming(ir_result, &ir_left, &bb_left, 1);
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

LLVMValueRef FunctionLowering::lowerNullableEquality (const cloth::BinaryExpression &binary, const cloth::ResolvedType &operandType) {
	const cloth::Operator o_op       = binary.op.get_operator();
	const bool            b_leftNull = dynamic_cast<const cloth::NullLiteral *>(binary.left.get()) != nullptr;
	if (b_leftNull || dynamic_cast<const cloth::NullLiteral *>(binary.right.get())) {
		const LLVMValueRef ir_peer = lowerAs(b_leftNull ? *binary.right : *binary.left, operandType);
		const LLVMValueRef ir_flag = detail::nullable::presence(ir_builder, ir_peer);
		return o_op == cloth::oNotEqual ? ir_flag : sl_scalars.boolean(LLVMBuildIsNull(ir_builder, ir_flag, ""));
	}
	const LLVMValueRef ir_left         = lowerAs(*binary.left, operandType);
	const LLVMValueRef ir_right        = lowerAs(*binary.right, operandType);
	const LLVMValueRef ir_leftFlag     = detail::nullable::presence(ir_builder, ir_left);
	const LLVMValueRef ir_rightFlag    = detail::nullable::presence(ir_builder, ir_right);
	const LLVMValueRef ir_presenceTest = LLVMBuildICmp(ir_builder, o_op == cloth::oEqual ? LLVMIntEQ : LLVMIntNE, ir_leftFlag, ir_rightFlag, "");
	const LLVMValueRef ir_payloadTest =
	        sl_scalars.binary(o_op, detail::nullable::payload(ir_builder, ir_left), detail::nullable::payload(ir_builder, ir_right), operandType.element());
	// When either value is absent, only the presence flags participate in equality.
	const LLVMValueRef ir_bothPresent = sl_scalars.predicate(LLVMBuildAnd(ir_builder, ir_leftFlag, ir_rightFlag, ""));
	return LLVMBuildSelect(ir_builder, ir_bothPresent, ir_payloadTest, sl_scalars.boolean(ir_presenceTest), "");
}

LLVMValueRef FunctionLowering::lowerBinary (const cloth::BinaryExpression &binary) {
	const cloth::ExpressionFacts &ef_facts  = mf_functions.program.expressions;
	const cloth::ExpressionInfo  &ei_binary = ef_facts.at(&binary);
	const cloth::Operator         o_op      = binary.op.get_operator();
	if (isBuiltinOperation(ei_binary) && !ei_binary.range) {
		if (o_op == cloth::oAnd || o_op == cloth::oOr) return lowerLogical(binary);
		const cloth::ExpressionInfo &ei_left = ef_facts.at(binary.left.get());
		if (!ei_binary.fallsThrough) {
			// The checker can record an exit before it selects an operand type for the operation.
			lowerExpression(*binary.left);
			if (ei_left.fallsThrough) lowerExpression(*binary.right);
			return nullptr;
		}
		const cloth::ExpressionInfo &ei_right = ef_facts.at(binary.right.get());
		if (ei_binary.comparisonType && ei_binary.comparisonType->nullable() && (o_op == cloth::oEqual || o_op == cloth::oNotEqual) &&
		    sl_scalars.type(ei_binary.comparisonType->element()))
			return lowerNullableEquality(binary, *ei_binary.comparisonType);
		if ((!cloth::isComparison(o_op) || ei_binary.booleanEquality || ei_binary.comparisonType) &&
		    ei_binary.resultKind() == cloth::ResultKind::Single && ei_left.resultKind() == cloth::ResultKind::Single &&
		    ei_right.resultKind() == cloth::ResultKind::Single && sl_scalars.type(ei_left.types.front()) && sl_scalars.type(ei_right.types.front())) {
			// Boolean equality uses the boolean result type to normalize both inputs.
			const cloth::ResolvedType &rt_operand = ei_binary.comparisonType ? *ei_binary.comparisonType : ei_binary.types.front();
			const LLVMValueRef         ir_left    = lowerAs(*binary.left, rt_operand);
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

void FunctionLowering::bindLocal (const cloth::Token &name, const cloth::ResolvedType &type, LLVMValueRef value, bool isMutable) {
	if (isMutable) {
		// Allocate once per call and initialize each time the declaration executes.
		const LLVMBasicBlockRef bb_site  = LLVMGetInsertBlock(ir_builder);
		const LLVMBasicBlockRef bb_entry = LLVMGetEntryBasicBlock(LLVMGetBasicBlockParent(bb_site));
		if (const LLVMValueRef ir_first = LLVMGetFirstInstruction(bb_entry)) LLVMPositionBuilderBefore(ir_builder, ir_first);
		const LLVMValueRef ir_slot = LLVMBuildAlloca(ir_builder, LLVMTypeOf(value), "");
		LLVMPositionBuilderAtEnd(ir_builder, bb_site);
		LLVMBuildStore(ir_builder, value, ir_slot);
		value = ir_slot;
	}
	m_locals.emplace(&name, LoweredValue{ value, type, isMutable });
}

void FunctionLowering::lowerBinding (const cloth::BindingDeclaration &binding) {
	const cloth::BodyFacts &bf_facts = mf_functions.program;
	if (!bf_facts.controlFlow.at(&binding).fallsThrough) {
		// An inferred local has no type when the initializer leaves before producing a value.
		lowerExpression(*binding.initializer);
		return;
	}
	const decltype(bf_facts.localTypes)::const_iterator it_type = bf_facts.localTypes.find(&binding.name);
	if (it_type == bf_facts.localTypes.end() || !mf_functions.valueTypes.get(it_type->second)) {
		throw cde::ClothDiagnostic(Code::UNSUPPORTED_LOWERING, "LLVM lowering requires a supported storage type for this local",
		                           { { binding.type ? binding.type->get_span() : binding.initializer->get_span(), "unsupported local type" } });
	}
	if (const LLVMValueRef ir_value = lowerAs(*binding.initializer, it_type->second)) {
		bindLocal(binding.name, it_type->second, ir_value, binding.isMutable);
	}
}

const LoweredValue &FunctionLowering::mutableTarget (const cloth::Statement &write, const cloth::Token &name) const {
	const cloth::BodyFacts &bf_facts  = mf_functions.program;
	const decltype(bf_facts.references)::const_iterator it_target = bf_facts.references.find(&name);
	const LoweredValue                                 *lv_target = it_target == bf_facts.references.end() ? nullptr : findLocal(it_target->second);
	if (!lv_target || !lv_target->isAddress || bf_facts.writes.contains(&write)) {
		throw cde::ClothDiagnostic(Code::UNSUPPORTED_LOWERING, "LLVM lowering requires a built-in update on a mutable local",
		                           { { name.get_span(), "unsupported assignment target" } });
	}
	return *lv_target;
}

void FunctionLowering::lowerAssignment (const cloth::AssignmentStatement &assignment) {
	// The checker records a compound assignment's read and arithmetic on its synthetic binary expression.
	storeExpression(mutableTarget(assignment, assignment.name), *assignment.value);
}

void FunctionLowering::lowerFieldWrite (const cloth::FieldWriteStatement &write) {
	const cloth::FieldAccessExpression *fa_field = dynamic_cast<const cloth::FieldAccessExpression *>(write.target.get());
	if (fa_field && !mf_functions.program.writes.contains(&write)) {
		const std::optional<LoweredValue> opt_field = lowerField(*fa_field);
		if (!opt_field) return;
		if (opt_field->isAddress) {
			if (write.compound) return storeExpression(*opt_field, *write.compound);
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

void FunctionLowering::lowerUpdate (const LoweredValue &target, const cloth::Expression &read, cloth::Operator op) {
	const cloth::ResolvedType &rt_read   = mf_functions.program.expressions.at(&read).types.front();
	const LLVMValueRef         ir_old    = readValue(target, rt_read);
	const LLVMTypeRef     ty_value  = LLVMTypeOf(ir_old);
	const LLVMValueRef         ir_one    = rt_read.floating() ? LLVMConstReal(ty_value, 1.) : LLVMConstInt(ty_value, 1, false);
	const cloth::Operator      o_op      = op == cloth::oPlusPlus ? cloth::oPlus : cloth::oMinus;
	const LLVMValueRef         ir_new    = sl_scalars.binary(o_op, ir_old, ir_one, rt_read);
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
	const LLVMTypeRef          ty_result = rt_result ? mf_functions.valueTypes.get(*rt_result) : nullptr;
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

void FunctionLowering::branchOnLoopCondition (LLVMValueRef condition, LLVMBasicBlockRef body, LLVMBasicBlockRef exit) {
	if (exit)
		LLVMBuildCondBr(ir_builder, sl_scalars.predicate(condition), body, exit);
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
	if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ir_builder))) branchOrUnreachable(blocks.condition);
}

void FunctionLowering::lowerWhile (const cloth::WhileStatement &loop) {
	const LLVMBasicBlockRef bb_test      = branchToNewBlock("while.cond");
	const LLVMValueRef      ir_condition = lowerExpression(*loop.condition);
	if (!ir_condition) return;

	const LLVMBasicBlockRef bb_body = appendBlock("while.body");
	const LoopBlocks        lb_blocks{ bb_test, loopExit(loop) };
	branchOnLoopCondition(ir_condition, bb_body, lb_blocks.exit);
	LLVMPositionBuilderAtEnd(ir_builder, bb_body);
	lowerLoopBody(loop, loop.body, lb_blocks);
	if (lb_blocks.exit) LLVMPositionBuilderAtEnd(ir_builder, lb_blocks.exit);
}

bool reachesDoWhileCondition (const cloth::DoWhileStatement &loop, const cloth::BodyFacts &facts) {
	const cloth::LoopTarget *lt_target = loop.get_loop_target().get();
	for (const cloth::StatementPtr &sp_statement : loop.body.statements) {
		const cloth::ControlFlow &cf_flow = facts.controlFlow.at(sp_statement.get());
		if (cf_flow.continues.contains(lt_target)) return true;
		if (!cf_flow.fallsThrough) return false;
	}
	return true;
}

void FunctionLowering::lowerDoWhile (const cloth::DoWhileStatement &loop) {
	const LLVMBasicBlockRef bb_body = branchToNewBlock("do.body");
	const LoopBlocks        lb_blocks{ reachesDoWhileCondition(loop, mf_functions.program) ? appendBlock("do.cond") : nullptr, loopExit(loop) };
	lowerLoopBody(loop, loop.body, lb_blocks);
	if (lb_blocks.condition) {
		LLVMPositionBuilderAtEnd(ir_builder, lb_blocks.condition);
		if (const LLVMValueRef ir_condition = lowerExpression(*loop.condition)) branchOnLoopCondition(ir_condition, bb_body, lb_blocks.exit);
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
	if (const cloth::FieldWriteStatement *fw_write = dynamic_cast<const cloth::FieldWriteStatement *>(&statement)) return lowerFieldWrite(*fw_write);
	if (const cloth::UpdateStatement *us_update = dynamic_cast<const cloth::UpdateStatement *>(&statement))
		return lowerUpdate(mutableTarget(*us_update, us_update->name), *us_update->target, us_update->op.get_operator());
	if (const auto *rs_return = dynamic_cast<const cloth::ReturnStatement *>(&statement)) return lowerReturn(rs_return->value.get());
	if (const auto *ws_loop = dynamic_cast<const cloth::WhileStatement *>(&statement)) return lowerWhile(*ws_loop);
	if (const auto *ds_loop = dynamic_cast<const cloth::DoWhileStatement *>(&statement)) return lowerDoWhile(*ds_loop);
	if (const auto *lj_jump = dynamic_cast<const cloth::LoopJumpStatement *>(&statement)) {
		const LoopBlocks &lb_blocks = m_loopBlocks.at(mf_functions.program.jumpTargets.at(lj_jump));
		return branchOrUnreachable(lj_jump->isContinue() ? lb_blocks.condition : lb_blocks.exit);
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
