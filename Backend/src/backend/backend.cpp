// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/driver/Objects.rst
// See Documentation/compiler/internals/Backend.rst

#include <backend/array_lifetimes.hpp>
#include <backend/backend.hpp>
#include <backend/class_objects.hpp>
#include <backend/function_abi.hpp>
#include <backend/growable_array.hpp>
#include <backend/integer_range.hpp>
#include <backend/lowered_module.hpp>
#include <backend/nullable_value.hpp>
#include <backend/runtime.hpp>
#include <backend/scalar_lowering.hpp>
#include <backend/string_value.hpp>
#include <backend/value_types.hpp>

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <ast/decl/class_declaration.hpp>
#include <ast/decl/declarations.hpp>
#include <ast/decl/struct_declaration.hpp>
#include <ast/expr/class_expression.hpp>
#include <ast/expr/expression.hpp>
#include <ast/expr/expressions.hpp>
#include <ast/expr/member_call_expression.hpp>
#include <ast/expr/provider_expressions.hpp>
#include <ast/expr/struct_expressions.hpp>
#include <ast/stmt/do_while_statement.hpp>
#include <ast/stmt/for_statement.hpp>
#include <ast/stmt/loop_control.hpp>
#include <ast/stmt/statement.hpp>
#include <ast/stmt/statements.hpp>
#include <ast/stmt/while_statement.hpp>
#include <diagnostics.hpp>
#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Types.h>
#include <semantic/checked_program.hpp>
#include <semantic/symbol_table.hpp>
#include <semantic/target_layout.hpp>
#include <token/numeric_literals.hpp>
#include <token/token.hpp>

namespace backend {
namespace {

using detail::IntegerRange;
using detail::StoragePlace;
using detail::ArrayLifetimes;
namespace growable  = detail::growable_array;
using Code          = cde::DiagnosticCode;
using Message       = std::unique_ptr<char, decltype(&LLVMDisposeMessage)>;
using TargetMachine = std::unique_ptr<LLVMOpaqueTargetMachine, decltype(&LLVMDisposeTargetMachine)>;
using TargetData    = std::unique_ptr<LLVMOpaqueTargetData, decltype(&LLVMDisposeTargetData)>;
using MemoryBuffer  = std::unique_ptr<LLVMOpaqueMemoryBuffer, decltype(&LLVMDisposeMemoryBuffer)>;

using detail::ModuleFunctions;
using detail::ResultView;

struct ResultExit {
	ResultView                types;
	LLVMBasicBlockRef         block;
	LLVMValueRef              phi;
	std::vector<LLVMValueRef> buffers;
	ArrayLifetimes::Mark      scope;
};

enum class ValueStorage { Direct, ReadOnlySlot, MutableSlot };
enum class AddressUse { Read, ReadOnly, Write };

bool permitsSlot (ValueStorage storage, AddressUse use) {
	if (use == AddressUse::ReadOnly) return storage == ValueStorage::ReadOnlySlot;
	if (use == AddressUse::Write) return storage == ValueStorage::MutableSlot;
	return storage != ValueStorage::Direct;
}

struct ObjectAccess {
	LLVMValueRef handle     = nullptr;
	bool         directSelf = false;
};

struct LoweredValue {
	LLVMValueRef               operand;
	const cloth::ResolvedType &type;
	ValueStorage               storage;
	StoragePlace               place   = StoragePlace::Slot;
	LLVMValueRef               witness = nullptr;
	ObjectAccess               owner   = {};
};

struct ArrayBuffer {
	LLVMValueRef address;
	LLVMValueRef present;
	LLVMValueRef length;
};

struct LoopBlocks {
	LLVMBasicBlockRef continuation;
	LLVMBasicBlockRef exit;
	ArrayLifetimes::Mark owners{};
};

class FunctionLowering {
    public:
	FunctionLowering (ModuleFunctions &functions, LLVMBuilderRef builder, const cloth::DeclarationInfo &declaration);
	void lowerFunction (const cloth::CallableSignature &signature, const detail::FunctionBodyToEmit &body);

    private:
	const LoweredValue         *findLocal (const cloth::Token *declaration) const;
	const LoweredValue         *namedLocal (const cloth::Expression &expression) const;
	AddressUse                  arrayReadSlot (const cloth::Expression &index) const;
	LLVMValueRef                readValue (const LoweredValue &value, const cloth::ResolvedType &view);
	LLVMValueRef                storeInEntrySlot (LLVMValueRef value);
	const LoweredValue         &bindLocal (const cloth::Token &name, const cloth::ResolvedType &type, LLVMValueRef value, bool isMutable);
	std::optional<LoweredValue> writableArray (const cloth::Expression &array);
	std::optional<LoweredValue> lowerField (const cloth::FieldAccessExpression &field, std::optional<AddressUse> addressUse = std::nullopt);

	void                      lowerStatements (const std::vector<cloth::StatementPtr> &statements, ResultExit *tail = nullptr);
	ResultExit                makeResultExit (ResultView types, LLVMBasicBlockRef block);
	void                      yieldResults (ResultExit &exit, std::span<LLVMValueRef> values);
	std::vector<LLVMValueRef> lowerValues (const cloth::Expression &expression, ResultView destinations);
	LLVMValueRef              snapshot (const cloth::ResolvedType &type, LLVMValueRef value);
	LLVMValueRef              adoptResults (LLVMValueRef value, ResultView types);
	LLVMValueRef              lowerDeferred (const cloth::DeferredBinding &binding, const cloth::ResolvedType &view);
	void              receive (const cloth::Statement &statement, const std::vector<cloth::ReceivingTarget> &targets, const cloth::Expression &producer,
	                           std::optional<bool> mutableBinding);
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
	ArrayBuffer               lowerArrayBuffer (const cloth::Expression &operand, AddressUse use, bool needCells = true, LLVMValueRef evaluated = nullptr);
	LLVMValueRef              tryLowerArrayEquality (const cloth::BinaryExpression &binary, LLVMValueRef left, const cloth::ResolvedType &leftType,
	                                                 const cloth::ResolvedType &rightType);
	LLVMValueRef              equalArrayBuffers (const cloth::ResolvedType &type, ArrayBuffer left, ArrayBuffer right, bool checkPresence);
	LLVMValueRef lowerIndex (const cloth::IndexExpression &index, const cloth::ExpressionInfo &info);
	LLVMValueRef              lowerGrowableIndex (const cloth::IndexExpression &index, const cloth::ResolvedType &element, const cloth::ResolvedType &view);
	LLVMValueRef              lowerArrayMutation (const cloth::MemberCallExpression &call, cloth::ArrayMutation operation);
	cloth::StorageLayout      arrayCellLayout (const cloth::ResolvedType &type) const;
	void                      assignArray (LLVMValueRef slot, const cloth::ResolvedType &type, LLVMValueRef value);
	LLVMValueRef              allocateArrayOwner (const cloth::ResolvedType &type, ArrayLifetimes::Kind kind);
	LLVMValueRef              ownArray (const cloth::ResolvedType &type, LLVMValueRef value, ArrayLifetimes::Kind kind);
	LLVMValueRef              tryArrayAddress (const cloth::Expression &array, AddressUse use, ObjectAccess *owner = nullptr);
	LLVMValueRef              tryIndexAddress (const cloth::IndexExpression &index, AddressUse use, ObjectAccess *owner = nullptr);
	LLVMValueRef lowerArrayOffset (const cloth::Expression &index, LLVMTypeRef arrayType);
	LLVMValueRef arrayElementAddress (LLVMTypeRef arrayType, LLVMValueRef slot, LLVMValueRef offset) const;
	LLVMValueRef lowerStructLiteral (const cloth::StructLiteral &literal, const cloth::ExpressionInfo &info);
	LLVMValueRef lowerAs (const cloth::Expression &expression, const cloth::ResolvedType &destination, LLVMValueRef evaluated = nullptr);
	LLVMValueRef convertValue (LLVMValueRef value, const cloth::ResolvedType &source, const cloth::ResolvedType &destination,
	                           const cloth::ExpressionInfo *cast = nullptr);
	LLVMValueRef lowerBinary (const cloth::BinaryExpression &binary, LLVMValueRef left = nullptr);
	LLVMValueRef lowerRangeMembership (const cloth::BinaryExpression &binary);
	LLVMValueRef              lowerGuardedAccess (const cloth::GuardedAccessExpression &access, const cloth::ResolvedType *result);
	LLVMValueRef lowerNullableFallback (const cloth::ElvisExpression &expression, const cloth::ResolvedType &result);
	LLVMValueRef lowerLogical (const cloth::BinaryExpression &binary, LLVMValueRef left);
	LLVMValueRef lowerCall (const cloth::CallExpression &call);
	std::vector<LLVMValueRef> lowerArguments (const std::vector<cloth::CallArgument> &supplied, const cloth::ExpressionInfo &info,
	                                          LLVMValueRef self = nullptr);
	LLVMValueRef              lowerObjectCall (const cloth::CallSite &call, const cloth::ExpressionInfo &info, const cloth::Expression *receiver);
	void                      initializeClass (const cloth::Symbol &owner, const cloth::ClassDeclaration &decl);
	void                      invokeConstructor (const cloth::ExpressionInfo &info, std::vector<LLVMValueRef> arguments, LLVMValueRef self);
	LLVMValueRef              objectData (ObjectAccess access) const;
	LLVMValueRef              handleOrZero (LLVMValueRef value, const cloth::ResolvedType &type) const;
	LoweredValue              objectField (const cloth::Symbol &field, ObjectAccess access);
	void                      validateReference (LLVMValueRef value, const cloth::ResolvedType &type, bool directSelf);
	LLVMValueRef              equalObjects (ObjectAccess left, ObjectAccess right, bool nullable, bool check);
	void                      validateWrite (ObjectAccess owner) const;
	void                      lowerStatementBody (const cloth::Statement &statement);
	void                      lowerOwnershipIntrinsic (cloth::LibraryIntrinsic intrinsic, LLVMValueRef function);

	bool                      hasTerminatingDefault (const cloth::ExpressionInfo &info) const;
	LLVMValueRef              afterTerminatingDefault (const cloth::ExpressionInfo &info);
	LLVMValueRef              lowerConstruction (const cloth::ClassCreationExpression &creation, const cloth::ExpressionInfo &info);
	LLVMValueRef lowerPrintln (const cloth::CallExpression &call);
	LLVMValueRef lowerExit (const cloth::CallExpression &call, const cloth::BuiltinCall &builtin);

	void                lowerBinding (const cloth::BindingDeclaration &binding);
	void                lowerAssignment (const cloth::AssignmentStatement &assignment);
	void                lowerElementAssignment (const cloth::IndexExpression &target, cloth::Operator op, const cloth::Expression *value,
	                                            const cloth::BinaryExpression *compound);
	void                lowerGrowableElementAssignment (const LoweredValue &array, const cloth::IndexExpression &target, cloth::Operator op,
	                                                    const cloth::Expression *value, const cloth::BinaryExpression *compound);
	void                lowerFieldWrite (const cloth::FieldWriteStatement &write);
	void                storeExpression (const LoweredValue &target, const cloth::Expression &value);
	void                storeCompound (const LoweredValue &target, const cloth::BinaryExpression &compound);
	void                lowerUpdate (const LoweredValue &target, const cloth::Expression &read, cloth::Operator op);
	LoweredValue        mutableTarget (const cloth::Statement &write, const cloth::Token &name);

