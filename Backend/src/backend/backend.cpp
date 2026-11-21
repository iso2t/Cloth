// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <backend/backend.hpp>
#include <backend/lowered_module.hpp>
#include <backend/scalar_lowering.hpp>

#include <algorithm>
#include <climits>
#include <cstddef>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include <ast/decl/declarations.hpp>
#include <ast/expr/expressions.hpp>
#include <ast/expr/member_call_expression.hpp>
#include <ast/expr/provider_expressions.hpp>
#include <ast/stmt/statements.hpp>
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
	                                       di_function.returns.size() == 1 ? detail::scalarType(ctx_context, program.layout, di_function.returns.front()) :
	                                                                         nullptr;
	std::vector<LLVMTypeRef> v_parameters;
	for (const cloth::ResolvedType &rt_parameter : di_function.parameters)
		v_parameters.push_back(detail::scalarType(ctx_context, program.layout, rt_parameter));
	if (!ty_result || std::ranges::find(v_parameters, nullptr) != v_parameters.end()) {
		throw cde::ClothDiagnostic(Code::UNSUPPORTED_FUNCTION_ABI, "LLVM lowering requires primitive parameters and at most one primitive result",
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

struct LocalValue {
	LLVMValueRef               operand;
	const cloth::ResolvedType *type;
	bool                       isAddress;
};

class FunctionLowering {
    public:
	FunctionLowering (ModuleFunctions &functions, LLVMBuilderRef builder, const cloth::DeclarationInfo &declaration);
	void lowerFunction (const cloth::CallableSignature &signature, LLVMValueRef function);

    private:
	const LocalValue *findLocal (const cloth::Token *declaration) const;
	void              bindLocal (const cloth::Token &name, const cloth::ResolvedType &type, LLVMValueRef value, bool isMutable);
	void              lowerStatement (const cloth::Statement &statement);
	void              lowerReturn (const cloth::Expression *expression) const;
	LLVMValueRef      lowerExpression (const cloth::Expression &expression) const;
	LLVMValueRef      lowerAs (const cloth::Expression &expression, const cloth::ResolvedType &destination) const;
	LLVMValueRef      lowerBinary (const cloth::BinaryExpression &binary) const;
	LLVMValueRef      lowerLogical (const cloth::BinaryExpression &binary) const;
	LLVMValueRef      lowerCall (const cloth::CallExpression &call) const;
	void              lowerBinding (const cloth::BindingDeclaration &binding);
	void              lowerAssignment (const cloth::AssignmentStatement &assignment) const;
	void              lowerUpdate (const cloth::UpdateStatement &update) const;
	const LocalValue &mutableTarget (const cloth::Statement &write, const cloth::Token &name) const;

	ModuleFunctions                                     &mf_functions;
	LLVMBuilderRef                                       ir_builder;
	const cloth::DeclarationInfo                        &di_function;
	detail::ScalarLowering                               sl_scalars;
	std::unordered_map<const cloth::Token *, LocalValue> m_locals;
};

FunctionLowering::FunctionLowering (ModuleFunctions &functions, LLVMBuilderRef builder, const cloth::DeclarationInfo &declaration) :
        mf_functions(functions),
        ir_builder(builder),
        di_function(declaration),
        sl_scalars(functions.module, builder, functions.program.layout) {
}

const LocalValue *FunctionLowering::findLocal (const cloth::Token *declaration) const {
	const decltype(m_locals)::const_iterator it_local = m_locals.find(declaration);
	return it_local == m_locals.end() ? nullptr : &it_local->second;
}

bool isBuiltinOperation (const cloth::ExpressionInfo &info) {
	return !info.symbol && !info.leftContext && !info.rightContext && !info.argumentContract && !info.dispatch;
}

LLVMValueRef FunctionLowering::lowerAs (const cloth::Expression &expression, const cloth::ResolvedType &destination) const {
	const LLVMValueRef         ir_value  = lowerExpression(expression);
	const cloth::ResolvedType &rt_source = mf_functions.program.expressions.at(&expression).types.front();
	return sl_scalars.convert(ir_value, rt_source, destination);
}

LLVMValueRef FunctionLowering::lowerExpression (const cloth::Expression &expression) const {
	const cloth::ExpressionFacts &ef_expressions = mf_functions.program.expressions;
	const cloth::ExpressionInfo  &ei_value       = ef_expressions.at(&expression);
	const cloth::ResolvedType    *rt_result      = ei_value.resultKind() == cloth::ResultKind::Single ? &ei_value.types.front() : nullptr;
	if (dynamic_cast<const cloth::NumberLiteral *>(&expression) || dynamic_cast<const cloth::BooleanLiteral *>(&expression)) {
		return sl_scalars.constant(*rt_result, ei_value.numericConstant);
	}
	if (const auto *cl_literal = dynamic_cast<const cloth::CharacterLiteral *>(&expression)) {
		return LLVMConstInt(sl_scalars.type(*rt_result), cl_literal->value, false);
	}
	if (dynamic_cast<const cloth::NameExpression *>(&expression)) {
		if (const LocalValue *lv_local = findLocal(ei_value.binding)) {
			return lv_local->isAddress ? LLVMBuildLoad2(ir_builder, sl_scalars.type(*lv_local->type), lv_local->operand, "") : lv_local->operand;
		}
	}
	if (const auto *ce_call = dynamic_cast<const cloth::CallExpression *>(&expression)) {
		return lowerCall(*ce_call);
	}
	if (const auto *ce_cast = dynamic_cast<const cloth::CastExpression *>(&expression); ce_cast && isBuiltinOperation(ei_value)) {
		if (ei_value.resultKind() == cloth::ResultKind::Empty) {
			lowerExpression(*ce_cast->operand);
			return nullptr;
		}
		const cloth::ResultTypes &v_source = ef_expressions.at(ce_cast->operand.get()).types;
		if (rt_result && sl_scalars.type(*rt_result) && v_source.size() == 1 && sl_scalars.type(v_source.front()))
			return sl_scalars.checkedCast(lowerExpression(*ce_cast->operand), v_source.front(), ei_value);
	}
	if (const auto *mc_call = dynamic_cast<const cloth::MemberCallExpression *>(&expression);
	    mc_call && ei_value.builtin && ei_value.builtin->operation == cloth::BuiltinOperation::ToInt) {
		return lowerAs(*mc_call->receiver, *rt_result);
	}
	if (const auto *pa_provider = dynamic_cast<const cloth::ProviderAccessExpression *>(&expression);
	    pa_provider && ei_value.provider && sl_scalars.type(ei_value.provider->receiver) &&
	    !std::holds_alternative<std::monostate>(ei_value.numericConstant)) {
		if (!ei_value.provider->typeReceiver) lowerExpression(*pa_provider->receiver);
		return sl_scalars.constant(*rt_result, ei_value.numericConstant);
	}
	if (const auto *tc_test = dynamic_cast<const cloth::TypeCheckExpression *>(&expression);
	    tc_test && ei_value.testedType && sl_scalars.type(*ei_value.testedType)) {
		const cloth::ResolvedType &rt_source = ef_expressions.at(tc_test->operand.get()).types.front();
		if (sl_scalars.type(rt_source)) {
			lowerExpression(*tc_test->operand);
			return LLVMConstInt(sl_scalars.type(*rt_result), rt_source == *ei_value.testedType, false);
		}
	}
	if (const auto *ue_unary = dynamic_cast<const cloth::UnaryExpression *>(&expression);
	    ue_unary && isBuiltinOperation(ei_value) && rt_result && sl_scalars.type(*rt_result)) {
		switch (ue_unary->op.get_operator()) {
		case cloth::oPlus:
			return lowerExpression(*ue_unary->operand);
		case cloth::oMinus: {
			const LLVMValueRef ir_operand = lowerExpression(*ue_unary->operand);
			return rt_result->floating() ? LLVMBuildFNeg(ir_builder, ir_operand, "") : LLVMBuildNeg(ir_builder, ir_operand, "");
		}
		case cloth::oTilde:
			return LLVMBuildNot(ir_builder, lowerExpression(*ue_unary->operand), "");
		case cloth::oNot:
			return sl_scalars.boolean(LLVMBuildNot(ir_builder, sl_scalars.predicate(lowerExpression(*ue_unary->operand)), ""));
		default:
			break;
		}
	}
	if (const auto *be_binary = dynamic_cast<const cloth::BinaryExpression *>(&expression)) {
		return lowerBinary(*be_binary);
	}
	throw cde::ClothDiagnostic(Code::UNSUPPORTED_LOWERING, "LLVM lowering does not support this expression",
	                           { { expression.get_span(), "unsupported expression" } });
}

LLVMValueRef FunctionLowering::lowerLogical (const cloth::BinaryExpression &binary) const {
	const bool              b_and       = binary.op.get_operator() == cloth::oAnd;
	LLVMValueRef            ir_left     = lowerExpression(*binary.left);
	LLVMBasicBlockRef       bb_left     = LLVMGetInsertBlock(ir_builder);
	const LLVMValueRef      fn_function = LLVMGetBasicBlockParent(bb_left);
	const LLVMContextRef    ctx_context = LLVMGetModuleContext(mf_functions.module);
	LLVMBasicBlockRef       bb_right    = LLVMAppendBasicBlockInContext(ctx_context, fn_function, "logical.rhs");
	const LLVMBasicBlockRef bb_end      = LLVMAppendBasicBlockInContext(ctx_context, fn_function, "logical.end");
	LLVMBuildCondBr(ir_builder, sl_scalars.predicate(ir_left), b_and ? bb_right : bb_end, b_and ? bb_end : bb_right);
	LLVMPositionBuilderAtEnd(ir_builder, bb_right);
	LLVMValueRef ir_right = lowerExpression(*binary.right);
	bb_right              = LLVMGetInsertBlock(ir_builder);
	LLVMBuildBr(ir_builder, bb_end);
	LLVMPositionBuilderAtEnd(ir_builder, bb_end);
	const LLVMValueRef ir_result = LLVMBuildPhi(ir_builder, LLVMTypeOf(ir_left), "");
	LLVMAddIncoming(ir_result, &ir_left, &bb_left, 1);
	LLVMAddIncoming(ir_result, &ir_right, &bb_right, 1);
	return ir_result;
}

LLVMValueRef FunctionLowering::lowerBinary (const cloth::BinaryExpression &binary) const {
	const cloth::ExpressionFacts &ef_facts  = mf_functions.program.expressions;
	const cloth::ExpressionInfo  &ei_binary = ef_facts.at(&binary);
	if (const cloth::Operator o_op = binary.op.get_operator(); isBuiltinOperation(ei_binary) && !ei_binary.range &&
	                                                           (!cloth::isComparison(o_op) || ei_binary.booleanEquality || ei_binary.comparisonType) &&
	                                                           ei_binary.resultKind() == cloth::ResultKind::Single) {
		const cloth::ExpressionInfo &ei_right = ef_facts.at(binary.right.get());
		if (const cloth::ExpressionInfo &ei_left = ef_facts.at(binary.left.get());
		    ei_left.resultKind() == cloth::ResultKind::Single && ei_right.resultKind() == cloth::ResultKind::Single &&
		    sl_scalars.type(ei_left.types.front()) && sl_scalars.type(ei_right.types.front())) {
			if (o_op == cloth::oAnd || o_op == cloth::oOr) return lowerLogical(binary);
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

LLVMValueRef FunctionLowering::lowerCall (const cloth::CallExpression &call) const {
	const cloth::ExpressionInfo &ei_call    = mf_functions.program.expressions.at(&call);
	const cloth::Symbol         *sym_callee = ei_call.symbol;
	if (!sym_callee || sym_callee->kind != cloth::SymbolKind::Function || ei_call.instance || ei_call.dispatch) {
		throw cde::ClothDiagnostic(Code::UNSUPPORTED_LOWERING, "LLVM lowering requires a direct call to a non-generic free function",
		                           { { call.get_span(), "unsupported call" } });
	}
	if (!ei_call.omittedParameters.empty()) {
		throw cde::ClothDiagnostic(Code::UNSUPPORTED_LOWERING, "LLVM lowering requires an explicit argument for each parameter",
		                           { { call.get_span(), "omitted argument" } });
	}
	const LLVMValueRef        fn_callee = mf_functions.declareFunction(*sym_callee);
	std::vector<LLVMValueRef> v_arguments(call.suppliedArguments.size());
	// Evaluate in source order, then pass each value in its checked parameter position.
	for (std::size_t st_index = 0; st_index < call.suppliedArguments.size(); ++st_index) {
		const std::size_t st_parameter = ei_call.parameterForArgument.at(st_index);
		v_arguments.at(st_parameter) =
		        lowerAs(*call.suppliedArguments[st_index].expression, mf_functions.program.declarations.at(sym_callee).parameters.at(st_parameter));
	}
	return LLVMBuildCall2(ir_builder, LLVMGlobalGetValueType(fn_callee), fn_callee, v_arguments.data(), static_cast<unsigned>(v_arguments.size()), "");
}

void FunctionLowering::bindLocal (const cloth::Token &name, const cloth::ResolvedType &type, LLVMValueRef value, bool isMutable) {
	if (isMutable) {
		const LLVMValueRef ir_slot = LLVMBuildAlloca(ir_builder, sl_scalars.type(type), "");
		LLVMBuildStore(ir_builder, value, ir_slot);
		value = ir_slot;
	}
	m_locals.emplace(&name, LocalValue{ value, &type, isMutable });
}

void FunctionLowering::lowerBinding (const cloth::BindingDeclaration &binding) {
	const cloth::BodyFacts &bf_facts = mf_functions.program;
	const auto              it_type  = bf_facts.localTypes.find(&binding.name);
	if (it_type == bf_facts.localTypes.end() || !sl_scalars.type(it_type->second)) {
		throw cde::ClothDiagnostic(Code::UNSUPPORTED_LOWERING, "LLVM lowering requires a primitive local type",
		                           { { binding.type ? binding.type->get_span() : binding.initializer->get_span(), "unsupported local type" } });
	}
	bindLocal(binding.name, it_type->second, lowerAs(*binding.initializer, it_type->second), binding.isMutable);
}

const LocalValue &FunctionLowering::mutableTarget (const cloth::Statement &write, const cloth::Token &name) const {
	const cloth::BodyFacts &bf_facts  = mf_functions.program;
	const auto              it_target = bf_facts.references.find(&name);
	const LocalValue       *lv_target = it_target == bf_facts.references.end() ? nullptr : findLocal(it_target->second);
	if (!lv_target || !lv_target->isAddress || bf_facts.writes.contains(&write)) {
		throw cde::ClothDiagnostic(Code::UNSUPPORTED_LOWERING, "LLVM lowering requires a built-in update on a mutable local",
		                           { { name.get_span(), "unsupported assignment target" } });
	}
	return *lv_target;
}

void FunctionLowering::lowerAssignment (const cloth::AssignmentStatement &assignment) const {
	const LocalValue &lv_target = mutableTarget(assignment, assignment.name);
	// The checker records a compound assignment's read and arithmetic on its synthetic binary expression.
	LLVMBuildStore(ir_builder, lowerAs(*assignment.value, *lv_target.type), lv_target.operand);
}

void FunctionLowering::lowerUpdate (const cloth::UpdateStatement &update) const {
	const LocalValue     &lv_target = mutableTarget(update, update.name);
	const LLVMValueRef    ir_old    = lowerExpression(*update.target);
	const LLVMTypeRef     ty_value  = LLVMTypeOf(ir_old);
	const LLVMValueRef    ir_one    = lv_target.type->floating() ? LLVMConstReal(ty_value, 1.) : LLVMConstInt(ty_value, 1, false);
	const cloth::Operator o_op      = update.op.get_operator() == cloth::oPlusPlus ? cloth::oPlus : cloth::oMinus;
	LLVMBuildStore(ir_builder, sl_scalars.binary(o_op, ir_old, ir_one, *lv_target.type), lv_target.operand);
}

void FunctionLowering::lowerReturn (const cloth::Expression *expression) const {
	const LLVMValueRef ir_value = !expression                 ? nullptr :
	                              di_function.returns.empty() ? lowerExpression(*expression) :
	                                                            lowerAs(*expression, di_function.returns.front());
	if (ir_value && LLVMGetTypeKind(LLVMTypeOf(ir_value)) != LLVMVoidTypeKind) {
		LLVMBuildRet(ir_builder, ir_value);
	} else {
		LLVMBuildRetVoid(ir_builder);
	}
}

void FunctionLowering::lowerStatement (const cloth::Statement &statement) {
	if (const auto *bd_binding = dynamic_cast<const cloth::BindingDeclaration *>(&statement)) return lowerBinding(*bd_binding);
	if (const auto *as_write = dynamic_cast<const cloth::AssignmentStatement *>(&statement)) return lowerAssignment(*as_write);
	if (const auto *us_update = dynamic_cast<const cloth::UpdateStatement *>(&statement)) return lowerUpdate(*us_update);
	if (const auto *rs_return = dynamic_cast<const cloth::ReturnStatement *>(&statement)) return lowerReturn(rs_return->value.get());
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
		for (const cloth::StatementPtr &sp_statement : bb_body->statements) {
			if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ir_builder))) {
				throw cde::ClothDiagnostic(Code::UNSUPPORTED_LOWERING, "LLVM lowering found a statement after the return",
				                           { { sp_statement->get_span(), "additional statement" } });
			}
			lowerStatement(*sp_statement);
		}
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

	TargetMachine tm_machine(LLVMCreateTargetMachine(t_target, s_triple.get(), "", "", LLVMCodeGenLevelNone, LLVMRelocDefault, LLVMCodeModelDefault),
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
