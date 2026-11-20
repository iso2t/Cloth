// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <filesystem>

#include <semantic/checker.hpp>

namespace cloth::semantic {

const Symbol *Analyzer::indexValidator (const Symbol &operation) const {
	const DeclarationInfo            &di_signature  = declarations.at(&operation);
	const SymbolScope::const_iterator it_validators = di_signature.owner->members.find(to_string(oIndexValidate));
	if (it_validators == di_signature.owner->members.end() || di_signature.parameters.empty()) return nullptr;
	for (const Symbol &sym_candidate : it_validators->second) {
		const ResultTypes &parameters = declarations.at(&sym_candidate).parameters;
		if (sym_candidate.kind == SymbolKind::Operator && parameters.size() == 1 && parameters.front() == di_signature.parameters.front())
			return &sym_candidate;
	}
	return nullptr;
}

std::optional<Iteration> Analyzer::snapshotLayout (const ResolvedType &type) const {
	const Symbol *owner = type.declaration;
	if (!owner || type.name != "Snapshot" || !type.suffixes.empty() || type.arguments().size() != 1 || program.standardLibraryRoot.empty()) return {};
	const DeclarationInfo &di_signature = declarations.at(owner);
	if (di_signature.owner) return {};
	const SourceFile *source = program.units[di_signature.file]->get_source_unit().get_source();
	std::error_code   ec_file;
	if (!source || source->get_path().empty() ||
	    !std::filesystem::equivalent(source->get_path(), program.standardLibraryRoot / "cloth/collections/Snapshot.co", ec_file))
		return {};
	const StructDeclaration *sd_snapshot = asStruct(owner);
	if (!sd_snapshot || sd_snapshot->fields.size() != 1 || sd_snapshot->fields.front().name.get_name_key() != "slots") return {};
	const Candidate    c_slots = members(type, sd_snapshot->fields.front().name.get_name_key(), SymbolKind::Field).front();
	const ResolvedType array   = substitute(*declarations.at(c_slots.symbol).value, c_slots.arguments);
	if (c_slots.symbol->visibility != Visibility::Private || !array.array() || array.suffixes.back() != arraySuffix()) return {};
	const ResolvedType       rt_slot = array.element();
	const StructDeclaration *sd_slot = rt_slot.suffixes.empty() ? asStruct(rt_slot.declaration) : nullptr;
	if (!sd_slot || sd_slot->fields.size() != 1) return {};
	const Candidate element = members(rt_slot, sd_slot->fields.front().name.get_name_key(), SymbolKind::Field).front();
	if (substitute(*declarations.at(element.symbol).value, element.arguments) != type.arguments().front()) return {};
	return Iteration{ IterationKind::Snapshot, type.arguments().front(), {}, c_slots.symbol, element.symbol };
}

void Analyzer::checkCollectionOperator (const Symbol &operation) const {
	const Operator op = operation.name.get_operator();
	if (op != oIndexRead && op != oIndexWrite && op != oIndexValidate && op != oForeach) return;
	const DeclarationInfo   &di_operator   = declarations.at(&operation);
	const MethodDeclaration &md_operator   = dynamic_cast<const MethodDeclaration &>(*callable(operation));
	const OperatorMember    &om_form       = *md_operator.operatorMember;
	const bool               b_invalidForm = om_form.provider || om_form.selector || om_form.rightReceiver || typeParameters(operation) ||
	                           std::ranges::any_of(md_operator.parameters, &ParameterDeclaration::isOptional);
	if (op == oForeach) {
		if (b_invalidForm || !di_operator.parameters.empty() || di_operator.returns.size() != 1 || !snapshotLayout(di_operator.returns.front()))
			fail(Code::INVALID_OPERATOR_DECLARATION, md_operator.span,
			     "Collection iteration requires a parameterless operator returning the standard library Snapshot");
		return;
	}
	if (b_invalidForm || di_operator.parameters.size() != (op == oIndexWrite ? 2u : 1u) ||
	    (op == oIndexRead && !md_operator.returnTypes.empty() && di_operator.returns.size() != 1))
		fail(Code::INVALID_OPERATOR_DECLARATION, md_operator.span, "Invalid indexing signature");
	if (op == oIndexWrite && !indexValidator(operation))
		fail(Code::INVALID_OPERATOR_DECLARATION, md_operator.span,
		     "An index setter requires a validator with the same index type in its declaring type", &di_operator.owner->name);
}

void BodyChecker::checkValidatorReceiver (const Expression *receiver, const TokenSpan &use) const {
	if (context.kind != SymbolKind::Operator || context.name.get_operator() != oIndexValidate) return;
	const NameExpression *name = dynamic_cast<const NameExpression *>(receiver);
	if (!receiver || (name && name->name.get_lexeme() == to_string(kSelf)))
		fail(Code::INVALID_OPERATOR_DECLARATION, use, "An index validator can only read its receiver", &context.name);
}

IndexedAccess BodyChecker::indexAccess (const ExpressionInfo &operation, const TokenSpan &span) {
	const Symbol *sym_validator = analyzer.indexValidator(*operation.symbol);
	return { operation.symbol, sym_validator,
		 sym_validator && record ? analyzer.useCallable(*sym_validator, operation.typeArguments, context, span) : nullptr, operation.typeArguments };
}

bool BodyChecker::writeIndex (const Statement &statement, const ExpressionPtr &target, const ExpressionInfo &array, const Token &op, const ExpressionPtr &rhs,
                              const ExpressionPtr &compound) {
	const IndexExpression &index    = static_cast<const IndexExpression &>(*target);
	const ResolvedType    &receiver = array.types.front();
	checkFieldWrite(array, target->get_span());
	checkValidatorReceiver(index.array.get(), target->get_span());
	const std::vector<Candidate> v_setters = analyzer.members(receiver, to_string(oIndexWrite), SymbolKind::Operator);
	if (v_setters.empty()) fail(Code::INVALID_INDEX, target->get_span(), "Indexed assignment requires an index setter");
	BodyChecker           bc_write = *this;
	const CapturedOperand co_array{ index.array.get(), array, captured };
	bc_write.captured = &co_array;
	std::optional<CapturedOperand> opt_index, opt_readValue;
	std::optional<ExpressionInfo>  opt_getter, opt_update;
	if (compound || !rhs) {
		opt_getter = bc_write.scalarInfo(*target);
		if (!opt_getter->indexed) fail(Code::INVALID_INDEX, target->get_span(), "Indexed update requires a getter");
		if (opt_getter->fallsThrough) opt_update = bc_write.updateOperator(statement, opt_getter->types.front(), op, rhs);
		const ResolvedType idx = analyzer.substitute(analyzer.declarations.at(opt_getter->symbol).parameters.front(), opt_getter->typeArguments);
		opt_index.emplace(CapturedOperand{ index.index.get(), ExpressionInfo{ { idx } }, bc_write.captured });
		opt_readValue.emplace(CapturedOperand{ target.get(), *opt_getter, &*opt_index });
		bc_write.captured = &*opt_readValue;
		if (!opt_update && !rhs && opt_getter->fallsThrough && !opt_getter->types.front().integer() && !opt_getter->types.front().floating())
			fail(Code::UNARY_OPERAND, op.get_span(), "Postfix update needs a numeric element");
	} else
		bc_write.validateAfter = index.index.get();
	const ExpressionPtr &value = opt_update ? target : compound ? compound : rhs ? rhs : target;
	ExpressionInfo       ei_setter;
	try {
		// A dedicated update still needs a writable index. Class handles mutate in place.
		if (opt_update && analyzer.referenceType(opt_getter->types.front())) {
			const ResultTypes v_types{ opt_index->value.types.front(), opt_getter->types.front() };
			ei_setter = bc_write.bindCall(op, statement.get_span(), { { index.index }, { value } }, v_setters, nullptr, &v_types);
		} else {
			ei_setter = bc_write.invoke(op, statement.get_span(), { { index.index }, { value } }, v_setters);
		}
	} catch (const cde::ClothDiagnostic &error) {
		if (error.get_code() != Code::NO_MATCHING_CALL && error.get_code() != Code::CALL_ARITY) throw;
		fail(Code::OPERATOR_MATCH, target->get_span(), "No index setter accepts these operands");
	}
	ei_setter.indexed = indexAccess(ei_setter, statement.get_span());
	if (opt_getter &&
	    (opt_getter->indexed->validator != ei_setter.indexed->validator ||
	     analyzer.substitute(analyzer.declarations.at(ei_setter.symbol).parameters.front(), ei_setter.typeArguments) != opt_index->value.types.front()))
		fail(Code::INVALID_INDEX, target->get_span(), "Indexed update requires a getter and setter with the same index type and validator");
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