	ModuleFunctions                                                 &mf_functions;
	LLVMBuilderRef                                                   ir_builder;
	const cloth::DeclarationInfo                                    &di_function;
	ResultView                                                       rv_returns;
	ResultExit                                                      *re_deferred = nullptr;
	LLVMValueRef                                                     ir_self     = nullptr;
	cloth::ReceiverChecks                                            rc_write    = {};
	detail::ScalarLowering                                           sl_scalars;
	ArrayLifetimes                                                   al_arrays;
	std::unordered_map<const cloth::Token *, LoweredValue>           m_locals;
	std::unordered_map<const cloth::GuardedReceiver *, LLVMValueRef> m_guardedPayloads;
	std::unordered_map<const cloth::LoopTarget *, LoopBlocks>        m_loopBlocks;
};

FunctionLowering::FunctionLowering (ModuleFunctions &functions, LLVMBuilderRef builder, const cloth::DeclarationInfo &declaration) :
        mf_functions(functions),
        ir_builder(builder),
        di_function(declaration),
        rv_returns(declaration.returns),
        sl_scalars(functions.module, builder, functions.program.layout),
        al_arrays(functions.module, builder) {
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
	const LLVMValueRef ir_value =
	        value.storage == ValueStorage::Direct ? value.operand :
	        growable::ownsBuffer(value.type)      ? LLVMBuildLoad2(ir_builder, mf_functions.valueTypes.localType(value.type), value.operand, "") :
	                                                mf_functions.valueTypes.loadLocal(ir_builder, value.type, value.operand, value.place);
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
	if (source.objectReference() && destination.objectReference()) return value;
	if (destination.nullable() && destination.element().objectReference()) {
		if (source.objectReference()) return detail::nullable::pack(ir_builder, value);
		if (source.nullable() && source.element().objectReference()) return value;
	}
	if (growable::supported(destination) && source.array() && source.element() == destination.element()) {
		const LLVMValueRef ir_count = LLVMConstInt(sl_scalars.type(cloth::ResolvedType(cloth::KUsize)), LLVMGetArrayLength2(LLVMTypeOf(value)), false);
		return growable::borrow(ir_builder, mf_functions.valueTypes.localType(destination), storeInEntrySlot(value), ir_count);
	}
	if (growable::supported(source) && destination.array() && source.element() == destination.element()) {
		const LLVMTypeRef ty_fixed = mf_functions.valueTypes.localType(destination);
		if (!ty_fixed) throw cde::ClothDiagnostic(Code::UnsupportedLowering, "LLVM lowering requires a representable fixed-size array");
		const growable::Elements elements  = growable::elements(ir_builder, value);
		const std::uint64_t      u64_count = LLVMGetArrayLength2(ty_fixed);
		sl_scalars.checkArrayLength(elements.length, u64_count);
		return u64_count ? LLVMBuildLoad2(ir_builder, ty_fixed, elements.data, "") : LLVMConstNull(ty_fixed);
	}
	if (!destination.nullable()) return cast ? sl_scalars.checkedCast(value, source, destination, *cast) : sl_scalars.convert(value, source, destination);
	const cloth::ResolvedType rt_payload = destination.element();
	if (source == cloth::ResolvedType(cloth::KNull)) return LLVMConstNull(mf_functions.valueTypes.localType(destination));
	if (source == rt_payload) return detail::nullable::pack(ir_builder, value);
	if (source.array() && rt_payload.array()) return detail::nullable::pack(ir_builder, convertValue(value, source, rt_payload, cast));
	const LLVMValueRef        ir_presence = source.nullable() ? detail::nullable::presence(ir_builder, value) : nullptr;
	const cloth::ResolvedType rt_from     = source.nullable() ? source.element() : source;
	if (ir_presence && rt_from.array() && rt_payload.array()) {
		// Only a present buffer has an extent to check or cells to capture.
		const LLVMBasicBlockRef bb_absent  = LLVMGetInsertBlock(ir_builder);
		const LLVMBasicBlockRef bb_present = appendBlock("array.present");
		const LLVMBasicBlockRef bb_end     = appendBlock("array.converted");
		LLVMBuildCondBr(ir_builder, sl_scalars.predicate(ir_presence), bb_present, bb_end);
		LLVMPositionBuilderAtEnd(ir_builder, bb_present);
		const LLVMValueRef ir_array   = convertValue(detail::nullable::payload(ir_builder, value), rt_from, rt_payload, cast);
		LLVMValueRef       v_values[] = { LLVMConstNull(mf_functions.valueTypes.localType(destination)), detail::nullable::pack(ir_builder, ir_array) };
		LLVMBasicBlockRef  v_blocks[] = { bb_absent, LLVMGetInsertBlock(ir_builder) };
		LLVMBuildBr(ir_builder, bb_end);
		LLVMPositionBuilderAtEnd(ir_builder, bb_end);
		const LLVMValueRef ir_result = LLVMBuildPhi(ir_builder, LLVMTypeOf(v_values[0]), "");
		LLVMAddIncoming(ir_result, v_values, v_blocks, 2);
		return ir_result;
	}
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
	if (const auto *ga_access = dynamic_cast<const cloth::GuardedAccessExpression *>(&expression)) return lowerGuardedAccess(*ga_access, rt_result);
	if (const auto *gr_payload = dynamic_cast<const cloth::GuardedReceiver *>(&expression)) return m_guardedPayloads.at(gr_payload);
	if (const auto *ee_fallback = dynamic_cast<const cloth::ElvisExpression *>(&expression); ee_fallback && isBuiltinOperation(ei_value)) {
		if (!ef_expressions.at(ee_fallback->left.get()).fallsThrough) return lowerExpression(*ee_fallback->left);
		if (rt_result && mf_functions.valueTypes.localType(*rt_result)) return lowerNullableFallback(*ee_fallback, *rt_result);
		if (!ei_value.fallsThrough && dynamic_cast<const cloth::NullLiteral *>(ee_fallback->left.get())) return lowerExpression(*ee_fallback->fallback);
	}
	if (const auto *rl_list = dynamic_cast<const cloth::ResultListExpression *>(&expression)) {
		std::vector<LLVMValueRef> v_values;
		v_values.reserve(rl_list->values.size());
		for (const cloth::ExpressionPtr &ep_value : rl_list->values) {
			const LLVMValueRef           ir_value = lowerExpression(*ep_value);
			const cloth::ExpressionInfo &ei_item  = ef_expressions.at(ep_value.get());
			if (!ei_item.fallsThrough) return nullptr;
			v_values.push_back(snapshot(ei_item.types.front(), ir_value));
		}
		return detail::packResults(ir_builder, v_values);
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
			const LLVMValueRef ir_field = mf_functions.enumPayloads.read(ir_builder, *ei_value.symbol, ir_tag, fa_read->getSpan());
			return readValue(
			        { .operand = ir_field, .type = *mf_functions.program.declarations.at(ei_value.symbol).value, .storage = ValueStorage::Direct },
			        *rt_result);
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
	if (const auto *sl_literal = dynamic_cast<const cloth::StringLiteral *>(&expression))
		return detail::string_value::literal(mf_functions.module, *sl_literal);
	if (dynamic_cast<const cloth::NameExpression *>(&expression)) {
		if (const auto it_deferred = bf_facts.deferredBindings.find(ei_value.binding); it_deferred != bf_facts.deferredBindings.end())
			return lowerDeferred(it_deferred->second, *rt_result);
		if (const LoweredValue *lv_local = findLocal(ei_value.binding)) return readValue(*lv_local, *rt_result);
		if (ir_self && bf_facts.isDirectReceiver(expression)) return ir_self;
		if (ir_self && ei_value.symbol && ei_value.symbol->classFieldIndex)
			return readValue(objectField(*ei_value.symbol, { .handle = ir_self, .directSelf = true }), *rt_result);
	}
	if (const auto *deletion = dynamic_cast<const cloth::ClassDeletionExpression *>(&expression)) {
		const LLVMValueRef receiver = lowerExpression(*deletion->receiver);
		if (!receiver) return nullptr;
		const LLVMValueRef handle = handleOrZero(receiver, ef_expressions.at(deletion->receiver.get()).types.front());
		return detail::objectCall(ir_builder, mf_functions.module, detail::ObjectOperation::Delete, { handle, LLVMConstNull(LLVMTypeOf(handle)) });
	}
	if (const auto *creation = dynamic_cast<const cloth::ClassCreationExpression *>(&expression)) return lowerConstruction(*creation, ei_value);
	if (const auto *ce_call = dynamic_cast<const cloth::CallExpression *>(&expression)) {
		return lowerCall(*ce_call);
	}
	if (const auto *ce_cast = dynamic_cast<const cloth::CastExpression *>(&expression); ce_cast && isBuiltinOperation(ei_value)) {
		if (!ei_value.fallsThrough || ei_value.resultKind() == cloth::ResultKind::Empty) {
			lowerExpression(*ce_cast->operand);
			return nullptr;
		}
		const cloth::ResultTypes &v_source = ef_expressions.at(ce_cast->operand.get()).types;
		if (rt_result && mf_functions.valueTypes.localType(*rt_result) && v_source.size() == 1 && mf_functions.valueTypes.localType(v_source.front())) {
			const LLVMValueRef value = lowerExpression(*ce_cast->operand);
			if (ei_value.receiverChecks.atAccess) validateReference(value, v_source.front(), bf_facts.isDirectReceiver(*ce_cast->operand));
			return convertValue(value, v_source.front(), *rt_result, &ei_value);
		}
	}
	if (const auto *mc_call = dynamic_cast<const cloth::MemberCallExpression *>(&expression)) {
		if (!ef_expressions.at(mc_call->receiver.get()).fallsThrough) return lowerExpression(*mc_call->receiver);
		if (ei_value.arrayMutation) return lowerArrayMutation(*mc_call, *ei_value.arrayMutation);
		if (ei_value.symbol && ei_value.symbol->kind == cloth::SymbolKind::Method) return lowerObjectCall(*mc_call, ei_value, mc_call->receiver.get());
		if (ei_value.builtin && ei_value.builtin->operation == cloth::BuiltinOperation::ToInt) {
			if (!ei_value.fallsThrough) return lowerExpression(*mc_call->receiver);
			return lowerAs(*mc_call->receiver, *rt_result);
		}
	}
	if (const auto *pa_provider = dynamic_cast<const cloth::ProviderAccessExpression *>(&expression)) {
		if (!ei_value.fallsThrough && isBuiltinOperation(ei_value)) return lowerExpression(*pa_provider->receiver);
		if (ei_value.provider && ei_value.provider->kind == cloth::ProviderKind::StringSize)
			return detail::string_value::scalarCount(ir_builder, lowerExpression(*pa_provider->receiver));
		if (ei_value.provider && ei_value.provider->kind == cloth::ProviderKind::ArraySize && growable::supported(ei_value.provider->receiver))
			return growable::elements(ir_builder, lowerExpression(*pa_provider->receiver)).length;
		if (ei_value.provider && (sl_scalars.type(ei_value.provider->receiver) || ei_value.provider->kind == cloth::ProviderKind::ArraySize) &&
		    !std::holds_alternative<std::monostate>(ei_value.numericConstant)) {
			if (!ei_value.provider->typeReceiver) lowerExpression(*pa_provider->receiver);
			return detail::scalarConstant(sl_scalars.type(*rt_result), ei_value.numericConstant);
		}
	}
	if (const auto *tc_test = dynamic_cast<const cloth::TypeCheckExpression *>(&expression)) {
		if (const auto it_test = bf_facts.valueTests.find(tc_test); it_test != bf_facts.valueTests.end()) return lowerBinary(*it_test->second);
		if (!ei_value.fallsThrough) return lowerExpression(*tc_test->operand);
		if (ei_value.testedType && ei_value.testedType->objectReference() && !ei_value.argumentContract) {
			const auto        &source = ef_expressions.at(tc_test->operand.get());
			const LLVMValueRef value  = lowerExpression(*tc_test->operand);
			if (!source.fallsThrough) return nullptr;
			const auto &type = source.types.front();
			if (!(type.nullable() ? type.element() : type).objectReference()) return LLVMConstNull(sl_scalars.type(*rt_result));
			return detail::objectCall(ir_builder, mf_functions.module, detail::ObjectOperation::Is,
			                          { handleOrZero(value, type), mf_functions.objects.typeTag(*ei_value.testedType),
			                            LLVMConstInt(sl_scalars.type(*rt_result), bf_facts.isDirectReceiver(*tc_test->operand), false) });
		}
		if (ei_value.testedType && !ei_value.testedType->objectReference() && sl_scalars.type(*ei_value.testedType)) {
			const cloth::ResolvedType &rt_source = ef_expressions.at(tc_test->operand.get()).types.front();
			if (sl_scalars.type(rt_source) || (rt_source.nullable() && rt_source.element().objectReference())) {
				lowerExpression(*tc_test->operand);
				return LLVMConstInt(sl_scalars.type(*rt_result), rt_source == *ei_value.testedType, false);
			}
		}
	}
	if (const auto *ue_unary = dynamic_cast<const cloth::UnaryExpression *>(&expression); ue_unary && isBuiltinOperation(ei_value)) {
		if (!ei_value.fallsThrough) return lowerExpression(*ue_unary->operand);
		if (rt_result && sl_scalars.type(*rt_result)) {
			const LLVMValueRef ir_operand = lowerExpression(*ue_unary->operand);
			switch (ue_unary->op.getOperator()) {
			case cloth::OPlus:
				return ir_operand;
			case cloth::OMinus:
				return rt_result->floating() ? LLVMBuildFNeg(ir_builder, ir_operand, "") : LLVMBuildNeg(ir_builder, ir_operand, "");
			case cloth::OTilde:
				return LLVMBuildNot(ir_builder, ir_operand, "");
			case cloth::ONot:
				return sl_scalars.boolean(LLVMBuildNot(ir_builder, sl_scalars.predicate(ir_operand), ""));
			default:
				break;
			}
		}
	}
	if (const auto *be_binary = dynamic_cast<const cloth::BinaryExpression *>(&expression)) {
		return lowerBinary(*be_binary);
	}
	throw cde::ClothDiagnostic(Code::UnsupportedLowering, "LLVM lowering does not support this expression",
	                           { { .span = expression.getSpan(), .text = "unsupported expression" } });
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
		throw cde::ClothDiagnostic(Code::UnsupportedLowering, "LLVM lowering requires a fixed array with supported element storage",
		                           { { .span = literal.getSpan(), .text = "unsupported array literal" } });
	const cloth::ResolvedType   rt_element = rt_array.element();
	const LLVMTypeRef           ty_element = LLVMGetElementType(ty_array);
	std::vector<LLVMValueRef>   v_constants;
	std::vector<DynamicElement> v_dynamic;
	v_constants.reserve(literal.elements.size());
	for (const cloth::ExpressionPtr &ep_element : literal.elements) {
		LLVMValueRef ir_element = lowerAs(*ep_element, rt_element);
		if (!LLVMIsConstant(ir_element)) {
			v_dynamic.push_back({ .index = static_cast<unsigned>(v_constants.size()), .value = ir_element });
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

ArrayBuffer FunctionLowering::lowerArrayBuffer (const cloth::Expression &operand, AddressUse use, bool needCells, LLVMValueRef evaluated) {
	const cloth::ResolvedType &rt_operand = mf_functions.program.expressions.at(&operand).types.front();
	const LLVMValueRef         ir_true    = LLVMConstAllOnes(LLVMInt1TypeInContext(LLVMGetModuleContext(mf_functions.module)));
	if (growable::ownsBuffer(rt_operand)) {
		if (!evaluated) evaluated = lowerExpression(operand);
		const LoweredValue *lv_local = namedLocal(operand);
		// A mutable receiver can be resized or replaced while its captured elements are still in use.
		if (needCells && use == AddressUse::ReadOnly && (!lv_local || lv_local->storage == ValueStorage::MutableSlot)) {
			const LLVMValueRef ir_copy = ownArray(rt_operand, evaluated, ArrayLifetimes::Kind::Temporary);
			evaluated                  = LLVMBuildLoad2(ir_builder, LLVMTypeOf(evaluated), ir_copy, "");
		}
		LLVMValueRef ir_present = ir_true;
		if (rt_operand.nullable()) {
			ir_present = sl_scalars.predicate(detail::nullable::presence(ir_builder, evaluated));
			evaluated  = detail::nullable::payload(ir_builder, evaluated);
		}
		const growable::Elements elements = growable::elements(ir_builder, evaluated);
		return { .address = needCells ? elements.data : nullptr, .present = ir_present, .length = elements.length };
	}
	const bool                b_nullable = rt_operand.nullable();
	const cloth::ResolvedType rt_payload = b_nullable ? rt_operand.element() : rt_operand;
	const LLVMTypeRef         ty_buffer  = mf_functions.valueTypes.localType(rt_payload);
	const LLVMValueRef        ir_length  = LLVMConstInt(sl_scalars.type(cloth::ResolvedType(cloth::KUsize)), LLVMGetArrayLength2(ty_buffer), false);
	// A shape-only comparison needs effects, presence and length. A mutable slot's tag can be read immediately.
	// A snapshot of descriptor-bearing cells has a different stride from the physical buffer.
	if (!evaluated && (!needCells || mf_functions.valueTypes.slotType(rt_payload) == ty_buffer)) {
		if (const LLVMValueRef ir_slot = tryArrayAddress(operand, needCells ? use : AddressUse::Read)) {
			if (!b_nullable) return { .address = needCells ? ir_slot : nullptr, .present = ir_true, .length = ir_length };
			return { .address = needCells ? detail::nullable::payloadAddress(ir_builder, ty_buffer, ir_slot) : nullptr,
				 .present = sl_scalars.predicate(detail::nullable::loadPresence(ir_builder, ir_slot)),
				 .length  = ir_length };
		}
	}
	if (!evaluated) evaluated = lowerExpression(operand);
	const LLVMValueRef ir_present = b_nullable ? sl_scalars.predicate(detail::nullable::presence(ir_builder, evaluated)) : ir_true;
	const LLVMValueRef ir_slot    = needCells ? storeInEntrySlot(b_nullable ? detail::nullable::payload(ir_builder, evaluated) : evaluated) : nullptr;
	return { .address = ir_slot, .present = ir_present, .length = ir_length };
}

LLVMValueRef FunctionLowering::tryLowerArrayEquality (const cloth::BinaryExpression &binary, LLVMValueRef left, const cloth::ResolvedType &leftType,
                                                      const cloth::ResolvedType &rightType) {
	if (!cloth::semantic::hasArrayShape(leftType) || !cloth::semantic::hasArrayShape(rightType)) return nullptr;
	const cloth::ResolvedType          rt_left  = leftType.nullable() ? leftType.element() : leftType;
	const cloth::ResolvedType          rt_right = rightType.nullable() ? rightType.element() : rightType;
	const std::optional<std::uint64_t> opt_left = rt_left.arrayExtent(), opt_right = rt_right.arrayExtent();
	if ((!opt_left && !growable::supported(rt_left)) || (!opt_right && !growable::supported(rt_right))) return nullptr;
	cloth::ResolvedType rt_leaf = rt_left;
	rt_leaf.suffixes.clear();
	const cloth::ResolvedType *rt_endpoint = cloth::semantic::rangeEndpoint(rt_leaf);
	if (!(rt_endpoint && rt_endpoint->integer()) && !sl_scalars.type(rt_leaf)) return nullptr;

	const LLVMTypeRef ty_left = mf_functions.valueTypes.localType(rt_left), ty_right = mf_functions.valueTypes.localType(rt_right);
	if (!ty_left || !ty_right) return nullptr;
	const bool b_growable = !opt_left || !opt_right;
	// A fixed shape mismatch or a known empty operand needs no cells, even if the other operand has effects.
	const bool              b_differentShapes = !b_growable && ty_left != ty_right;
	const LLVMTargetDataRef td_target = LLVMGetModuleDataLayout(mf_functions.module);
	const bool              b_needCells       = b_growable ? opt_left != 0 && opt_right != 0 : !b_differentShapes && LLVMABISizeOfType(td_target, ty_left);
	const AddressUse        au_left           = namedLocal(*binary.right) ? AddressUse::Read : AddressUse::ReadOnly;
	const ArrayBuffer       ab_left           = lowerArrayBuffer(*binary.left, au_left, b_needCells, left);
	const ArrayBuffer       ab_right          = lowerArrayBuffer(*binary.right, AddressUse::Read, b_needCells);
	if (b_differentShapes) return LLVMBuildNot(ir_builder, LLVMBuildOr(ir_builder, ab_left.present, ab_right.present, ""), "");
	return equalArrayBuffers(!opt_right ? rt_right : rt_left, ab_left, ab_right, leftType.nullable() || rightType.nullable());
}

LLVMValueRef FunctionLowering::equalArrayBuffers (const cloth::ResolvedType &type, ArrayBuffer left, ArrayBuffer right, bool checkPresence) {
	const LLVMValueRef ir_samePresence = LLVMBuildICmp(ir_builder, LLVMIntEQ, left.present, right.present, "");
	LLVMValueRef       ir_enter        = checkPresence ? LLVMBuildAnd(ir_builder, left.present, right.present, "") : nullptr;
	LLVMValueRef       ir_skipped      = ir_samePresence;
	LLVMValueRef       ir_length       = left.length;
	// Walk contiguous dimensions in one loop. A nullable row has its own tag and starts a new comparison.
	cloth::ResolvedType rt_element = type.element();
	while (rt_element.array()) rt_element.suffixes.pop_back();
	const LLVMTypeRef ty_element = mf_functions.valueTypes.localType(rt_element);
	const LLVMTypeRef ty_offset  = LLVMTypeOf(ir_length);
	std::uint64_t     u64_extent = 0;
	if (growable::supported(type)) {
		const LLVMValueRef ir_sameLength = LLVMBuildICmp(ir_builder, LLVMIntEQ, left.length, right.length, "");
		ir_skipped                       = ir_enter ? LLVMBuildSelect(ir_builder, ir_enter, ir_sameLength, ir_samePresence, "") : ir_sameLength;
		if (!left.address || !right.address) return ir_skipped;
		const LLVMValueRef ir_hasCells = LLVMBuildAnd(ir_builder, ir_sameLength, LLVMBuildIsNotNull(ir_builder, ir_length, ""), "");
		ir_enter                       = ir_enter ? LLVMBuildAnd(ir_builder, ir_enter, ir_hasCells, "") : ir_hasCells;
	} else {
		const LLVMTargetDataRef td_target = LLVMGetModuleDataLayout(mf_functions.module);
		const std::uint64_t     u64_bytes = LLVMABISizeOfType(td_target, mf_functions.valueTypes.localType(type));
		if (!u64_bytes) return ir_samePresence;
		u64_extent = u64_bytes / LLVMABISizeOfType(td_target, ty_element);
		ir_length  = LLVMConstInt(ty_offset, u64_extent, false);
	}
	// A zero extent supplies the element stride for a runtime-sized buffer. The loop bound is its captured length.
	const LLVMTypeRef       ty_array        = LLVMArrayType2(ty_element, u64_extent);
	const LLVMBasicBlockRef bb_start        = LLVMGetInsertBlock(ir_builder);
	const LLVMBasicBlockRef bb_compare      = appendBlock("array.equal.element");
	const LLVMBasicBlockRef bb_end          = appendBlock("array.equal.end");
	if (ir_enter) {
		LLVMBuildCondBr(ir_builder, ir_enter, bb_compare, bb_end);
	} else {
		LLVMBuildBr(ir_builder, bb_compare);
	}
	LLVMPositionBuilderAtEnd(ir_builder, bb_compare);
	const LLVMValueRef ir_offset    = LLVMBuildPhi(ir_builder, ty_offset, "");
	const LLVMValueRef ir_leftCell  = arrayElementAddress(ty_array, left.address, ir_offset);
	const LLVMValueRef ir_rightCell = arrayElementAddress(ty_array, right.address, ir_offset);
	LLVMValueRef       ir_equal     = nullptr;
	if (cloth::semantic::hasArrayShape(rt_element)) {
		const cloth::ResolvedType rt_row = rt_element.element();
		const LLVMTypeRef         ty_row = mf_functions.valueTypes.localType(rt_row);
		const auto                rowAt  = [&] (LLVMValueRef cell) -> ArrayBuffer {
			return { .address = detail::nullable::payloadAddress(ir_builder, ty_row, cell),
				 .present = sl_scalars.predicate(detail::nullable::loadPresence(ir_builder, cell)),
				 .length  = LLVMConstInt(ty_offset, LLVMGetArrayLength2(ty_row), false) };
		};
		const ArrayBuffer ab_left = rowAt(ir_leftCell), ab_right = rowAt(ir_rightCell);
		ir_equal = equalArrayBuffers(rt_row, ab_left, ab_right, true);
	} else {
		const LLVMValueRef ir_left  = LLVMBuildLoad2(ir_builder, ty_element, ir_leftCell, "");
		const LLVMValueRef ir_right = LLVMBuildLoad2(ir_builder, ty_element, ir_rightCell, "");
		if ((rt_element.nullable() ? rt_element.element() : rt_element).objectReference())
			ir_equal = equalObjects({ .handle = handleOrZero(ir_left, rt_element), .directSelf = false },
			                        { .handle = handleOrZero(ir_right, rt_element), .directSelf = false }, rt_element.nullable(), true);
		else if (cloth::semantic::rangeEndpoint(rt_element))
			ir_equal = IntegerRange::equalPacked(ir_builder, ir_left, ir_right, rt_element.nullable());
		else
			ir_equal = sl_scalars.compare(cloth::OEqual, ir_left, ir_right, rt_element);
	}
	const LLVMValueRef ir_next     = LLVMBuildAdd(ir_builder, ir_offset, LLVMConstInt(ty_offset, 1, false), "");
	LLVMValueRef       v_offsets[] = { LLVMConstNull(ty_offset), ir_next };
	LLVMBasicBlockRef  v_blocks[]  = { bb_start, LLVMGetInsertBlock(ir_builder) };
	LLVMAddIncoming(ir_offset, v_offsets, v_blocks, 2);
	const LLVMValueRef ir_more = LLVMBuildICmp(ir_builder, LLVMIntNE, ir_next, ir_length, "");
	LLVMBuildCondBr(ir_builder, LLVMBuildAnd(ir_builder, ir_equal, ir_more, ""), bb_compare, bb_end);
	LLVMPositionBuilderAtEnd(ir_builder, bb_end);
	if (!ir_enter) return ir_equal;
	const LLVMValueRef ir_result   = LLVMBuildPhi(ir_builder, LLVMTypeOf(ir_equal), "");
	LLVMValueRef       v_results[] = { ir_skipped, ir_equal };
	LLVMAddIncoming(ir_result, v_results, v_blocks, 2);
	return ir_result;
}

AddressUse FunctionLowering::arrayReadSlot (const cloth::Expression &index) const {
	if (dynamic_cast<const cloth::IntegerLiteral *>(&index) || namedLocal(index)) return AddressUse::Read;
	// A computed subscript can rebind the receiver before a read. Only an immutable slot is safe to borrow.
	return AddressUse::ReadOnly;
}

LLVMValueRef FunctionLowering::tryArrayAddress (const cloth::Expression &array, AddressUse use, ObjectAccess *owner) {
	const auto &info = mf_functions.program.expressions.at(&array);
	if (ir_self && dynamic_cast<const cloth::NameExpression *>(&array) && info.symbol && info.symbol->classFieldIndex) {
		if (use == AddressUse::ReadOnly) return nullptr;
		const LoweredValue field = objectField(*info.symbol, { .handle = ir_self, .directSelf = true });
		if (owner) *owner = field.owner;
		return field.type.nullable() && !info.types.front().nullable() ?
		               detail::nullable::payloadAddress(ir_builder, mf_functions.valueTypes.slotType(field.type.element()), field.operand) :
		               field.operand;
	}
	if (const LoweredValue *lv_local = namedLocal(array)) {
		if (!permitsSlot(lv_local->storage, use)) return nullptr;
		if (lv_local->type.nullable() && !mf_functions.program.expressions.at(&array).types.front().nullable()) {
			const LLVMTypeRef ty_payload = mf_functions.valueTypes.slotType(lv_local->type.element());
			return detail::nullable::payloadAddress(ir_builder, ty_payload, lv_local->operand);
		}
		return lv_local->operand;
	}
	if (const auto *fa_field = dynamic_cast<const cloth::FieldAccessExpression *>(&array)) {
		const cloth::ExpressionInfo &ei_field = mf_functions.program.expressions.at(&array);
		if (!ei_field.symbol ||
		    (!ei_field.symbol->classFieldIndex && !std::holds_alternative<const cloth::StructFieldDeclaration *>(ei_field.symbol->declaration)))
			return nullptr;
		// A nullable field's tag and cells live in separate storage. Borrow the cells only after a presence proof.
		if (ei_field.types.front().nullable()) return nullptr;
		// Immutable structs are SSA values. Only an addressable root can lend its field buffer.
		const std::optional<LoweredValue> opt_field = lowerField(*fa_field, use);
		if (!opt_field) return nullptr;
		if (owner) *owner = opt_field->owner;
		if (opt_field->type.nullable() && opt_field->place == StoragePlace::Slot)
			return detail::nullable::payloadAddress(ir_builder, mf_functions.valueTypes.slotType(opt_field->type.element()), opt_field->operand);
		return opt_field->place == StoragePlace::Embedded ? mf_functions.valueTypes.arrayData(ir_builder, opt_field->type, opt_field->operand) :
		                                                    opt_field->operand;
	}
	if (const auto *ie_row = dynamic_cast<const cloth::IndexExpression *>(&array)) {
		const cloth::ExpressionFacts &ef_facts = mf_functions.program.expressions;
		const cloth::ExpressionInfo  &ei_row   = ef_facts.at(ie_row);
		// A nullable indexed row stores a descriptor beside its tag. Only a proven row lends its cell buffer.
		if (!ei_row.fallsThrough || ei_row.types.front().nullable()) return nullptr;
		const LLVMValueRef ir_cell = tryIndexAddress(*ie_row, use, owner);
		if (!ir_cell) return nullptr;
		const cloth::ResolvedType rt_element = ef_facts.at(ie_row->array.get()).types.front().element();
		return detail::arrayElementPlace(rt_element) == StoragePlace::Embedded ? mf_functions.valueTypes.arrayData(ir_builder, rt_element, ir_cell) :
		                                                                         ir_cell;
	}
	return nullptr;
}

LLVMValueRef FunctionLowering::tryIndexAddress (const cloth::IndexExpression &index, AddressUse use, ObjectAccess *owner) {
	const cloth::ExpressionFacts &ef_facts = mf_functions.program.expressions;
	const cloth::ExpressionInfo  &ei_index = ef_facts.at(&index);
	if (!ei_index.fallsThrough || ei_index.indexed || !isBuiltinOperation(ei_index)) return nullptr;
	const cloth::ResolvedType &rt_array = ef_facts.at(index.array.get()).types.front();
	if (!rt_array.arrayExtent()) return nullptr;
	if (use == AddressUse::Read) use = arrayReadSlot(*index.index);
	const LLVMValueRef ir_outer = tryArrayAddress(*index.array, use, owner);
	if (!ir_outer) return nullptr;
	const LLVMTypeRef ty_outer = mf_functions.valueTypes.slotType(rt_array);
	const LLVMValueRef offset   = lowerArrayOffset(*index.index, ty_outer);
	if (!offset) return nullptr;
	if (owner && owner->handle) objectData(*owner);
	return arrayElementAddress(ty_outer, ir_outer, offset);
}

LLVMValueRef FunctionLowering::lowerIndex (const cloth::IndexExpression &index, const cloth::ExpressionInfo &info) {
	const cloth::ExpressionInfo &ei_array = mf_functions.program.expressions.at(index.array.get());
	if (!ei_array.fallsThrough) return lowerExpression(*index.array);
	if (growable::supported(ei_array.types.front())) return lowerGrowableIndex(index, ei_array.types.front().element(), info.types.front());
	const LLVMValueRef ir_buffer = tryArrayAddress(*index.array, arrayReadSlot(*index.index));
	const LLVMValueRef ir_array  = ir_buffer ? nullptr : lowerExpression(*index.array);
	if (!ir_buffer && !ir_array) return nullptr;
	const LLVMTypeRef  ty_array  = ir_buffer ? mf_functions.valueTypes.slotType(ei_array.types.front()) : LLVMTypeOf(ir_array);
	const LLVMValueRef ir_offset = lowerArrayOffset(*index.index, ty_array);
	if (!ir_offset) return nullptr;
	const cloth::ResolvedType rt_element = ei_array.types.front().element();
	if (ir_array && LLVMIsAConstantInt(ir_offset)) {
		const std::uint64_t u64_offset = LLVMConstIntGetZExtValue(ir_offset);
		if (u64_offset < LLVMGetArrayLength2(ty_array) && u64_offset <= UINT_MAX) {
			const LLVMValueRef ir_element = LLVMBuildExtractValue(ir_builder, ir_array, static_cast<unsigned>(u64_offset), "");
			return readValue({ .operand = ir_element, .type = rt_element, .storage = ValueStorage::Direct }, info.types.front());
		}
	}
	// Keep the receiver's value from before index evaluation, even if the index reassigns the local.
	const LLVMValueRef ir_slot    = ir_buffer ? ir_buffer : storeInEntrySlot(ir_array);
	const LLVMValueRef ir_address = arrayElementAddress(ty_array, ir_slot, ir_offset);
	if (!ir_buffer) {
		const LLVMValueRef ir_element = LLVMBuildLoad2(ir_builder, LLVMGetElementType(ty_array), ir_address, "");
		return readValue({ .operand = ir_element, .type = rt_element, .storage = ValueStorage::Direct }, info.types.front());
	}
	return readValue({ .operand = ir_address, .type = rt_element, .storage = ValueStorage::ReadOnlySlot, .place = detail::arrayElementPlace(rt_element) },
	                 info.types.front());
}

cloth::StorageLayout FunctionLowering::arrayCellLayout (const cloth::ResolvedType &type) const {
	const cloth::ResolvedType                 rt_cell    = type.element();
	const std::optional<cloth::StorageLayout> opt_layout = mf_functions.program.storageLayout(rt_cell);
	const LLVMTypeRef                         ty_cell    = mf_functions.valueTypes.localType(rt_cell);
	if (!opt_layout || !ty_cell)
		throw cde::ClothDiagnostic(Code::UnsupportedLowering, "LLVM lowering requires a checked storage layout for the array element");
	const LLVMTargetDataRef    td_target = LLVMGetModuleDataLayout(mf_functions.module);
	const cloth::StorageLayout actual{ .size = LLVMABISizeOfType(td_target, ty_cell), .alignment = LLVMABIAlignmentOfType(td_target, ty_cell) };
	if (actual != *opt_layout)
		throw cde::ClothDiagnostic(Code::AbiLayout,
		                           std::format("LLVM array cell for '{}' differs from its checked storage layout", rt_cell.spelling()));
	return *opt_layout;
}

void FunctionLowering::assignArray (LLVMValueRef slot, const cloth::ResolvedType &type, LLVMValueRef value) {
	const bool                b_nullable = type.nullable();
	const cloth::ResolvedType rt_array   = b_nullable ? type.element() : type;
	if (b_nullable) {
		const LLVMValueRef ir_present = detail::nullable::presence(ir_builder, value);
		LLVMBuildStore(ir_builder, ir_present, slot);
		slot  = detail::nullable::payloadAddress(ir_builder, mf_functions.valueTypes.localType(rt_array), slot);
		value = detail::nullable::payload(ir_builder, value);
		value = LLVMBuildSelect(ir_builder, sl_scalars.predicate(ir_present), value, LLVMConstNull(LLVMTypeOf(value)), "");
	}
	const growable::Elements elements = growable::elements(ir_builder, value);
	growable::assign(ir_builder, mf_functions.module, slot, elements.data, elements.length, arrayCellLayout(rt_array));
}

LLVMValueRef FunctionLowering::allocateArrayOwner (const cloth::ResolvedType &type, ArrayLifetimes::Kind kind) {
	const bool        b_nullable    = type.nullable();
	const LLVMTypeRef ty_descriptor = mf_functions.valueTypes.localType(b_nullable ? type.element() : type);
	return al_arrays.own(ty_descriptor, kind, b_nullable);
}

LLVMValueRef FunctionLowering::ownArray (const cloth::ResolvedType &type, LLVMValueRef value, ArrayLifetimes::Kind kind) {
	const LLVMValueRef ir_slot = allocateArrayOwner(type, kind);
	assignArray(ir_slot, type, value);
	return ir_slot;
}

LLVMValueRef FunctionLowering::lowerGrowableIndex (const cloth::IndexExpression &index, const cloth::ResolvedType &element, const cloth::ResolvedType &view) {
	const ArrayBuffer  elements = lowerArrayBuffer(*index.array, arrayReadSlot(*index.index));
	const LLVMValueRef ir_index = lowerExpression(*index.index);
	if (!ir_index) return nullptr;
	const cloth::ResolvedType &rt_index   = mf_functions.program.expressions.at(index.index.get()).types.front();
	LLVMValueRef               ir_offset  = sl_scalars.checkedArrayIndex(ir_index, rt_index.unsignedInteger(), elements.length);
	const LLVMTypeRef          ty_element = mf_functions.valueTypes.localType(element);
	const LLVMValueRef         ir_cell    = LLVMBuildGEP2(ir_builder, ty_element, elements.address, &ir_offset, 1, "");
	return readValue({ .operand = ir_cell, .type = element, .storage = ValueStorage::ReadOnlySlot }, view);
}

std::optional<LoweredValue> FunctionLowering::writableArray (const cloth::Expression &array) {
	if (const auto *local = namedLocal(array); local && local->storage == ValueStorage::MutableSlot) return *local;
	if (const auto *field = dynamic_cast<const cloth::FieldAccessExpression *>(&array)) return lowerField(*field, AddressUse::Write);
	const auto &info = mf_functions.program.expressions.at(&array);
	if (ir_self && dynamic_cast<const cloth::NameExpression *>(&array) && info.symbol && info.symbol->classFieldIndex)
		return objectField(*info.symbol, { .handle = ir_self, .directSelf = true });
	return std::nullopt;
}

LLVMValueRef FunctionLowering::lowerArrayMutation (const cloth::MemberCallExpression &call, cloth::ArrayMutation operation) {
	const cloth::ExpressionInfo &ei_receiver = mf_functions.program.expressions.at(call.receiver.get());
	if (!ei_receiver.fallsThrough) return lowerExpression(*call.receiver);
	const cloth::ResolvedType &rt_array = ei_receiver.types.front();
	if (!growable::supported(rt_array))
		throw cde::ClothDiagnostic(Code::UnsupportedLowering, "LLVM lowering does not support this array mutation",
		                           { { .span = call.getSpan(), .text = "unsupported array mutation" } });
	const auto         lv_local = writableArray(*call.receiver);
	const LLVMValueRef ir_slot =
	        lv_local && lv_local->storage == ValueStorage::MutableSlot ?
	                (lv_local->type.nullable() ?
	                         detail::nullable::payloadAddress(ir_builder, mf_functions.valueTypes.localType(rt_array), lv_local->operand) :
	                         lv_local->operand) :
	                ownArray(rt_array, lowerExpression(*call.receiver), ArrayLifetimes::Kind::Temporary);
	const cloth::ResolvedType  rt_element = rt_array.element();
	const cloth::ResolvedType  rt_word(cloth::KUsize);
	const cloth::StorageLayout sl_cell    = arrayCellLayout(rt_array);
	const LLVMModuleRef        mod_module = mf_functions.module;
	const auto                 argument   = [&] (std::size_t index, const cloth::ResolvedType &type) {
		const LLVMValueRef value = lowerAs(*call.suppliedArguments[index].expression, type);
		if (value && index + 1 == call.suppliedArguments.size() && lv_local && lv_local->owner.handle &&
		    mf_functions.program.expressions.at(&call).receiverChecks.atAccess)
			objectData(lv_local->owner);
		return value;
	};
	switch (operation) {
	case cloth::ArrayMutation::Append:
		if (const LLVMValueRef ir_cell = argument(0, rt_element)) growable::append(ir_builder, mod_module, ir_slot, storeInEntrySlot(ir_cell), sl_cell);
		break;
	case cloth::ArrayMutation::Insert: {
		const LLVMValueRef ir_index = argument(0, rt_word);
		if (!ir_index) return nullptr;
		if (const LLVMValueRef ir_cell = argument(1, rt_element))
			growable::insert(ir_builder, mod_module, ir_slot, ir_index, storeInEntrySlot(ir_cell), sl_cell);
		break;
	}
	case cloth::ArrayMutation::RemoveAt:
		if (const LLVMValueRef ir_index = argument(0, rt_word))
			return growable::removeAt(ir_builder, mod_module, ir_slot, ir_index, mf_functions.valueTypes.localType(rt_element), sl_cell);
		break;
	case cloth::ArrayMutation::Clear:
		growable::clear(ir_builder, mod_module, ir_slot);
		break;
	case cloth::ArrayMutation::Reserve:
		if (const LLVMValueRef ir_minimum = argument(0, rt_word)) growable::reserve(ir_builder, mod_module, ir_slot, ir_minimum, sl_cell);
		break;
	}
	return nullptr;
}

LLVMValueRef FunctionLowering::lowerStructLiteral (const cloth::StructLiteral &literal, const cloth::ExpressionInfo &info) {
	const cloth::CheckedProgram &cp_program = mf_functions.program;
	if (!info.fallsThrough) return lowerUntilExit(literal.fields);
	const cloth::ResolvedType &rt_struct = info.types.front();
	const LLVMTypeRef          ty_struct = mf_functions.valueTypes.localType(rt_struct);
	if (!ty_struct) {
		throw cde::ClothDiagnostic(Code::UnsupportedLowering, "LLVM lowering requires a supported storage type for every struct field",
		                           { { .span = literal.getSpan(), .text = "unsupported struct literal" } });
	}
	const cloth::StructLayout &sl_layout = cp_program.structLayouts.at(rt_struct);
	LLVMValueRef               ir_struct = LLVMConstNull(ty_struct);
	for (std::size_t st_index = 0; st_index < literal.fields.size(); ++st_index) {
		const LLVMValueRef ir_field = lowerAs(*literal.fields[st_index], sl_layout.fields.at(st_index).type);
		ir_struct                   = LLVMBuildInsertValue(ir_builder, ir_struct, ir_field, static_cast<unsigned>(st_index), "");
	}
	return ir_struct;
}

std::optional<LoweredValue> FunctionLowering::lowerField (const cloth::FieldAccessExpression &field, std::optional<AddressUse> addressUse) {
	struct FieldStep {
		const cloth::ResolvedType &owner;
		LLVMTypeRef                storage;
		unsigned                   index;
		const cloth::ResolvedType &type;
	};

	const cloth::CheckedProgram &cp_program = mf_functions.program;
	const auto                  &info       = cp_program.expressions.at(&field);
	if (info.symbol && info.symbol->classFieldIndex) {
		if (addressUse == AddressUse::ReadOnly) return std::nullopt;
		const LLVMValueRef handle = lowerExpression(*field.receiver);
		if (!handle) return std::nullopt;
		return objectField(*info.symbol, { .handle = handle, .directSelf = cp_program.isDirectReceiver(*field.receiver) });
	}
	ObjectAccess                 object;
	const cloth::Expression     *e_root     = &field;
	std::vector<FieldStep>       v_path;
	while (const auto *fa_step = dynamic_cast<const cloth::FieldAccessExpression *>(e_root)) {
		e_root                                   = fa_step->receiver.get();
		const cloth::ExpressionInfo &ei_receiver = cp_program.expressions.at(e_root);
		if (!ei_receiver.fallsThrough) {
			if (!addressUse) lowerExpression(*e_root);
			return std::nullopt;
		}
		const cloth::Symbol *sym_field = cp_program.expressions.at(fa_step).symbol;
		const LLVMTypeRef ty_struct = ei_receiver.resultKind() == cloth::ResultKind::Single ? mf_functions.valueTypes.get(ei_receiver.types.front()) :
		                                                                                      nullptr;
		if (!sym_field || !std::holds_alternative<const cloth::StructFieldDeclaration *>(sym_field->declaration) || !ty_struct) {
			throw cde::ClothDiagnostic(Code::UnsupportedLowering, "LLVM lowering requires a struct field with a supported storage type",
			                           { { .span = fa_step->getSpan(), .text = "unsupported field access" } });
		}
		const cloth::StructLayout &sl_layout = cp_program.structLayouts.at(ei_receiver.types.front());
		const unsigned u_index = LLVMElementAtOffset(LLVMGetModuleDataLayout(mf_functions.module), ty_struct, sl_layout.fieldOffsets.at(sym_field));
		v_path.push_back({ .owner = ei_receiver.types.front(), .storage = ty_struct, .index = u_index, .type = sl_layout.fields[u_index].type });
		if (!ei_receiver.symbol || !std::holds_alternative<const cloth::StructFieldDeclaration *>(ei_receiver.symbol->declaration)) break;
	}

	const auto *ie_root = dynamic_cast<const cloth::IndexExpression *>(e_root);
	// A read may project a single field when its index leaves the receiver intact.
	LLVMValueRef                ir_operand = ie_root ? tryIndexAddress(*ie_root, addressUse.value_or(AddressUse::Read), &object) : nullptr;
	ValueStorage vs_storage = ValueStorage::Direct;
	bool         b_nullable = false;
	std::optional<LoweredValue> rootField;
	const auto                 &rootInfo = cp_program.expressions.at(e_root);
	if (rootInfo.symbol && rootInfo.symbol->classFieldIndex) {
		if (addressUse == AddressUse::ReadOnly) return std::nullopt;
		if (const auto *root = dynamic_cast<const cloth::FieldAccessExpression *>(e_root)) {
			if (auto storage = lowerField(*root, addressUse)) rootField.emplace(*storage);
		} else if (ir_self && dynamic_cast<const cloth::NameExpression *>(e_root)) {
			rootField.emplace(objectField(*rootInfo.symbol, { .handle = ir_self, .directSelf = true }));
		}
	}
	if (const LoweredValue *lv_root = rootField ? &*rootField : namedLocal(*e_root)) {
		object = lv_root->owner;
		if (addressUse && !permitsSlot(lv_root->storage, *addressUse)) return std::nullopt;
		vs_storage = lv_root->storage;
		ir_operand = lv_root->operand;
		b_nullable = lv_root->type.nullable();
	} else if (ir_operand) {
		vs_storage = addressUse == AddressUse::Write ? ValueStorage::MutableSlot : ValueStorage::ReadOnlySlot;
		b_nullable = cp_program.expressions.at(ie_root->array.get()).types.front().element().nullable();
	} else {
		if (addressUse) return std::nullopt;
		ir_operand = lowerExpression(*e_root);
	}
	const bool   b_address  = vs_storage != ValueStorage::Direct;
	LLVMValueRef ir_witness = nullptr;
	// A mutable root keeps the address of its subobject even when the right-hand side replaces a parent.
	for (const FieldStep &fs_step : std::views::reverse(v_path)) {
		// A presence proof exposes the payload inside the nullable receiver's storage.
		if (b_nullable)
			ir_operand = b_address ? detail::nullable::payloadAddress(ir_builder, fs_step.storage, ir_operand) :
			                         detail::nullable::payload(ir_builder, ir_operand);
		if (b_address && fs_step.type.nullable() && fs_step.type.element().array())
			ir_witness = mf_functions.valueTypes.arrayFieldWitness(ir_builder, fs_step.owner, ir_operand, fs_step.index);
		ir_operand = b_address ? LLVMBuildStructGEP2(ir_builder, fs_step.storage, ir_operand, fs_step.index, "") :
		                         LLVMBuildExtractValue(ir_builder, ir_operand, fs_step.index, "");
		b_nullable = fs_step.type.nullable();
	}
	const cloth::ResolvedType &rt_field = v_path.front().type;
	if (b_address && rt_field.array()) ir_operand = mf_functions.valueTypes.arrayData(ir_builder, rt_field, ir_operand);
	return LoweredValue{ .operand = ir_operand,
		             .type    = rt_field,
		             .storage = vs_storage,
		             .place   = ir_witness ? StoragePlace::Embedded : StoragePlace::Slot,
		             .witness = ir_witness,
		             .owner   = object };
}

LLVMValueRef FunctionLowering::lowerLogical (const cloth::BinaryExpression &binary, LLVMValueRef left) {
	const bool b_and = binary.op.getOperator() == cloth::OAnd;
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

LLVMValueRef FunctionLowering::lowerGuardedAccess (const cloth::GuardedAccessExpression &access, const cloth::ResolvedType *result) {
	const cloth::Expression     &e_receiver  = *access.getReceiver();
	const cloth::ExpressionInfo &ei_receiver = mf_functions.program.expressions.at(&e_receiver);
	LLVMValueRef                 ir_payload  = lowerExpression(e_receiver);
	if (!ei_receiver.fallsThrough) return ir_payload;
	const LLVMTypeRef ty_result = result ? mf_functions.valueTypes.localType(*result) : nullptr;
	if (result && !ty_result)
		throw cde::ClothDiagnostic(Code::UnsupportedLowering, "LLVM lowering requires a supported result type for guarded access",
		                           { { .span = access.getSpan(), .text = "unsupported guarded result" } });

	const LLVMBasicBlockRef bb_source = LLVMGetInsertBlock(ir_builder);
	LLVMBasicBlockRef       bb_end    = nullptr;
	if (ei_receiver.types.front().nullable()) {
		const LLVMBasicBlockRef bb_present = appendBlock("guard.present");
		bb_end                             = appendBlock("guard.end");
		LLVMBuildCondBr(ir_builder, sl_scalars.predicate(detail::nullable::presence(ir_builder, ir_payload)), bb_present, bb_end);
		LLVMPositionBuilderAtEnd(ir_builder, bb_present);
		ir_payload = detail::nullable::payload(ir_builder, ir_payload);
	}
	// The checked continuation reads this payload, including when it starts another guard.
	m_guardedPayloads.emplace(access.payload.get(), ir_payload);
	const LLVMValueRef ir_present = result ? lowerAs(*access.continuation, *result) : lowerExpression(*access.continuation);
	m_guardedPayloads.erase(access.payload.get());
	if (!bb_end) return ir_present;

	const LLVMBasicBlockRef bb_exit     = LLVMGetInsertBlock(ir_builder);
	const bool              b_continues = mf_functions.program.expressions.at(access.continuation.get()).fallsThrough;
	if (b_continues) LLVMBuildBr(ir_builder, bb_end);
	LLVMPositionBuilderAtEnd(ir_builder, bb_end);
	if (!ty_result) return nullptr;
	const LLVMValueRef ir_absent = LLVMConstNull(ty_result);
	if (!b_continues) return ir_absent;
	const LLVMValueRef ir_result  = LLVMBuildPhi(ir_builder, ty_result, "");
	LLVMValueRef       v_values[] = { ir_absent, ir_present };
	LLVMBasicBlockRef  v_blocks[] = { bb_source, bb_exit };
	LLVMAddIncoming(ir_result, v_values, v_blocks, 2);
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
		throw cde::ClothDiagnostic(Code::UnsupportedLowering, "LLVM range membership requires integer endpoints",
		                           { { .span = binary.getSpan(), .text = "unsupported range membership" } });

	// Equality permits either order. Evaluate the member on its written side of the range.
	LLVMValueRef       ir_value = b_rangeOnLeft ? nullptr : lowerExpression(*binary.left);
	const IntegerRange range    = lowerRange(b_rangeOnLeft ? *binary.left : *binary.right, rt_member);
	if (b_rangeOnLeft) ir_value = lowerExpression(*binary.right);
	const LLVMValueRef ir_lower  = sl_scalars.compare(cloth::OLessEqual, range.start, ir_value, rt_member);
	const LLVMValueRef ir_upper  = sl_scalars.withinRangeEnd(ir_value, range.end, range.inclusive, rt_member);
	const LLVMValueRef ir_member = LLVMBuildAnd(ir_builder, ir_lower, ir_upper, "");
	return sl_scalars.boolean(binary.op.getOperator() == cloth::ONotEqual ? LLVMBuildNot(ir_builder, ir_member, "") : ir_member);
}

LLVMValueRef FunctionLowering::lowerBinary (const cloth::BinaryExpression &binary, LLVMValueRef left) {
	const cloth::ExpressionFacts &ef_facts  = mf_functions.program.expressions;
	const cloth::ExpressionInfo  &ei_binary = ef_facts.at(&binary);
	const cloth::Operator         o_op      = binary.op.getOperator();
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
			LLVMValueRef ir_equal = nullptr;
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
			return sl_scalars.boolean(o_op == cloth::ONotEqual ? LLVMBuildNot(ir_builder, ir_equal, "") : ir_equal);
		}
		}
	}
	if (isBuiltinOperation(ei_binary) && !ei_binary.range) {
		if (o_op == cloth::OAnd || o_op == cloth::OOr) return lowerLogical(binary, left ? left : lowerExpression(*binary.left));
		const cloth::ExpressionInfo &ei_left = ef_facts.at(binary.left.get());
		if (!ei_binary.fallsThrough) {
			// The checker can record an exit before it selects an operand type for the operation.
			if (!left) lowerExpression(*binary.left);
			if (ei_left.fallsThrough) lowerExpression(*binary.right);
			return nullptr;
		}
		const cloth::ExpressionInfo &ei_right = ef_facts.at(binary.right.get());
		const auto                  &rt_left = ei_left.types.front(), &rt_right = ei_right.types.front();
		const cloth::ResolvedType    rt_null(cloth::KNull);
		const auto                   objectOrNull = [&] (const cloth::ResolvedType &type) {
			return type == rt_null || (type.nullable() ? type.element() : type).objectReference();
		};
		if (cloth::isEquality(o_op) && !ei_binary.callsClassEquality && objectOrNull(rt_left) && objectOrNull(rt_right)) {
			cloth::ResolvedType rt_handle = rt_left == rt_null ? rt_right : rt_left;
			if (!rt_handle.nullable() && (rt_left == rt_null || rt_right == rt_null || rt_right.nullable()))
				rt_handle.suffixes.emplace_back(cloth::toString(cloth::OQuestion));
			const LLVMValueRef ir_left   = handleOrZero(lowerAs(*binary.left, rt_handle, left), rt_handle);
			const LLVMValueRef ir_right  = handleOrZero(lowerAs(*binary.right, rt_handle), rt_handle);
			const bool         b_checked = o_op != cloth::OIdentity || ei_binary.checkedIdentity;
			const LLVMValueRef ir_equal  = equalObjects({ .handle = ir_left, .directSelf = mf_functions.program.isDirectReceiver(*binary.left) },
			                                            { .handle = ir_right, .directSelf = mf_functions.program.isDirectReceiver(*binary.right) },
			                                            rt_handle.nullable(),
			                                            b_checked && (ei_binary.receiverChecks.atAccess || rt_handle.nullable()));
			return sl_scalars.boolean(o_op == cloth::ONotEqual ? LLVMBuildNot(ir_builder, ir_equal, "") : ir_equal);
		}
		if ((o_op == cloth::OEqual || o_op == cloth::ONotEqual) && !ei_binary.callsElementEquality && !ei_binary.elementEquality &&
		    ei_left.resultKind() == cloth::ResultKind::Single && ei_right.resultKind() == cloth::ResultKind::Single) {
			if (const LLVMValueRef ir_equal = tryLowerArrayEquality(binary, left, ei_left.types.front(), ei_right.types.front()))
				return sl_scalars.boolean(o_op == cloth::OEqual ? ir_equal : LLVMBuildNot(ir_builder, ir_equal, ""));
		}
		if (ei_binary.comparisonType && ei_binary.comparisonType->nullable() && (o_op == cloth::OEqual || o_op == cloth::ONotEqual)) {
			const cloth::ResolvedType &rt_operand = *ei_binary.comparisonType;
			const cloth::ResolvedType  rt_null(cloth::KNull);
			const bool                 b_leftNull     = ei_left.types.front() == rt_null;
			const bool                 b_comparesNull = b_leftNull || ei_right.types.front() == rt_null;
			if ((b_comparesNull && mf_functions.valueTypes.localType(rt_operand)) || sl_scalars.type(rt_operand.element())) {
				const LLVMValueRef ir_left  = lowerAs(*binary.left, rt_operand, left);
				const LLVMValueRef ir_right = lowerAs(*binary.right, rt_operand);
				if (!b_comparesNull) return sl_scalars.binary(o_op, ir_left, ir_right, rt_operand);
				const LLVMValueRef ir_flag = detail::nullable::presence(ir_builder, b_leftNull ? ir_right : ir_left);
				return o_op == cloth::ONotEqual ? ir_flag : sl_scalars.boolean(LLVMBuildIsNull(ir_builder, ir_flag, ""));
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
	throw cde::ClothDiagnostic(Code::UnsupportedLowering,
	                           std::format("LLVM lowering does not support binary operator '{}' for these operands", binary.op.getLexeme()),
	                           { { .span = binary.getSpan(), .text = "unsupported binary expression" } });
}

LLVMValueRef FunctionLowering::objectData (ObjectAccess access) const {
	const LLVMTypeRef byte = LLVMInt8TypeInContext(LLVMGetModuleContext(mf_functions.module));
	return detail::objectCall(ir_builder, mf_functions.module, detail::ObjectOperation::Data,
	                          { access.handle, LLVMConstInt(byte, access.directSelf, false) });
}

LLVMValueRef FunctionLowering::handleOrZero (LLVMValueRef value, const cloth::ResolvedType &type) const {
	if (!type.nullable()) return value;
	const LLVMValueRef payload = detail::nullable::payload(ir_builder, value);
	return LLVMBuildSelect(ir_builder, LLVMBuildIsNotNull(ir_builder, detail::nullable::presence(ir_builder, value), ""), payload,
	                       LLVMConstNull(LLVMTypeOf(payload)), "");
}

void FunctionLowering::validateReference (LLVMValueRef value, const cloth::ResolvedType &type, bool directSelf) {
	if (type == cloth::ResolvedType(cloth::KNull)) return;
	if (!type.nullable()) {
		objectData({ .handle = value, .directSelf = directSelf });
		return;
	}
	const LLVMBasicBlockRef check = appendBlock("object.present"), done = appendBlock("object.checked");
	LLVMBuildCondBr(ir_builder, LLVMBuildIsNotNull(ir_builder, detail::nullable::presence(ir_builder, value), ""), check, done);
	LLVMPositionBuilderAtEnd(ir_builder, check);
	objectData({ .handle = detail::nullable::payload(ir_builder, value), .directSelf = directSelf });
	LLVMBuildBr(ir_builder, done);
	LLVMPositionBuilderAtEnd(ir_builder, done);
}

LLVMValueRef FunctionLowering::equalObjects (ObjectAccess left, ObjectAccess right, bool nullable, bool check) {
	if (check) {
		LLVMBasicBlockRef done = nullptr;
		if (nullable) {
			const LLVMBasicBlockRef present = appendBlock("objects.present");
			done                            = appendBlock("objects.compared");
			const LLVMValueRef both =
			        LLVMBuildAnd(ir_builder, LLVMBuildIsNotNull(ir_builder, left.handle, ""), LLVMBuildIsNotNull(ir_builder, right.handle, ""), "");
			LLVMBuildCondBr(ir_builder, both, present, done);
			LLVMPositionBuilderAtEnd(ir_builder, present);
		}
		objectData(left);
		objectData(right);
		if (done) {
			LLVMBuildBr(ir_builder, done);
			LLVMPositionBuilderAtEnd(ir_builder, done);
		}
	}
	return LLVMBuildICmp(ir_builder, LLVMIntEQ, left.handle, right.handle, "");
}

void FunctionLowering::validateWrite (ObjectAccess owner) const {
	if (owner.handle && rc_write.atAccess) objectData(owner);
}

LoweredValue FunctionLowering::objectField (const cloth::Symbol &field, ObjectAccess access) {
	const LLVMValueRef storage = objectData(access);
	return { .operand = mf_functions.objects.fieldAddress(ir_builder, storage, field),
		 .type    = mf_functions.objects.fieldType(field),
		 .storage = ValueStorage::MutableSlot,
		 .owner   = access };
}

void FunctionLowering::invokeConstructor (const cloth::ExpressionInfo &info, std::vector<LLVMValueRef> arguments, LLVMValueRef self) {
	arguments.insert(arguments.begin(), self);
	const LLVMValueRef constructor = mf_functions.declareFunction(*info.symbol);
	LLVMBuildCall2(ir_builder, LLVMGlobalGetValueType(constructor), constructor, arguments.data(), static_cast<unsigned>(arguments.size()), "");
}

LLVMValueRef FunctionLowering::lowerConstruction (const cloth::ClassCreationExpression &creation, const cloth::ExpressionInfo &info) {
	std::vector<LLVMValueRef> arguments = lowerArguments(creation.suppliedArguments, info);
	if (!info.fallsThrough) return nullptr;
	if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ir_builder))) return afterTerminatingDefault(info);
	const cloth::ResolvedType &type  = info.types.front();
	const auto                 table = mf_functions.program.dispatchTables.find(type);
	if (info.instance || info.contractInstance || (table != mf_functions.program.dispatchTables.end() && table->second.initialState))
		throw cde::ClothDiagnostic(Code::UnsupportedLowering, "Native construction requires a non-generic class without initial state dispatch",
		                           { { .span = creation.getSpan(), .text = "unsupported class construction" } });
	const LLVMValueRef self = mf_functions.objects.create(ir_builder, type, mf_functions);
	invokeConstructor(info, std::move(arguments), self);
	return self;
}

LLVMValueRef FunctionLowering::lowerObjectCall (const cloth::CallSite &call, const cloth::ExpressionInfo &info, const cloth::Expression *receiver) {
	const LLVMValueRef self = receiver ? lowerExpression(*receiver) : ir_self;
	if (!self) return nullptr;
	if (info.instance || info.contractInstance || (info.dispatch && info.dispatch->kind != cloth::DispatchKind::Direct))
		throw cde::ClothDiagnostic(Code::UnsupportedLowering, "Native member calls require a direct, non-generic class method",
		                           { { .span = call.getSpan(), .text = "unsupported method dispatch" } });
	const ObjectAccess access{ .handle = self, .directSelf = !receiver || mf_functions.program.isDirectReceiver(*receiver) };
	if (info.receiverChecks.beforeArguments) objectData(access);
	std::vector<LLVMValueRef> arguments = lowerArguments(call.suppliedArguments, info, self);
	if (!info.fallsThrough) return nullptr;
	if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ir_builder))) return afterTerminatingDefault(info);
	if (info.receiverChecks.atAccess) objectData(access);
	arguments.insert(arguments.begin(), self);
	const LLVMValueRef function = mf_functions.declareFunction(*info.symbol);
	const LLVMValueRef result =
	        LLVMBuildCall2(ir_builder, LLVMGlobalGetValueType(function), function, arguments.data(), static_cast<unsigned>(arguments.size()), "");
	return adoptResults(result, mf_functions.program.declarations.at(info.symbol).returns);
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
	if (sym_callee && sym_callee->kind == cloth::SymbolKind::Method) return lowerObjectCall(call, ei_call, nullptr);
	if (!sym_callee || sym_callee->kind != cloth::SymbolKind::Function || ei_call.instance || ei_call.dispatch) {
		throw cde::ClothDiagnostic(Code::UnsupportedLowering, "LLVM lowering requires a direct call to a non-generic free function",
		                           { { .span = call.getSpan(), .text = "unsupported call" } });
	}
	// Validate the callee's ABI before its arguments when evaluation can reach the call.
	const LLVMValueRef        fn_callee   = ei_call.fallsThrough && !hasTerminatingDefault(ei_call) ? mf_functions.declareFunction(*sym_callee) : nullptr;
	std::vector<LLVMValueRef> v_arguments = lowerArguments(call.suppliedArguments, ei_call);
	if (!ei_call.fallsThrough) return nullptr;
	if (!fn_callee) return afterTerminatingDefault(ei_call);
	const LLVMValueRef ir_result =
	        LLVMBuildCall2(ir_builder, LLVMGlobalGetValueType(fn_callee), fn_callee, v_arguments.data(), static_cast<unsigned>(v_arguments.size()), "");
	return adoptResults(ir_result, mf_functions.program.declarations.at(sym_callee).returns);
}

bool FunctionLowering::hasTerminatingDefault (const cloth::ExpressionInfo &info) const {
	if (info.omittedParameters.empty()) return false;
	const auto &contract          = *std::get<const cloth::CallableSignature *>((info.argumentContract ? info.argumentContract : info.symbol)->declaration);
	const cloth::BodyFacts &facts = info.contractInstance ? info.contractInstance->body : static_cast<const cloth::BodyFacts &>(mf_functions.program);
	return std::ranges::any_of(info.omittedParameters,
	                           [&] (std::size_t index) { return !facts.defaultEffects.at(&contract.parameters.at(index)).fallsThrough; });
}

LLVMValueRef FunctionLowering::afterTerminatingDefault (const cloth::ExpressionInfo &info) {
	// The body checker types the enclosing expression with the call's declared results.
	// Its nominal continuation has no predecessors after a default that always exits.
	LLVMPositionBuilderAtEnd(ir_builder, appendBlock("default.unreachable"));
	if (info.types.empty()) return nullptr;
	const LLVMTypeRef ty_result = mf_functions.resultType(info.types);
	if (!ty_result) throw cde::ClothDiagnostic(Code::UnsupportedFunctionAbi, "LLVM lowering requires representable call results");
	return LLVMGetUndef(ty_result);
}

std::vector<LLVMValueRef> FunctionLowering::lowerArguments (const std::vector<cloth::CallArgument> &supplied, const cloth::ExpressionInfo &info,
                                                            LLVMValueRef self) {
	const cloth::Symbol          &contract  = *(info.argumentContract ? info.argumentContract : info.symbol);
	const cloth::DeclarationInfo &signature = info.contractInstance ? info.contractInstance->signature : mf_functions.program.declarations.at(&contract);
	std::vector<LLVMValueRef>     v_arguments(signature.parameters.size());
	// Evaluate in source order, then pass each value in its checked parameter position.
	for (std::size_t st_index = 0; st_index < supplied.size(); ++st_index) {
		const std::size_t          st_parameter = info.parameterForArgument.at(st_index);
		const cloth::ResolvedType &type         = signature.parameters.at(st_parameter);
		const cloth::Expression   &argument     = *supplied[st_index].expression;
		if (mf_functions.program.expressions.at(&argument).fallsThrough && !mf_functions.valueTypes.localType(type))
			throw cde::ClothDiagnostic(Code::UnsupportedLowering, "LLVM lowering requires a supported argument type",
			                           { { .span = argument.getSpan(), .text = "unsupported argument type" } });
		const LLVMValueRef ir_argument = lowerAs(argument, type);
		if (!ir_argument) return v_arguments;
		v_arguments.at(st_parameter) = snapshot(type, ir_argument);
	}
	const auto &cs_contract = *std::get<const cloth::CallableSignature *>(contract.declaration);
	for (const std::size_t st_index : info.omittedParameters) {
		const LLVMValueRef        fn_default = mf_functions.declareDefault(contract, st_index);
		std::vector<LLVMValueRef> prefix(v_arguments.begin(), v_arguments.begin() + static_cast<std::ptrdiff_t>(st_index));
		if (detail::hasObjectReceiver(contract, true)) prefix.insert(prefix.begin(), self);
		const LLVMValueRef ir_default =
		        LLVMBuildCall2(ir_builder, LLVMGlobalGetValueType(fn_default), fn_default, prefix.data(), static_cast<unsigned>(prefix.size()), "");
		if (!mf_functions.program.defaultEffects.at(&cs_contract.parameters.at(st_index)).fallsThrough) {
			LLVMBuildUnreachable(ir_builder);
			break;
		}
		v_arguments.at(st_index) = adoptResults(ir_default, ResultView(signature.parameters).subspan(st_index, 1));
	}
	return v_arguments;
}

LLVMValueRef FunctionLowering::lowerPrintln (const cloth::CallExpression &call) {
	if (call.suppliedArguments.size() == 1) {
		const cloth::Expression &e_argument = *call.suppliedArguments.front().expression;
		const cloth::BodyFacts  &bf_facts   = mf_functions.program;
		if (!bf_facts.expressions.at(&e_argument).fallsThrough) return lowerExpression(e_argument);

		const cloth::ProviderSelection &ps_text = bf_facts.textConversions.at(&e_argument);
		if (ps_text.kind == cloth::ProviderKind::Format && !ps_text.callsFormatter && ps_text.receiver == cloth::ResolvedType(cloth::KInteger)) {
			LLVMValueRef       ir_argument = lowerExpression(e_argument);
			const LLVMValueRef fn_println  = detail::declarePrintlnInt32(mf_functions.module);
			return LLVMBuildCall2(ir_builder, LLVMGlobalGetValueType(fn_println), fn_println, &ir_argument, 1, "");
		}
	}
	throw cde::ClothDiagnostic(Code::UnsupportedLowering,
	                           std::format("LLVM lowering supports '{}' with one signed 32-bit {} argument", call.name.getLexeme(),
	                                       cloth::toString(cloth::KInteger)),
	                           { { .span = call.getSpan(), .text = "unsupported output call" } });
}

LLVMValueRef FunctionLowering::lowerExit (const cloth::CallExpression &call, const cloth::BuiltinCall &builtin) {
	LLVMValueRef ir_status = lowerAs(*call.suppliedArguments.front().expression, builtin.parameters.front());
	if (!ir_status) return nullptr;
	if (call.suppliedArguments.size() > 1) {
		const LLVMValueRef ir_message = lowerAs(*call.suppliedArguments[1].expression, builtin.parameters[1]);
		if (!ir_message) return nullptr;
		const detail::string_value::Bytes by_text       = detail::string_value::bytes(ir_builder, ir_message);
		LLVMValueRef                      v_arguments[] = { ir_status, by_text.data, by_text.length };
		const LLVMValueRef                fn_exit       = detail::declareExitMessage(mf_functions.module);
		LLVMBuildCall2(ir_builder, LLVMGlobalGetValueType(fn_exit), fn_exit, v_arguments, 3, "");
	} else {
		const LLVMValueRef fn_exit = detail::declareExit(mf_functions.module);
		LLVMBuildCall2(ir_builder, LLVMGlobalGetValueType(fn_exit), fn_exit, &ir_status, 1, "");
	}
	LLVMBuildUnreachable(ir_builder);
	return nullptr;
}

LLVMValueRef FunctionLowering::storeInEntrySlot (LLVMValueRef value) {
	const LLVMValueRef ir_slot = detail::allocateEntrySlot(ir_builder, LLVMTypeOf(value));
	LLVMBuildStore(ir_builder, value, ir_slot);
	return ir_slot;
}

const LoweredValue &FunctionLowering::bindLocal (const cloth::Token &name, const cloth::ResolvedType &type, LLVMValueRef value, bool isMutable) {
	const ValueStorage vs_storage = isMutable                            ? ValueStorage::MutableSlot :
	                                cloth::semantic::hasArrayShape(type) ? ValueStorage::ReadOnlySlot :
	                                                                       ValueStorage::Direct;
	if (growable::ownsBuffer(type))
		value = ownArray(type, value, ArrayLifetimes::Kind::Local);
	else if (vs_storage != ValueStorage::Direct)
		value = mf_functions.valueTypes.allocateLocal(ir_builder, type, value);
	// A deferred block can bind the same declaration at several read sites.
	m_locals.erase(&name);
	return m_locals.emplace(&name, LoweredValue{ .operand = value, .type = type, .storage = vs_storage }).first->second;
}

void FunctionLowering::lowerBinding (const cloth::BindingDeclaration &binding) {
	const cloth::BodyFacts &bf_facts = mf_functions.program;
	if (bf_facts.deferredBindings.contains(&binding.name)) return;
	if (!bf_facts.controlFlow.at(&binding).fallsThrough) {
		// An inferred local has no type when the initializer leaves before producing a value.
		lowerExpression(*binding.initializer);
		return;
	}
	const auto it_type = bf_facts.localTypes.find(&binding.name);
	if (it_type == bf_facts.localTypes.end() || !mf_functions.valueTypes.localType(it_type->second)) {
		throw cde::ClothDiagnostic(
		        Code::UnsupportedLowering, "LLVM lowering requires a supported storage type for this local",
		        { { .span = binding.type ? binding.type->getSpan() : binding.initializer->getSpan(), .text = "unsupported local type" } });
	}
	if (const LLVMValueRef ir_value = lowerAs(*binding.initializer, it_type->second)) {
		bindLocal(binding.name, it_type->second, ir_value, binding.isMutable);
	}
}

LoweredValue FunctionLowering::mutableTarget (const cloth::Statement &write, const cloth::Token &name) {
	const cloth::BodyFacts &bf_facts  = mf_functions.program;
	const auto              it_target = bf_facts.references.find(&name);
	const LoweredValue     *lv_target = it_target == bf_facts.references.end() ? nullptr : findLocal(it_target->second);
	if (!lv_target && ir_self && it_target != bf_facts.references.end() && !bf_facts.writes.contains(&write)) {
		if (const auto *field = mf_functions.objects.field(it_target->second)) return objectField(*field, { .handle = ir_self, .directSelf = true });
	}
	if (!lv_target || lv_target->storage != ValueStorage::MutableSlot || bf_facts.writes.contains(&write)) {
		throw cde::ClothDiagnostic(Code::UnsupportedLowering, "LLVM lowering requires a built-in update on a mutable local",
		                           { { .span = name.getSpan(), .text = "unsupported assignment target" } });
	}
	return *lv_target;
}

void FunctionLowering::lowerAssignment (const cloth::AssignmentStatement &assignment) {
	// The checker records a compound assignment's read and arithmetic on its synthetic binary expression.
	storeExpression(mutableTarget(assignment, assignment.name), *assignment.value);
}

void FunctionLowering::lowerElementAssignment (const cloth::IndexExpression &target, cloth::Operator op, const cloth::Expression *value,
                                               const cloth::BinaryExpression *compound) {
	const cloth::ExpressionInfo &ei_array = mf_functions.program.expressions.at(target.array.get());
	if (!ei_array.fallsThrough) {
		lowerExpression(*target.array);
		return;
	}
	const cloth::ResolvedType &rt_array  = ei_array.types.front();
	if (growable::supported(rt_array)) {
		const auto lv_array = writableArray(*target.array);
		if (!lv_array || lv_array->storage != ValueStorage::MutableSlot)
			throw cde::ClothDiagnostic(Code::UnsupportedLowering, "LLVM lowering requires a writable growable array local",
			                           { { .span = target.getSpan(), .text = "unsupported array assignment target" } });
		return lowerGrowableElementAssignment(*lv_array, target, op, value, compound);
	}
	ObjectAccess       owner;
	const LLVMValueRef ir_buffer = rt_array.array() ? tryArrayAddress(*target.array, AddressUse::Write, &owner) : nullptr;
	if (!ir_buffer)
		throw cde::ClothDiagnostic(Code::UnsupportedLowering, "LLVM lowering requires a mutable fixed-size array for this assignment",
		                           { { .span = target.getSpan(), .text = "unsupported array assignment target" } });
	const LLVMTypeRef  ty_array  = mf_functions.valueTypes.slotType(rt_array);
	const LLVMValueRef ir_offset = lowerArrayOffset(*target.index, ty_array);
	if (!ir_offset) return;
	if (owner.handle) objectData(owner);
	const LLVMValueRef        ir_address = arrayElementAddress(ty_array, ir_buffer, ir_offset);
	const cloth::ResolvedType rt_element = rt_array.element();
	const LoweredValue        lv_element{
		.operand = ir_address, .type = rt_element, .storage = ValueStorage::MutableSlot, .place = detail::arrayElementPlace(rt_element), .owner = owner
	};
	if (compound) return storeCompound(lv_element, *compound);
	if (value) return storeExpression(lv_element, *value);
	lowerUpdate(lv_element, target, op);
}

void FunctionLowering::lowerGrowableElementAssignment (const LoweredValue &array, const cloth::IndexExpression &target, cloth::Operator op,
                                                       const cloth::Expression *value, const cloth::BinaryExpression *compound) {
	const cloth::ResolvedType rt_array = array.type.nullable() ? array.type.element() : array.type;
	const LLVMValueRef ir_index = lowerExpression(*target.index);
	if (!ir_index) return;
	if (array.owner.handle) objectData(array.owner);
	const cloth::ResolvedType &rt_index   = mf_functions.program.expressions.at(target.index.get()).types.front();
	const cloth::ResolvedType  rt_element = rt_array.element();
	const LLVMTypeRef          ty_element = mf_functions.valueTypes.localType(rt_element);
	const growable::Elements   before     = growable::elements(ir_builder, readValue(array, rt_array));
	LLVMValueRef               ir_offset  = sl_scalars.checkedArrayIndex(ir_index, rt_index.unsignedInteger(), before.length);
	const auto cellAt = [&] (const growable::Elements &elements) { return LLVMBuildGEP2(ir_builder, ty_element, elements.data, &ir_offset, 1, ""); };
	if (!compound && !value) {
		lowerUpdate({ .operand = cellAt(before), .type = rt_element, .storage = ValueStorage::MutableSlot }, target, op);
		return;
	}
	LLVMValueRef ir_new = nullptr;
	if (compound) {
		const LLVMValueRef ir_old = readValue({ .operand = cellAt(before), .type = rt_element, .storage = ValueStorage::ReadOnlySlot },
		                                      mf_functions.program.expressions.at(compound->left.get()).types.front());
		ir_new                    = lowerBinary(*compound, ir_old);
		if (ir_new) ir_new = convertValue(ir_new, mf_functions.program.expressions.at(compound).types.front(), rt_element);
	} else {
		ir_new = lowerAs(*value, rt_element);
	}
	if (!ir_new) return;
	if (array.owner.handle) objectData(array.owner);
	// The RHS can replace or reallocate the buffer. Keep the coordinate, then check it against the new length.
	const growable::Elements after = growable::elements(ir_builder, readValue(array, rt_array));
	sl_scalars.checkedArrayIndex(ir_offset, true, after.length);
	LLVMBuildStore(ir_builder, ir_new, cellAt(after));
}

void FunctionLowering::lowerFieldWrite (const cloth::FieldWriteStatement &write) {
	if (!mf_functions.program.writes.contains(&write)) {
		if (const auto *ie_index = dynamic_cast<const cloth::IndexExpression *>(write.target.get()))
			return lowerElementAssignment(*ie_index, write.op.getOperator(), write.value.get(), write.compound.get());
		if (const auto *fa_field = dynamic_cast<const cloth::FieldAccessExpression *>(write.target.get())) {
			if (!mf_functions.program.expressions.at(fa_field->receiver.get()).fallsThrough) {
				lowerExpression(*fa_field->receiver);
				return;
			}
			if (const std::optional<LoweredValue> opt_field = lowerField(*fa_field, AddressUse::Write)) {
				if (write.compound) return storeCompound(*opt_field, *write.compound);
				if (write.value) return storeExpression(*opt_field, *write.value);
				return lowerUpdate(*opt_field, *write.target, write.op.getOperator());
			}
		}
	}
	throw cde::ClothDiagnostic(Code::UnsupportedLowering, "LLVM lowering requires a built-in write to a mutable local struct field",
	                           { { .span = write.target->getSpan(), .text = "unsupported field assignment target" } });
}

void FunctionLowering::storeExpression (const LoweredValue &target, const cloth::Expression &value) {
	if (const LLVMValueRef ir_value = lowerAs(value, target.type)) {
		validateWrite(target.owner);
		if (growable::ownsBuffer(target.type))
			assignArray(target.operand, target.type, ir_value);
		else
			mf_functions.valueTypes.storeLocal(ir_builder, target.type, target.operand, ir_value, target.place, target.witness);
	}
}

void FunctionLowering::storeCompound (const LoweredValue &target, const cloth::BinaryExpression &compound) {
	// Capture the element or field before the right operand can change the target.
	const cloth::ExpressionFacts &ef_facts = mf_functions.program.expressions;
	const LLVMValueRef            ir_old   = readValue(target, ef_facts.at(compound.left.get()).types.front());
	if (const LLVMValueRef ir_new = lowerBinary(compound, ir_old)) {
		validateWrite(target.owner);
		mf_functions.valueTypes.storeLocal(ir_builder, target.type, target.operand,
		                                   convertValue(ir_new, ef_facts.at(&compound).types.front(), target.type), target.place, target.witness);
	}
}

void FunctionLowering::lowerUpdate (const LoweredValue &target, const cloth::Expression &read, cloth::Operator op) {
	const cloth::ResolvedType &rt_read  = mf_functions.program.expressions.at(&read).types.front();
	const LLVMValueRef         ir_old   = readValue(target, rt_read);
	const LLVMTypeRef          ty_value = LLVMTypeOf(ir_old);
	const LLVMValueRef         ir_one   = rt_read.floating() ? LLVMConstReal(ty_value, 1.) : LLVMConstInt(ty_value, 1, false);
	const cloth::Operator      o_op     = op == cloth::OPlusPlus ? cloth::OPlus : cloth::OMinus;
	const LLVMValueRef         ir_new   = sl_scalars.binary(o_op, ir_old, ir_one, rt_read);
	mf_functions.valueTypes.storeLocal(ir_builder, target.type, target.operand, convertValue(ir_new, rt_read, target.type), target.place, target.witness);
}

LLVMValueRef FunctionLowering::snapshot (const cloth::ResolvedType &type, LLVMValueRef value) {
	if (!growable::ownsBuffer(type)) return value;
	const LLVMValueRef ir_slot = ownArray(type, value, ArrayLifetimes::Kind::Temporary);
	return LLVMBuildLoad2(ir_builder, LLVMTypeOf(value), ir_slot, "");
}

LLVMValueRef FunctionLowering::adoptResults (LLVMValueRef value, ResultView types) {
	// The callee transferred each result buffer. Register it even when the caller discards the result.
	for (std::size_t st_slot = 0; st_slot < types.size(); ++st_slot) {
		if (!growable::ownsBuffer(types[st_slot])) continue;
		const LLVMValueRef ir_buffer = detail::selectResult(ir_builder, value, types.size(), st_slot);
		const LLVMValueRef ir_owner  = allocateArrayOwner(types[st_slot], ArrayLifetimes::Kind::Temporary);
		LLVMBuildStore(ir_builder, ir_buffer, ir_owner);
	}
	return value;
}

std::vector<LLVMValueRef> FunctionLowering::lowerValues (const cloth::Expression &expression, ResultView destinations) {
	const cloth::ExpressionInfo &info      = mf_functions.program.expressions.at(&expression);
	const LLVMValueRef           ir_result = lowerExpression(expression);
	std::vector<LLVMValueRef>    v_results;
	if (!info.fallsThrough) return v_results;
	v_results.reserve(destinations.size());
	for (std::size_t st_index = 0; st_index < destinations.size(); ++st_index) {
		const std::size_t  st_source = info.types.size() == 1 ? 0 : st_index;
		const LLVMValueRef ir_slot   = detail::selectResult(ir_builder, ir_result, info.types.size(), st_source);
		v_results.push_back(convertValue(ir_slot, info.types.at(st_source), destinations[st_index]));
	}
	return v_results;
}

ResultExit FunctionLowering::makeResultExit (ResultView types, LLVMBasicBlockRef block) {
	ResultExit exit{ .types = types, .block = block, .phi = nullptr, .buffers = {}, .scope = {} };
	if (!types.empty()) {
		const LLVMTypeRef ty_result = mf_functions.resultType(types);
		if (!ty_result) throw cde::ClothDiagnostic(Code::UnsupportedLowering, "LLVM lowering requires representable result types");
		exit.buffers.reserve(types.size());
		for (const cloth::ResolvedType &type : types) {
			exit.buffers.push_back(growable::ownsBuffer(type) ? allocateArrayOwner(type, ArrayLifetimes::Kind::Temporary) : nullptr);
		}
		const LLVMBasicBlockRef bb_site = LLVMGetInsertBlock(ir_builder);
		LLVMPositionBuilderAtEnd(ir_builder, block);
		exit.phi = LLVMBuildPhi(ir_builder, ty_result, "");
		LLVMPositionBuilderAtEnd(ir_builder, bb_site);
	}
	exit.scope = al_arrays.mark();
	return exit;
}

void FunctionLowering::yieldResults (ResultExit &exit, std::span<LLVMValueRef> values) {
	for (std::size_t st_index = 0; st_index < values.size(); ++st_index) {
		if (const LLVMValueRef ir_owner = exit.buffers[st_index]) {
			assignArray(ir_owner, exit.types[st_index], values[st_index]);
			values[st_index] = LLVMBuildLoad2(ir_builder, LLVMTypeOf(values[st_index]), ir_owner, "");
		}
	}
	LLVMValueRef ir_result = detail::packResults(ir_builder, values);
	al_arrays.releaseSince(exit.scope);
	LLVMBasicBlockRef bb_from = LLVMGetInsertBlock(ir_builder);
	LLVMBuildBr(ir_builder, exit.block);
	if (exit.phi) LLVMAddIncoming(exit.phi, &ir_result, &bb_from, 1);
}

LLVMValueRef FunctionLowering::lowerDeferred (const cloth::DeferredBinding &binding, const cloth::ResolvedType &view) {
	const cloth::FunctionBlockExpression &block    = *binding.block;
	const ResultView                      types    = mf_functions.program.expressions.at(&block).types;
	ResultExit                            exit     = makeResultExit(types, appendBlock("deferred.end"));
	ResultExit *const                     re_outer = re_deferred;
	re_deferred                                    = &exit;
	lowerStatements(std::get<cloth::BlockBody>(block.declaration.body.value).statements);
	re_deferred = re_outer;
	LLVMPositionBuilderAtEnd(ir_builder, exit.block);
	const LLVMValueRef         ir_result = detail::selectResult(ir_builder, exit.phi, types.size(), binding.resultIndex);
	const cloth::ResolvedType &type      = types[binding.resultIndex];
	return type.nullable() && !view.nullable() ? detail::nullable::payload(ir_builder, ir_result) : ir_result;
}

void FunctionLowering::lowerReturn (const cloth::Expression *expression) {
	const ResultView          types = re_deferred ? re_deferred->types : rv_returns;
	std::vector<LLVMValueRef> v_results;
	if (expression) {
		v_results = lowerValues(*expression, types);
		if (!mf_functions.program.expressions.at(expression).fallsThrough) return;
	}
	if (re_deferred) return yieldResults(*re_deferred, v_results);
	for (std::size_t st_index = 0; st_index < types.size(); ++st_index) {
		if (!growable::ownsBuffer(types[st_index])) continue;
		// Detach an independent buffer before releasing the callee's owners.
		const LLVMTypeRef  ty_value = LLVMTypeOf(v_results[st_index]);
		const LLVMValueRef ir_owner = detail::allocateEntrySlot(ir_builder, ty_value, LLVMConstNull(ty_value));
		assignArray(ir_owner, types[st_index], v_results[st_index]);
		v_results[st_index] = LLVMBuildLoad2(ir_builder, ty_value, ir_owner, "");
	}
	al_arrays.releaseSince({});
	if (const LLVMValueRef ir_result = detail::packResults(ir_builder, v_results))
		LLVMBuildRet(ir_builder, ir_result);
	else
		LLVMBuildRetVoid(ir_builder);
}

LLVMValueRef FunctionLowering::lowerIf (const cloth::IfExpression &conditional, const cloth::ExpressionInfo &info) {
	const LLVMBasicBlockRef bb_end = info.fallsThrough ? appendBlock("if.end") : nullptr;
	if (!info.types.empty() && !mf_functions.resultType(info.types))
		throw cde::ClothDiagnostic(Code::UnsupportedLowering, "LLVM lowering requires representable conditional results",
		                           { { .span = conditional.getSpan(), .text = "unsupported conditional result" } });
	ResultExit exit     = makeResultExit(info.types, bb_end);
	const auto lowerArm = [&] (const std::vector<cloth::StatementPtr> &statements) {
		lowerStatements(statements, info.types.empty() ? nullptr : &exit);
		if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ir_builder))) branchOrUnreachable(bb_end);
	};
	for (const cloth::ConditionalBranch &cb_branch : conditional.branches) {
		const ArrayLifetimes::Mark condition    = al_arrays.mark();
		const LLVMValueRef ir_condition = lowerExpression(*cb_branch.condition);
		al_arrays.endStatement(condition);
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
	return exit.phi;
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
	const cloth::LoopTarget *lt_target = loop.getLoopTarget().get();
	blocks.owners                      = al_arrays.mark();
	m_loopBlocks.emplace(lt_target, blocks);
	lowerStatements(body.statements);
	m_loopBlocks.erase(lt_target);
	if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ir_builder))) branchOrUnreachable(blocks.continuation);
}

