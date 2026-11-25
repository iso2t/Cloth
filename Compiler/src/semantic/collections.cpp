// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/language/statements/ControlFlow.rst

#include <algorithm>
#include <filesystem>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

#include <ast/decl/declarations.hpp>
#include <ast/decl/struct_declaration.hpp>
#include <ast/expr/expression.hpp>
#include <ast/expr/expressions.hpp>
#include <ast/stmt/statement.hpp>
#include <diagnostics.hpp>
#include <file/source_file.hpp>
#include <semantic/checked_program.hpp>
#include <semantic/checker.hpp>
#include <semantic/symbol_table.hpp>
#include <token/token.hpp>

namespace cloth::semantic {

const Symbol *Analyzer::indexValidator (const Symbol &operation) const {
	const DeclarationInfo &di_signature  = declarations.at(&operation);
	const auto             it_validators = di_signature.owner->members.find(toString(OIndexValidate));
	if (it_validators == di_signature.owner->members.end() || di_signature.parameters.empty()) return nullptr;
	for (const Symbol &sym_candidate : it_validators->second) {
		if (const ResultTypes &parameters = declarations.at(&sym_candidate).parameters;
		    sym_candidate.kind == SymbolKind::Operator && parameters.size() == 1 && parameters.front() == di_signature.parameters.front())
			return &sym_candidate;
	}
	return nullptr;
}

std::optional<Iteration> Analyzer::snapshotLayout (const ResolvedType &type) const {
	const Symbol *owner = type.declaration;
	if (!owner || type.name != "Snapshot" || !type.suffixes.empty() || type.arguments().size() != 1 || program.standardLibraryRoot.empty()) return {};
	const DeclarationInfo &di_signature = declarations.at(owner);
	if (di_signature.owner) return {};
	const SourceFile *source = program.units[di_signature.file]->getSourceUnit().getSource();
	if (std::error_code ec_file; !source || source->getPath().empty() ||
	                             !std::filesystem::equivalent(source->getPath(), program.standardLibraryRoot / "cloth/collections/Snapshot.co", ec_file))
		return {};
	const StructDeclaration *sd_snapshot = asStruct(owner);
	if (!sd_snapshot || sd_snapshot->fields.size() != 1 || sd_snapshot->fields.front().name.getNameKey() != "slots") return {};
	const Candidate    c_slots = members(type, sd_snapshot->fields.front().name.getNameKey(), SymbolKind::Field).front();
	const ResolvedType array   = substitute(*declarations.at(c_slots.symbol).value, c_slots.arguments);
	if (c_slots.symbol->visibility != Visibility::Private || !array.array() || array.suffixes.back() != arraySuffix()) return {};
	const ResolvedType       rt_slot = array.element();
	const StructDeclaration *sd_slot = rt_slot.suffixes.empty() ? asStruct(rt_slot.declaration) : nullptr;
	if (!sd_slot || sd_slot->fields.size() != 1) return {};
	const Candidate element = members(rt_slot, sd_slot->fields.front().name.getNameKey(), SymbolKind::Field).front();
	if (substitute(*declarations.at(element.symbol).value, element.arguments) != type.arguments().front()) return {};
	return Iteration{
		.kind = IterationKind::Snapshot, .element = type.arguments().front(), .snapshot = {}, .slots = c_slots.symbol, .slotValue = element.symbol
	};
}

void Analyzer::checkCollectionOperator (const Symbol &operation) const {
	const Operator op = operation.name.getOperator();
	if (op != OIndexRead && op != OIndexWrite && op != OIndexValidate && op != OForeach) return;
	const DeclarationInfo &di_operator              = declarations.at(&operation);
	const auto            &md_operator              = dynamic_cast<const MethodDeclaration &>(*callable(operation));
	const auto &[selector, rightReceiver, provider] = *md_operator.operatorMember;
	const bool b_invalidForm                        = provider || selector || rightReceiver || typeParameters(operation) ||
	                           std::ranges::any_of(md_operator.parameters, &ParameterDeclaration::isOptional);
	if (op == OForeach) {
		if (b_invalidForm || !di_operator.parameters.empty() || di_operator.returns.size() != 1 || !snapshotLayout(di_operator.returns.front()))
			fail(Code::InvalidOperatorDeclaration, md_operator.span,
			     "Collection iteration requires a parameterless operator returning the standard library Snapshot");
		return;
	}
	if (b_invalidForm || di_operator.parameters.size() != (op == OIndexWrite ? 2u : 1u) ||
	    (op == OIndexRead && !md_operator.returnTypes.empty() && di_operator.returns.size() != 1))
		fail(Code::InvalidOperatorDeclaration, md_operator.span, "Invalid indexing signature");
	if (op == OIndexWrite && !indexValidator(operation))
		fail(Code::InvalidOperatorDeclaration, md_operator.span, "An index setter requires a validator with the same index type in its declaring type",
		     &di_operator.owner->name);
}

void BodyChecker::checkValidatorReceiver (const Expression *receiver, const TokenSpan &use) const {
	if (context.kind != SymbolKind::Operator || context.name.getOperator() != OIndexValidate) return;
	if (const auto *name = dynamic_cast<const NameExpression *>(receiver); !receiver || (name && name->name.getLexeme() == toString(KSelf)))
		fail(Code::InvalidOperatorDeclaration, use, "An index validator can only read its receiver", &context.name);
}

IndexedAccess BodyChecker::indexAccess (const ExpressionInfo &operation, const TokenSpan &span) const {
	const Symbol *sym_validator = analyzer.indexValidator(*operation.symbol);
	return { .operation         = operation.symbol,
		 .operationInstance = operation.instance,
		 .validator         = sym_validator,
		 .validatorInstance = sym_validator && record ? analyzer.useCallable(*sym_validator, operation.typeArguments, context, span) : nullptr,
		 .arguments         = operation.typeArguments };
}

bool BodyChecker::writeIndex (const Statement &statement, const ExpressionPtr &target, const ExpressionInfo &array, const Token &op, const ExpressionPtr &rhs,
                              const ExpressionPtr &compound) {
	const auto         &index    = static_cast<const IndexExpression &>(*target);
	const ResolvedType &receiver = array.types.front();
	checkFieldWrite(array, target->getSpan());
	checkValidatorReceiver(index.array.get(), target->getSpan());
	const std::vector<Candidate> v_setters = analyzer.members(receiver, toString(OIndexWrite), SymbolKind::Operator);
	if (v_setters.empty()) fail(Code::InvalidIndex, target->getSpan(), "Indexed assignment requires an index setter");
	BodyChecker           bc_write = *this;
	const CapturedOperand co_array{ .node = index.array.get(), .value = array, .previous = captured };
	bc_write.captured = &co_array;
	// The setter reuses both captures after the getter has been checked.
	std::optional<CapturedOperand> opt_index, opt_readValue;
	std::optional<ExpressionInfo>  opt_getter, opt_update;
	if (compound || !rhs) {
		opt_getter = bc_write.scalarInfo(*target);
		if (!opt_getter->indexed) fail(Code::InvalidIndex, target->getSpan(), "Indexed update requires a getter");
		if (opt_getter->fallsThrough) opt_update = bc_write.updateOperator(statement, opt_getter->types.front(), op, rhs);
		const ResolvedType idx = analyzer.substitute(analyzer.declarations.at(opt_getter->symbol).parameters.front(), opt_getter->typeArguments);
		opt_index.emplace(CapturedOperand{ .node = index.index.get(), .value = ExpressionInfo{ .types = { idx } }, .previous = bc_write.captured });
		opt_readValue.emplace(CapturedOperand{ .node = target.get(), .value = *opt_getter, .previous = &*opt_index });
		bc_write.captured = &*opt_readValue;
		if (!opt_update && !rhs && opt_getter->fallsThrough && !opt_getter->types.front().integer() && !opt_getter->types.front().floating())
			fail(Code::UnaryOperand, op.getSpan(), "Postfix update needs a numeric element");
	} else
		bc_write.validateAfter = index.index.get();
	const ExpressionPtr &value = opt_update ? target : compound ? compound : rhs ? rhs : target;
	ExpressionInfo       ei_setter;
	try {
		// A dedicated update still needs a writable index. Class handles mutate in place.
		if (opt_update && analyzer.referenceType(opt_getter->types.front())) {
			const ResultTypes v_types{ opt_index->value.types.front(), opt_getter->types.front() };
			ei_setter = bc_write.bindCall(op, statement.getSpan(), { { .expression = index.index }, { .expression = value } }, v_setters, nullptr,
			                              &v_types);
		} else {
			ei_setter = bc_write.invoke(op, statement.getSpan(), { { .expression = index.index }, { .expression = value } }, v_setters);
		}
	} catch (const cde::ClothDiagnostic &error) {
		if (error.getCode() != Code::NoMatchingCall && error.getCode() != Code::CallArity) throw;
		fail(Code::OperatorMatch, target->getSpan(), "No index setter accepts these operands");
	}
	ei_setter.indexed = indexAccess(ei_setter, statement.getSpan());
	if (opt_getter &&
	    (opt_getter->indexed->validator != ei_setter.indexed->validator ||
	     analyzer.substitute(analyzer.declarations.at(ei_setter.symbol).parameters.front(), ei_setter.typeArguments) != opt_index->value.types.front()))
		fail(Code::InvalidIndex, target->getSpan(), "Indexed update requires a getter and setter with the same index type and validator");
	if (opt_getter) ei_setter.fallsThrough &= opt_getter->fallsThrough;
	if (opt_update) {
		opt_update->indexed = ei_setter.indexed;
		opt_update->fallsThrough &= ei_setter.fallsThrough;
		if (record) remember(op, opt_update->symbol->name);
	}
	if (record) bf_facts.writes[&statement] = opt_update ? *opt_update : ei_setter;
	fs_flow = std::move(bc_write.fs_flow);
	return opt_update ? opt_update->fallsThrough : ei_setter.fallsThrough;
}
}