void FunctionLowering::lowerWhile (const cloth::WhileStatement &loop) {
	const LLVMBasicBlockRef bb_test      = branchToNewBlock("while.cond");
	const ArrayLifetimes::Mark condition    = al_arrays.mark();
	const LLVMValueRef      ir_condition = lowerExpression(*loop.condition);
	al_arrays.endStatement(condition);
	if (!ir_condition) return;

	const LLVMBasicBlockRef bb_body = appendBlock("while.body");
	const LoopBlocks        lb_blocks{ .continuation = bb_test, .exit = loopExit(loop) };
	branchOnLoopCondition(sl_scalars.predicate(ir_condition), bb_body, lb_blocks.exit);
	LLVMPositionBuilderAtEnd(ir_builder, bb_body);
	lowerLoopBody(loop, loop.body, lb_blocks);
	if (lb_blocks.exit) LLVMPositionBuilderAtEnd(ir_builder, lb_blocks.exit);
}

LLVMBasicBlockRef FunctionLowering::loopContinuation (const cloth::LoopStatement &loop, const cloth::LoopBody &body, const char *name) const {
	const cloth::LoopTarget *lt_target = loop.getLoopTarget().get();
	for (const cloth::StatementPtr &sp_statement : body.statements) {
		const cloth::ControlFlow &cf_flow = mf_functions.program.controlFlow.at(sp_statement.get());
		if (cf_flow.continues.contains(lt_target)) break;
		if (!cf_flow.fallsThrough) return nullptr;
	}
	return appendBlock(name);
}

void FunctionLowering::lowerDoWhile (const cloth::DoWhileStatement &loop) {
	const LLVMBasicBlockRef bb_body = branchToNewBlock("do.body");
	const LoopBlocks        lb_blocks{ .continuation = loopContinuation(loop, loop.body, "do.cond"), .exit = loopExit(loop) };
	lowerLoopBody(loop, loop.body, lb_blocks);
	if (lb_blocks.continuation) {
		LLVMPositionBuilderAtEnd(ir_builder, lb_blocks.continuation);
		const ArrayLifetimes::Mark condition    = al_arrays.mark();
		const LLVMValueRef         ir_condition = lowerExpression(*loop.condition);
		al_arrays.endStatement(condition);
		if (ir_condition) branchOnLoopCondition(sl_scalars.predicate(ir_condition), bb_body, lb_blocks.exit);
	}
	if (lb_blocks.exit) LLVMPositionBuilderAtEnd(ir_builder, lb_blocks.exit);
}

void FunctionLowering::lowerFor (const cloth::ForStatement &loop) {
	const cloth::BodyFacts      &bf_facts  = mf_functions.program;
	const cloth::Expression     &e_source  = loop.source.getExpression();
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
			throw cde::ClothDiagnostic(Code::UnsupportedLowering, "LLVM range iteration requires a built-in integer range",
			                           { { .span = loop.source.getSpan(), .text = "unsupported range loop" } });
		return lowerIntegerRange(loop, lowerRange(e_source, it_loop.element), it_loop.element);
	}
	const cloth::ResolvedType &rt_source  = ei_source.types.front();
	const bool                 b_growable = growable::supported(rt_source);
	const LLVMTypeRef ty_source = it_loop.kind == cloth::IterationKind::Array && !loop.stepper ? mf_functions.valueTypes.localType(rt_source) : nullptr;
	if (!ty_source || (!b_growable && LLVMGetTypeKind(ty_source) != LLVMArrayTypeKind))
		throw cde::ClothDiagnostic(Code::UnsupportedLowering, "LLVM iteration requires an array with supported element storage",
		                           { { .span = loop.source.getSpan(), .text = "unsupported iteration source" } });
	if (!b_growable && !LLVMGetArrayLength2(ty_source)) {
		lowerExpression(e_source);
		return;
	}
	const cloth::ResolvedType &rt_binding = bf_facts.localTypes.at(&loop.binding.name);
	if (!mf_functions.valueTypes.localType(rt_binding))
		throw cde::ClothDiagnostic(Code::UnsupportedLowering, "LLVM lowering requires a supported storage type for this loop binding",
		                           { { .span = loop.binding.getSpan(), .text = "unsupported loop binding type" } });

	// Capture the input before the body can modify it. A read-only local already holds an independent buffer.
	const ArrayBuffer       ab_source = lowerArrayBuffer(e_source, AddressUse::ReadOnly);
	LLVMBasicBlockRef       bb_start  = LLVMGetInsertBlock(ir_builder);
	const LLVMBasicBlockRef bb_body   = appendBlock("for.body");
	LoopBlocks              lb_blocks{ .continuation = loopContinuation(loop, loop.body, "for.next"), .exit = loopExit(loop) };
	if (b_growable)
		branchOnLoopCondition(LLVMBuildIsNotNull(ir_builder, ab_source.length, ""), bb_body, lb_blocks.exit);
	else
		LLVMBuildBr(ir_builder, bb_body);
	LLVMPositionBuilderAtEnd(ir_builder, bb_body);
	const LLVMTypeRef       ty_index = LLVMTypeOf(ab_source.length);
	const LLVMValueRef      ir_index = LLVMBuildPhi(ir_builder, ty_index, "");
	LLVMValueRef            ir_zero  = LLVMConstNull(ty_index);
	LLVMAddIncoming(ir_index, &ir_zero, &bb_start, 1);
	const LLVMTypeRef  ty_array = b_growable ? LLVMArrayType2(mf_functions.valueTypes.localType(it_loop.element), 0) : ty_source;
	const LLVMValueRef ir_element =
	        LLVMBuildLoad2(ir_builder, LLVMGetElementType(ty_array), arrayElementAddress(ty_array, ab_source.address, ir_index), "");
	bindLocal(loop.binding.name, rt_binding, convertValue(ir_element, it_loop.element, rt_binding), true);
	lowerLoopBody(loop, loop.body, lb_blocks);
	if (lb_blocks.continuation) {
		LLVMPositionBuilderAtEnd(ir_builder, lb_blocks.continuation);
		LLVMValueRef ir_next = LLVMBuildAdd(ir_builder, ir_index, LLVMConstInt(ty_index, 1, false), "");
		LLVMAddIncoming(ir_index, &ir_next, &lb_blocks.continuation, 1);
		const LLVMValueRef ir_more = LLVMBuildICmp(ir_builder, LLVMIntULT, ir_next, ab_source.length, "");
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
	return { .start = ir_start, .end = ir_end, .inclusive = LLVMConstInt(ty_flag, range.op.getOperator() == cloth::ORangeInclusive, false) };
}

void FunctionLowering::lowerIntegerRange (const cloth::ForStatement &loop, IntegerRange range, const cloth::ResolvedType &type) {
	const cloth::Operator          o_step    = loop.stepper ? loop.stepper->op.getOperator() : cloth::OPlusPlus;
	const cloth::BinaryExpression *be_update = loop.stepper ? loop.stepper->compound.get() : nullptr;
	if (loop.stepper) {
		bool b_supported = o_step == cloth::OAssign || o_step == cloth::OPlusPlus || o_step == cloth::OMinusMinus;
		if (be_update) {
			const cloth::ExpressionInfo &ei_update = mf_functions.program.expressions.at(be_update);
			b_supported                            = isBuiltinOperation(ei_update) &&
			              (!ei_update.fallsThrough || (ei_update.resultKind() == cloth::ResultKind::Single && ei_update.types.front() == type));
		}
		if (!b_supported || mf_functions.program.references.at(&loop.stepper->target) != &loop.binding.name)
			throw cde::ClothDiagnostic(Code::UnsupportedLowering,
			                           "LLVM integer range steps support assignment, postfix updates and built-in compound assignments",
			                           { { .span = loop.stepper->span, .text = "unsupported range step" } });
	}

	LLVMValueRef ir_lower = range.start, ir_upper = range.end;
	if (loop.stepper) {
		// An explicit step can traverse bounds written in either order. The written end controls exclusion.
		const LLVMValueRef ir_ascending = sl_scalars.compare(cloth::OLessEqual, range.start, range.end, type);
		ir_lower                        = LLVMBuildSelect(ir_builder, ir_ascending, range.start, range.end, "");
		ir_upper                        = LLVMBuildSelect(ir_builder, ir_ascending, range.end, range.start, "");
	}
	const LLVMValueRef ir_nonempty = sl_scalars.withinRangeEnd(ir_lower, ir_upper, range.inclusive, type);
	if (LLVMIsNull(ir_nonempty)) return;

	LLVMBasicBlockRef       bb_entry = LLVMGetInsertBlock(ir_builder);
	const LLVMBasicBlockRef bb_body  = appendBlock("range.body");
	LoopBlocks const        lb_blocks{ .continuation = loopContinuation(loop, loop.body, "range.next"), .exit = loopExit(loop) };
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
		const ArrayLifetimes::Mark step       = al_arrays.mark();
		const LLVMValueRef       ir_amount  = e_value ? lowerAs(*e_value, type) : LLVMConstInt(LLVMTypeOf(ir_current), 1, false);
		al_arrays.endStatement(step);
		if (ir_amount) {
			const cloth::Operator o_operation = be_update ? be_update->op.getOperator() : o_step;
			auto [ir_next, ir_fits]           = sl_scalars.integerRangeStep(o_operation, ir_current, ir_amount, type);
			const LLVMValueRef ir_lowerTest   = sl_scalars.compare(cloth::OLessEqual, ir_lower, ir_next, type);
			const LLVMValueRef ir_upperTest   = loop.stepper ? sl_scalars.compare(cloth::OLessEqual, ir_next, ir_upper, type) :
			                                                   sl_scalars.withinRangeEnd(ir_next, ir_upper, range.inclusive, type);
			LLVMValueRef       ir_inRange     = LLVMBuildAnd(ir_builder, ir_lowerTest, ir_upperTest, "");
			if (loop.stepper) {
				const LLVMValueRef ir_notEnd = sl_scalars.compare(cloth::ONotEqual, ir_next, range.end, type);
				ir_inRange = LLVMBuildAnd(ir_builder, ir_inRange, LLVMBuildOr(ir_builder, range.inclusive, ir_notEnd, ""), "");
			}
			const LLVMValueRef ir_repeat = LLVMBuildAnd(ir_builder, ir_fits, ir_inRange, "");
			if (e_value) {
				const bool         b_additive   = o_operation == cloth::OPlus || o_operation == cloth::OMinus;
				const LLVMValueRef ir_unchanged = b_additive ? LLVMBuildIsNull(ir_builder, ir_amount, "") :
				                                               sl_scalars.compare(cloth::OEqual, ir_current, ir_next, type);
				sl_scalars.guardRangeProgress(ir_unchanged, ir_repeat);
			}
			LLVMBasicBlockRef bb_backedge = LLVMGetInsertBlock(ir_builder);
			LLVMAddIncoming(ir_counter, &ir_next, &bb_backedge, 1);
			branchOnLoopCondition(ir_repeat, bb_body, lb_blocks.exit);
		}
	}
	if (lb_blocks.exit) LLVMPositionBuilderAtEnd(ir_builder, lb_blocks.exit);
}

void FunctionLowering::lowerStatements (const std::vector<cloth::StatementPtr> &statements, ResultExit *tail) {
	const ArrayLifetimes::Mark scope          = al_arrays.mark();
	bool b_fallsThrough = true;
	for (const cloth::StatementPtr &sp_statement : statements) {
		if (!b_fallsThrough) {
			throw cde::ClothDiagnostic(Code::UnsupportedLowering, "LLVM lowering found an unreachable statement",
			                           { { .span = sp_statement->getSpan(), .text = "unreachable statement" } });
		}
		b_fallsThrough = mf_functions.program.controlFlow.at(sp_statement.get()).fallsThrough;
		const ArrayLifetimes::Mark statement = al_arrays.mark();
		if (tail && b_fallsThrough && &sp_statement == &statements.back()) {
			// The checker requires a trailing expression in each continuing value arm.
			const auto &es_tail = static_cast<const cloth::ExpressionStatement &>(*sp_statement);
			std::vector<LLVMValueRef> v_results = lowerValues(*es_tail.expression, tail->types);
			yieldResults(*tail, v_results);
		} else {
			lowerStatement(*sp_statement);
		}
		al_arrays.endStatement(statement);
	}
	al_arrays.endScope(scope);
}

void FunctionLowering::receive (const cloth::Statement &statement, const std::vector<cloth::ReceivingTarget> &targets, const cloth::Expression &producer,
                                std::optional<bool> mutableBinding) {
	if (mutableBinding && dynamic_cast<const cloth::FunctionBlockExpression *>(&producer)) return;
	const cloth::ExpressionInfo &info     = mf_functions.program.expressions.at(&producer);
	const LLVMValueRef           ir_value = lowerExpression(producer);
	if (!info.fallsThrough) return;
	// Capture all sources before the first write, including the source of a broadcast assignment.
	std::vector<LLVMValueRef> v_sources;
	v_sources.reserve(info.types.size());
	for (std::size_t st_index = 0; st_index < info.types.size(); ++st_index) {
		v_sources.push_back(snapshot(info.types[st_index], detail::selectResult(ir_builder, ir_value, info.types.size(), st_index)));
	}
	for (std::size_t st_index = 0; st_index < targets.size(); ++st_index) {
		const auto *named = std::get_if<cloth::ReceivingTarget::Named>(&targets[st_index].value);
		if (!named) continue;
		const std::optional<LoweredValue> lv_target = mutableBinding ? std::nullopt : std::optional{ mutableTarget(statement, named->token) };
		const cloth::ResolvedType        &type      = lv_target ? lv_target->type : mf_functions.program.localTypes.at(&named->token);
		if (!mf_functions.valueTypes.localType(type))
			throw cde::ClothDiagnostic(Code::UnsupportedLowering, "LLVM lowering requires a supported binding type",
			                           { { .span = named->token.getSpan(), .text = "unsupported result binding" } });
		const std::size_t  st_source = info.types.size() == 1 ? 0 : st_index;
		const LLVMValueRef ir_result = convertValue(v_sources[st_source], info.types[st_source], type);
		if (mutableBinding)
			bindLocal(named->token, type, ir_result, *mutableBinding);
		else if (growable::ownsBuffer(type))
			assignArray(lv_target->operand, type, ir_result);
		else
			mf_functions.valueTypes.storeLocal(ir_builder, type, lv_target->operand, ir_result, lv_target->place, lv_target->witness);
	}
}

void FunctionLowering::lowerStatement (const cloth::Statement &statement) {
	const auto saved  = rc_write;
	const auto checks = mf_functions.program.writeChecks.find(&statement);
	rc_write          = checks == mf_functions.program.writeChecks.end() ? cloth::ReceiverChecks{} : checks->second;
	lowerStatementBody(statement);
	rc_write = saved;
}

void FunctionLowering::lowerStatementBody (const cloth::Statement &statement) {
	if (const auto *group = dynamic_cast<const cloth::BindingGroupDeclaration *>(&statement))
		return receive(statement, group->targets, *group->initializer, group->isMutable);
	if (const auto *group = dynamic_cast<const cloth::MultiAssignmentStatement *>(&statement))
		return receive(statement, group->targets, *group->value, std::nullopt);
	if (const auto *bd_binding = dynamic_cast<const cloth::BindingDeclaration *>(&statement)) return lowerBinding(*bd_binding);
	if (const auto *as_write = dynamic_cast<const cloth::AssignmentStatement *>(&statement)) return lowerAssignment(*as_write);
	if (const auto *ea_write = dynamic_cast<const cloth::ElementAssignmentStatement *>(&statement);
	    ea_write && !mf_functions.program.writes.contains(ea_write))
		return lowerElementAssignment(*ea_write->target, ea_write->op.getOperator(), ea_write->value.get(), ea_write->compound.get());
	if (const auto *fw_write = dynamic_cast<const cloth::FieldWriteStatement *>(&statement)) return lowerFieldWrite(*fw_write);
	if (const auto *us_update = dynamic_cast<const cloth::UpdateStatement *>(&statement))
		return lowerUpdate(mutableTarget(*us_update, us_update->name), *us_update->target, us_update->op.getOperator());
	if (const auto *rs_return = dynamic_cast<const cloth::ReturnStatement *>(&statement)) return lowerReturn(rs_return->value.get());
	if (const auto *ws_loop = dynamic_cast<const cloth::WhileStatement *>(&statement)) return lowerWhile(*ws_loop);
	if (const auto *ds_loop = dynamic_cast<const cloth::DoWhileStatement *>(&statement)) return lowerDoWhile(*ds_loop);
	if (const auto *fs_loop = dynamic_cast<const cloth::ForStatement *>(&statement)) return lowerFor(*fs_loop);
	if (const auto *lj_jump = dynamic_cast<const cloth::LoopJumpStatement *>(&statement)) {
		const LoopBlocks &lb_blocks = m_loopBlocks.at(mf_functions.program.jumpTargets.at(lj_jump));
		al_arrays.releaseSince(lb_blocks.owners);
		return branchOrUnreachable(lj_jump->isContinue() ? lb_blocks.continuation : lb_blocks.exit);
	}
	if (const auto *es_statement = dynamic_cast<const cloth::ExpressionStatement *>(&statement)) {
		lowerExpression(*es_statement->expression);
		return;
	}
	throw cde::ClothDiagnostic(Code::UnsupportedLowering, "LLVM lowering does not support this statement",
	                           { { .span = statement.getSpan(), .text = "unsupported statement" } });
}

void FunctionLowering::initializeClass (const cloth::Symbol &owner, const cloth::ClassDeclaration &decl) {
	for (const cloth::ClassParentClause &parent : decl.parents) {
		const auto selected = mf_functions.program.parentConstructors.find(&parent);
		if (selected == mf_functions.program.parentConstructors.end()) continue;
		const std::vector<cloth::CallArgument> empty;
		auto arguments = lowerArguments(parent.constructorArguments ? *parent.constructorArguments : empty, selected->second);
		if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ir_builder))) return;
		invokeConstructor(selected->second, std::move(arguments), ir_self);
	}
	for (const auto *field : mf_functions.objects.layout(owner).fields) {
		const cloth::ResolvedType &fieldType = mf_functions.objects.fieldType(*field);
		LLVMValueRef               value     = nullptr;
		if (const auto *parameter = std::get_if<const cloth::ClassParameterDeclaration *>(&field->declaration))
			value = readValue(*findLocal(&(*parameter)->parameter.name), fieldType);
		else
			value = lowerAs(*std::get<const cloth::ClassFieldDeclaration *>(field->declaration)->initializer, fieldType);
		if (!value) return;
		const LoweredValue destination = objectField(*field, { .handle = ir_self, .directSelf = true });
		if (growable::ownsBuffer(fieldType))
			assignArray(destination.operand, fieldType, value);
		else
			mf_functions.valueTypes.initializeField(ir_builder, fieldType, destination.operand, value, ir_self);
	}
	lowerReturn(nullptr);
}

void FunctionLowering::lowerOwnershipIntrinsic (cloth::LibraryIntrinsic intrinsic, LLVMValueRef function) {
	using Operation = detail::ObjectOperation;
	using Intrinsic = cloth::LibraryIntrinsic;
	switch (intrinsic) {
	case Intrinsic::OwnershipAcquire:
		detail::objectCall(ir_builder, mf_functions.module, Operation::Acquire, { ir_self, LLVMGetParam(function, 1) });
		break;
	case Intrinsic::OwnershipRelease:
		detail::objectCall(ir_builder, mf_functions.module, Operation::Release, { ir_self, LLVMGetParam(function, 1) });
		break;
	case Intrinsic::OwnershipDelete:
		detail::objectCall(ir_builder, mf_functions.module, Operation::Delete, { LLVMGetParam(function, 1), ir_self });
		break;
	case Intrinsic::OwnershipCheckMutation:
		detail::objectCall(ir_builder, mf_functions.module, Operation::CheckMutation, { ir_self });
		break;
	case Intrinsic::OwnershipSetCleanup:
		detail::objectCall(ir_builder, mf_functions.module, Operation::SetCleanup, { ir_self, LLVMGetParam(function, 1) });
		break;
	default:
		throw cde::ClothDiagnostic(Code::UnsupportedLowering, "This library intrinsic has no native implementation");
	}
	lowerReturn(nullptr);
}

void FunctionLowering::lowerFunction (const cloth::CallableSignature &signature, const detail::FunctionBodyToEmit &body) {
	const LLVMValueRef function = body.function;
	const unsigned     offset   = detail::hasObjectReceiver(*body.symbol, body.defaultParameter.has_value()) ? 1u : 0u;
	if (offset) ir_self = LLVMGetParam(function, 0);
	const auto       *primary  = dynamic_cast<const cloth::PrimaryConstructorDeclaration *>(&signature);
	const std::size_t st_count = body.defaultParameter.value_or(signature.parameters.size());
	for (std::size_t st_index = 0; st_index < st_count; ++st_index) {
		const cloth::ParameterDeclaration &pd_parameter = signature.parameters[st_index];
		const LoweredValue                &input        = bindLocal(pd_parameter.name, di_function.parameters.at(st_index),
		                                                            LLVMGetParam(function, static_cast<unsigned>(st_index) + offset),
		                                                            !body.defaultParameter && pd_parameter.isMutable());
		if (primary && !body.defaultParameter) m_locals.emplace(&primary->owner->parameters[st_index].parameter.name, input);
	}
	if (body.defaultParameter) {
		rv_returns = ResultView(di_function.parameters).subspan(st_count, 1);
		return lowerReturn(signature.parameters.at(st_count).defaultValue.get());
	}
	if (primary) return initializeClass(*di_function.owner, *primary->owner);
	if (const auto *secondary = dynamic_cast<const cloth::ConstructorDeclaration *>(&signature)) {
		const auto &delegation = secondary->delegation;
		const auto &selected   = mf_functions.program.expressions.at(&delegation);
		auto        args       = lowerArguments(delegation.suppliedArguments, selected);
		if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ir_builder))) return;
		invokeConstructor(selected, std::move(args), ir_self);
	}
	if (di_function.intrinsic) return lowerOwnershipIntrinsic(di_function.intrinsic.value(), function);
	const cloth::FunctionBody &fb_body = *signature.executableBody();
	if (const cloth::ExpressionBody *eb_body = std::get_if<cloth::ExpressionBody>(&fb_body.value)) {
		return lowerReturn(eb_body->expression.get());
	}
	if (const cloth::BlockBody *bb_body = std::get_if<cloth::BlockBody>(&fb_body.value)) {
		lowerStatements(bb_body->statements);
		if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ir_builder))) return;
		if (LLVMGetTypeKind(LLVMGetReturnType(LLVMGlobalGetValueType(function))) == LLVMVoidTypeKind) {
			al_arrays.releaseSince({});
			LLVMBuildRetVoid(ir_builder);
			return;
		}
	}
	throw cde::ClothDiagnostic(Code::UnsupportedLowering, "LLVM lowering could not emit a return for this function body",
	                           { { .span = fb_body.getSpan(), .text = "unsupported function body" } });
}

void lowerEntryPoint (const cloth::CheckedProgram &program, LLVMModuleRef module) {
	if (!program.entryPoint) {
		throw cde::ClothDiagnostic(Code::InvalidCompilationRequest, "LLVM entry-point lowering requires a checked executable");
	}
	const LLVMContextRef ctx_context = LLVMGetModuleContext(module);
	ModuleFunctions                                                         mf_functions(program, module);
	const LLVMValueRef                                                      fn_entry = mf_functions.declareFunction(*program.entryPoint);
	const std::unique_ptr<LLVMOpaqueBuilder, decltype(&LLVMDisposeBuilder)> ir_builder(LLVMCreateBuilderInContext(ctx_context), LLVMDisposeBuilder);
	mf_functions.emitNativeEntry(ir_builder.get(), fn_entry);
	for (std::size_t st_index = 0; st_index < mf_functions.pending.size(); ++st_index) {
		// Copy the work item before lowering can append a callee or a recursive default.
		const detail::FunctionBodyToEmit body = mf_functions.pending[st_index];
		LLVMPositionBuilderAtEnd(ir_builder.get(), LLVMAppendBasicBlockInContext(ctx_context, body.function, "entry"));
		FunctionLowering fl_function(mf_functions, ir_builder.get(), program.declarations.at(body.symbol));
		fl_function.lowerFunction(*std::get<const cloth::CallableSignature *>(body.symbol->declaration), body);
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
		throw cde::ClothDiagnostic(Code::UnsupportedTarget, std::format("The {} toolchain has no configured {}-bit target", s_triple, sizeBits));
	}
#endif
	return s_triple;
}

TargetMachine createTargetMachine (const std::string_view targetTriple, const unsigned sizeBits) {
	static const bool b_initialized = LLVMInitializeNativeTarget() == 0 && LLVMInitializeNativeAsmPrinter() == 0;
	if (!b_initialized) throw cde::ClothDiagnostic(Code::LlvmInitialization, "LLVM could not initialize the native target and assembly printer");

	if (targetTriple.find('\0') != std::string_view::npos) throw cde::ClothDiagnostic(Code::LlvmTarget, "A target triple contains a null byte");
	const std::string s_requested = targetTriple.empty() ? toolchainTriple(sizeBits) : std::string(targetTriple);
	const Message     s_triple(LLVMNormalizeTargetTriple(s_requested.c_str()), LLVMDisposeMessage);
	LLVMTargetRef     t_target = nullptr;
	char             *s_error  = nullptr;
	const LLVMBool    b_failed = LLVMGetTargetFromTriple(s_triple.get(), &t_target, &s_error);
	checkLLVMResult(b_failed, s_error, Code::LlvmTarget, "LLVM target lookup failed");

	// References to constant data must also link into position-independent executables.
	TargetMachine tm_machine(LLVMCreateTargetMachine(t_target, s_triple.get(), "", "", LLVMCodeGenLevelNone, LLVMRelocPIC, LLVMCodeModelDefault),
	                         LLVMDisposeTargetMachine);
	if (!tm_machine) throw cde::ClothDiagnostic(Code::LlvmTarget, std::format("LLVM could not create a target machine for '{}'", s_triple.get()));
	return tm_machine;
}

void configureModule (LLVMModuleRef module, LLVMTargetMachineRef machine, const cloth::TargetLayout &checkedLayout) {
	const Message    s_triple(LLVMGetTargetMachineTriple(machine), LLVMDisposeMessage);
	const TargetData td_layout(LLVMCreateTargetDataLayout(machine), LLVMDisposeTargetData);
	const unsigned   u_pointerBits = LLVMPointerSize(td_layout.get()) * CHAR_BIT;
	if (u_pointerBits != checkedLayout.sizeBits) {
		throw cde::ClothDiagnostic(Code::AbiLayout, std::format("Target '{}' uses {}-bit pointers. The program was checked for {}-bit pointers",
		                                                        s_triple.get(), u_pointerBits, checkedLayout.sizeBits));
	}
	// Check the native integer alignment classes and the stride of the widest Cloth integer.
	for (const unsigned u_width : { 8u, 16u, 32u, 64u, 128u, cloth::MAX_INTEGER_BITS }) {
		const LLVMTypeRef          ty_int = LLVMIntTypeInContext(LLVMGetModuleContext(module), u_width);
		const cloth::StorageLayout sl_actual{ .size      = LLVMABISizeOfType(td_layout.get(), ty_int),
			                              .alignment = LLVMABIAlignmentOfType(td_layout.get(), ty_int) };
		if (const cloth::StorageLayout sl_expected = checkedLayout.integerStorage(u_width); sl_actual != sl_expected) {
			throw cde::ClothDiagnostic(
			        Code::AbiLayout,
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
	checkLLVMResult(b_failed, s_error, Code::InvalidIr, "LLVM module verification failed");
}

std::vector<std::byte> LoweredModule::emit (EmitKind kind) {
	verify();

	const LLVMCodeGenFileType fileType  = kind == EmitKind::Assembly ? LLVMAssemblyFile : LLVMObjectFile;
	char                     *s_error   = nullptr;
	LLVMMemoryBufferRef       mb_output = nullptr;
	const LLVMBool            b_failed  = LLVMTargetMachineEmitToMemoryBuffer(tm_machine.get(), get(), fileType, &s_error, &mb_output);
	const MemoryBuffer        buffer(mb_output, LLVMDisposeMemoryBuffer);
	checkLLVMResult(b_failed, s_error, Code::LlvmEmission, "LLVM code emission failed");
	if (!buffer || LLVMGetBufferSize(buffer.get()) == 0) {
		throw cde::ClothDiagnostic(Code::LlvmEmission, "LLVM returned empty output");
	}
	const auto *p_bytes = reinterpret_cast<const std::byte *>(LLVMGetBufferStart(buffer.get()));
	return { p_bytes, p_bytes + LLVMGetBufferSize(buffer.get()) };
}

std::vector<std::byte> emit (const cloth::CheckedProgram &program, EmitKind kind, std::string_view targetTriple) {
	return LoweredModule(program, targetTriple).emit(kind);
}

std::vector<std::byte> emitObject (const cloth::CheckedProgram &program, std::string_view targetTriple) {
	return emit(program, EmitKind::Object, targetTriple);
}

}
