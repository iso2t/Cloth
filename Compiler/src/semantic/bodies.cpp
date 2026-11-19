// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <format>
#include <functional>
#include <ranges>

#include <ast/decl/enum_declaration.hpp>
#include <ast/stmt/do_while_statement.hpp>
#include <ast/stmt/for_statement.hpp>
#include <ast/stmt/statements.hpp>
#include <ast/stmt/while_statement.hpp>
#include <semantic/checker.hpp>
#include <token/token.hpp>

namespace cloth::semantic {

BodyChecker::BodyChecker (Analyzer &analyzer, const Symbol &context, BodyFacts &facts, const Substitutions &arguments, DeclarationInfo &signature,
                          const ResolvedType *attachment) :
        analyzer(analyzer),
        context(context),
        bf_facts(facts),
        m_arguments(arguments),
        di_signature(signature),
        owner(analyzer.declarations.at(&context).owner),
        rt_attachment(attachment),
        returns(signature.returns) {
	while (owner && owner->kind != SymbolKind::Type && owner->kind != SymbolKind::State) {
		owner = analyzer.declarations.at(owner).owner;
	}
}

const Binding *BodyChecker::local (const std::string &name) const {
	for (const auto &scope : std::views::reverse(scopes)) {
		const auto binding = scope.find(name);
		if (binding != scope.end()) {
			return &binding->second;
		}
	}
	return nullptr;
}

void BodyChecker::remember (const Token &use, const Token &declaration) const {
	if (record) {
		bf_facts.references[&use] = &declaration;
	}
}

void BodyChecker::bind (const Token &name, std::optional<ResolvedType> type, const bool mutableBinding, const bool deferred) {
	const auto [fst, snd] = scopes.back().emplace(
	        name.get_lexeme(), Binding{ .type = std::move(type), .name = &name, .mutableBinding = mutableBinding, .deferred = deferred });
	if (!snd) {
		fail(Code::DUPLICATE_BINDING, name.get_span(), "Duplicate binding '" + name.get_lexeme() + "'", fst->second.name);
	}
	if (record && fst->second.type) bf_facts.localTypes[&name] = *fst->second.type;
}

void BodyChecker::checkDefaultName (const Token &name) const {
	if (!pd_default || local(name.get_lexeme())) return;
	for (const ParameterDeclaration &pd_parameter : callable(context)->parameters) {
		if (pd_parameter.name.get_lexeme() == name.get_lexeme()) {
			fail(Code::INVALID_PARAMETER_DEFAULT, name.get_span(), "A default can only read earlier parameters", &pd_parameter.name);
		}
	}
}

bool BodyChecker::readOnlyDefaultRoot (const Expression &node) const {
	if (!pd_default) return false;
	if (const FieldAccessExpression *fa_field = dynamic_cast<const FieldAccessExpression *>(&node)) return readOnlyDefaultRoot(*fa_field->receiver);
	if (const IndexExpression *ie_index = dynamic_cast<const IndexExpression *>(&node)) return readOnlyDefaultRoot(*ie_index->array);
	if (const CastExpression *ce_cast = dynamic_cast<const CastExpression *>(&node)) return readOnlyDefaultRoot(*ce_cast->operand);
	if (const GuardedReceiver *gr_receiver = dynamic_cast<const GuardedReceiver *>(&node)) return readOnlyDefaultRoot(*gr_receiver->source);
	if (const GuardedAccessExpression *ga_access = dynamic_cast<const GuardedAccessExpression *>(&node))
		return readOnlyDefaultRoot(*ga_access->continuation);
	const NameExpression *ne_name = dynamic_cast<const NameExpression *>(&node);
	if (!ne_name) return false;
	const std::string &s_name = ne_name->name.get_lexeme();
	if (const Binding *b_binding = local(s_name)) {
		return std::ranges::any_of(callable(context)->parameters,
		                           [b_binding] (const ParameterDeclaration &parameter) { return &parameter.name == b_binding->name; });
	}
	return hasInstance() && (s_name == to_string(kSelf) || !analyzer.members(selfType(), s_name, SymbolKind::Field).empty());
}

ResolvedType BodyChecker::selfType () const {
	if (!owner) {
		fail(Code::RECEIVER_OUTSIDE_METHOD, context.name.get_span(), "No enclosing receiver");
	}
	if (owner->kind == SymbolKind::State) {
		const DeclarationInfo &state = analyzer.declarations.at(owner);
		return analyzer.substitute(state.owner ? analyzer.ownerType(*state.owner) : state.parents.front(), m_arguments);
	}
	return analyzer.substitute(analyzer.ownerType(*owner), m_arguments);
}

ResolvedType BodyChecker::bodyType (const TypeReference &reference, bool allowOpen) {
	ResolvedType rt_type = analyzer.checkedType(reference, context, m_arguments, allowOpen);
	if (record && !allowOpen) analyzer.useType(rt_type, context, reference.get_span());
	return rt_type;
}

bool BodyChecker::hasInstance () const {
	return owner && rp_receiverPhase != ReceiverPhase::ConstructorDefault && rp_receiverPhase != ReceiverPhase::Delegation;
}

void BodyChecker::checkFieldRead (const Symbol &field, const Token &use) const {
	if (rp_receiverPhase == ReceiverPhase::FieldInitializer && field.classFieldIndex && analyzer.declarations.at(&field).owner == owner &&
	    *field.classFieldIndex >= context.classFieldIndex.value()) {
		fail(Code::PARTIAL_CLASS_INSTANCE, use.get_span(), std::format("Field '{}' has not been initialized", use.get_lexeme()), &field.name);
	}
}

void BodyChecker::checkFieldWrite (const ExpressionInfo &target, const TokenSpan &span) const {
	if (rp_receiverPhase == ReceiverPhase::FieldInitializer && target.receiverFieldPath != ReceiverFieldPath::None) {
		fail(Code::PARTIAL_CLASS_INSTANCE, span, "Instance fields are read-only during field initialization");
	}
}

void BodyChecker::require (const ResultTypes &actual, const ResultTypes &expected, const TokenSpan &span, const Code code) const {
	if (actual.size() != expected.size()) {
		fail(code == Code::TYPE_MISMATCH ? Code::RESULT_ARITY : code, span,
		     std::format("Expected {} result(s), found {}", expected.size(), actual.size()));
	}
	for (std::size_t i = 0; i < actual.size(); ++i) {
		if (!analyzer.accepts(expected[i], actual[i])) {
			fail(code, span, "Expected " + expected[i].spelling() + ", found " + actual[i].spelling());
		}
	}
}

bool BodyChecker::expect (const Expression &node, const ResolvedType &type) {
	const std::optional<ResolvedType> opt_actual = scalar(node, &type);
	if (opt_actual) require({ *opt_actual }, { type }, node.get_span());
	return opt_actual.has_value();
}

void BodyChecker::checkReturn (const Expression &node) {
	ExpressionInfo ei_result;
	try {
		ei_result = expression(node, &returns);
	} catch (const cde::ClothDiagnostic &error) {
		throw ReturnDiagnostic(error);
	}
	if (!ei_result.fallsThrough) return;
	if (returns.empty() && !ei_result.types.empty()) {
		fail(Code::VALUE_IN_VOID, node.get_span(), "This callable declares no results", &context.name);
	}
	if (ei_result.types.size() != returns.size()) {
		const Code dc_code = returns.size() == 1 && ei_result.types.empty() ? Code::RETURN_TYPE : Code::RESULT_ARITY;
		fail(dc_code, node.get_span(), std::format("Expected {} result(s), found {}", returns.size(), ei_result.types.size()), &context.name);
	}
	const ResultListExpression *rl_list = dynamic_cast<const ResultListExpression *>(&node);
	for (std::size_t st_slot = 0; st_slot < returns.size(); ++st_slot) {
		if (!analyzer.accepts(returns[st_slot], ei_result.types[st_slot])) {
			if (returns.size() == 1) {
				fail(Code::RETURN_TYPE, node.get_span(),
				     "Expected " + returns.front().spelling() + ", found " + ei_result.types.front().spelling(), &context.name);
			}
			fail(Code::RETURN_TYPE, rl_list ? rl_list->values[st_slot]->get_span() : node.get_span(),
			     std::format("Expected {} for result {}, found {}", returns[st_slot].spelling(), st_slot + 1, ei_result.types[st_slot].spelling()),
			     &context.name);
		}
	}
}

void BodyChecker::checkBody (const FunctionBody &body) {
	if (const BlockBody *block = std::get_if<BlockBody>(&body.value)) {
		bool fallsThrough = true;
		for (const StatementPtr &stmt : block->statements) {
			const bool next = statement(*stmt);
			fallsThrough &= next;
		}
		if (fallsThrough && !returns.empty()) {
			fail(Code::MISSING_RETURN, block->span, "Not every path returns a value", &context.name);
		}
	} else if (const ExpressionBody *bodyExpr = std::get_if<ExpressionBody>(&body.value)) {
		checkReturn(*bodyExpr->expression);
	}
}

void BodyChecker::check () {
	const DeclarationInfo &info = di_signature;
	if (const CallableSignature *signature = callable(context)) {
		if (context.kind == SymbolKind::Constructor) {
			rp_receiverPhase = ReceiverPhase::ConstructorDefault;
		}
		for (std::size_t i = 0; i < signature->parameters.size(); ++i) {
			const ParameterDeclaration &param = signature->parameters[i];
			if (param.defaultValue) {
				BodyChecker bc_default = *this;
				bc_default.pd_default  = &param;
				bc_default.expect(*param.defaultValue, info.parameters[i]);
			}
			bind(param.name, info.parameters[i], param.isMutable());
		}
		if (const ConstructorDeclaration *cd_constructor = dynamic_cast<const ConstructorDeclaration *>(signature)) {
			rp_receiverPhase = ReceiverPhase::Delegation;
			const Candidate              c_primary{ analyzer.declarations.at(owner).primaryConstructor, analyzer.argumentsOf(selfType()) };
			const ConstructorDelegation &cd_delegation = cd_constructor->delegation;
			const ExpressionInfo         ei_call = invoke(cd_delegation.name, cd_delegation.span, cd_delegation.suppliedArguments, { c_primary });
			if (record) {
				bf_facts.expressions[&cd_delegation] = ei_call;
			}
		}
		rp_receiverPhase = ReceiverPhase::Ready;
		if (const FunctionBody *body = signature->executableBody()) {
			if (info.inferResult) {
				const auto                       &inferred   = std::get<ExpressionBody>(body->value);
				const std::optional<ResolvedType> opt_result = scalar(*inferred.expression);
				if (!opt_result)
					fail(Code::INVALID_OPERATOR_DECLARATION, inferred.expression->get_span(),
					     "Cannot infer an operator result from a body that always exits", &context.name);
				di_signature.returns     = { *opt_result };
				di_signature.inferResult = false;
			} else {
				checkBody(*body);
			}
		}
		return;
	}
	if (context.kind == SymbolKind::StateInitializer) {
		checkBody(std::get<const StateInitializerDeclaration *>(context.declaration)->body);
		return;
	}
	const ClassDeclaration *cd        = nullptr;
	const EnumDeclaration  *ed        = nullptr;
	const Symbol           *container = context.kind == SymbolKind::Type ? &context : owner;
	if (container && container->kind == SymbolKind::Type) {
		const auto td = std::get<const TypeDeclaration *>(container->declaration);
		cd            = dynamic_cast<const ClassDeclaration *>(td);
		ed            = dynamic_cast<const EnumDeclaration *>(td);
	}
	if (cd && (context.kind == SymbolKind::Field || context.kind == SymbolKind::Type)) {
		owner = container;
		for (const ClassParameterDeclaration &p : cd->parameters) {
			bind(p.parameter.name, bodyType(p.parameter.type), p.parameter.isMutable());
		}
	}
	if (context.kind == SymbolKind::Type && cd) {
		rp_receiverPhase = ReceiverPhase::Delegation;
		for (std::size_t i = 0; i < cd->parents.size(); ++i) {
			if (cd->parents[i].constructorArguments) {
				const ResolvedType &parent  = info.parents[i];
				ExpressionInfo      ei_call = invoke(cd->parents[i].type.get_name(), cd->parents[i].span, *cd->parents[i].constructorArguments,
				                                     analyzer.members(parent, parent.name, SymbolKind::Constructor));
				if (record) bf_facts.parentConstructors.emplace(&cd->parents[i], std::move(ei_call));
			}
		}
	} else if (context.kind == SymbolKind::Field) {
		if (const auto field = std::get_if<const ClassFieldDeclaration *>(&context.declaration)) {
			rp_receiverPhase = ReceiverPhase::FieldInitializer;
			expect(*(*field)->initializer, *info.value);
		} else if (const auto field = std::get_if<const EnumFieldDeclaration *>(&context.declaration); field && (*field)->defaultValue) {
			expect(*(*field)->defaultValue, *info.value);
		}
	} else if (context.kind == SymbolKind::EnumCase) {
		const EnumCaseDeclaration &ec = *std::get<const EnumCaseDeclaration *>(context.declaration);
		if (ec.values.size() > ed->fields.size()) {
			fail(Code::INVALID_ENUM_MEMBER, ec.name.get_span(), "Too many enum payload values");
		}
		for (std::size_t i = 0; i < ed->fields.size(); ++i) {
			if (i < ec.values.size()) {
				expect(*ec.values[i], bodyType(ed->fields[i].type));
			} else if (!ed->fields[i].defaultValue) {
				fail(Code::INVALID_ENUM_MEMBER, ec.name.get_span(), "Missing enum payload value", &ed->fields[i].name);
			}
		}
	} else if (context.kind == SymbolKind::State || context.kind == SymbolKind::StateAttachment) {
		const StateSelectionDeclaration *selection = nullptr;
		if (context.kind == SymbolKind::State) {
			selection = dynamic_cast<const NestedStateDeclaration *>(std::get<const StateBodyDeclaration *>(context.declaration));
		} else {
			selection = std::get<const StateAttachmentDeclaration *>(context.declaration);
		}
		if (selection && selection->initial) {
			statePayload(selection->initial->payload, *info.value, selection->initial->span, context.name);
		}
	}
}

BodyChecker &BodyChecker::onPath (bool reachable, std::optional<BodyChecker> &unreachable) {
	if (reachable) return *this;
	// Keep checking later source without letting its writes or jumps escape an exited path.
	if (!unreachable) unreachable.emplace(*this);
	return *unreachable;
}

ExpressionInfo BodyChecker::block (const std::vector<StatementPtr> &statements, const ResultTypes *expected, const bool value) {
	scopes.emplace_back();

	struct ScopeExit {
		decltype(scopes) &stack;

		~ScopeExit () {
			stack.pop_back();
		}
	} scopeExit{ scopes };

	ExpressionInfo             result;
	std::optional<BodyChecker> unreachable;
	for (const StatementPtr &stmt : statements) {
		if (!result.fallsThrough) {
			onPath(false, unreachable).statement(*stmt);
			continue;
		}
		if (value && &stmt == &statements.back()) {
			if (const auto *last = dynamic_cast<const ExpressionStatement *>(stmt.get())) {
				const ExpressionInfo tail = expression(*last->expression, expected);
				if (result.fallsThrough) {
					result = tail;
				}
				continue;
			}
		}
		const bool next = statement(*stmt);
		result.fallsThrough &= next;
	}
	return result;
}

void BodyChecker::refine (const Expression &test, const bool truth) {
	if (const UnaryExpression *neg = dynamic_cast<const UnaryExpression *>(&test); neg && neg->op.get_operator() == oNot) {
		refine(*neg->operand, !truth);
		return;
	}
	const auto *binary = dynamic_cast<const BinaryExpression *>(&test);
	if (!binary) {
		return;
	}
	const Operator o_operation = binary->op.get_operator();
	if ((o_operation == oAnd && truth) || (o_operation == oOr && !truth)) {
		refine(*binary->left, truth);
		refine(*binary->right, truth);
		return;
	}
	if (!((o_operation == oNotEqual && truth) || (o_operation == oEqual && !truth))) {
		return;
	}
	const Expression *operand = nullptr;
	if (dynamic_cast<const NullLiteral *>(binary->left.get())) {
		operand = binary->right.get();
	}
	if (dynamic_cast<const NullLiteral *>(binary->right.get())) {
		operand = binary->left.get();
	}
	const auto *name = dynamic_cast<const NameExpression *>(operand);
	if (const Binding *binding = name ? local(name->name.get_lexeme()) : nullptr; binding && binding->type && binding->type->nullable()) {
		fs_flow.narrowed[binding->name] = binding->type->element();
	}
}

ExpressionInfo BodyChecker::condition (const IfExpression &node, const ResultTypes *expected, const bool discard) {
	std::optional<ResultTypes>                   opt_common;
	std::optional<Narrowing>                     opt_joined;
	const std::function<void(const Narrowing &)> join = [&] (const Narrowing &facts) {
		if (!opt_joined) {
			opt_joined = facts;
			return;
		}
		std::erase_if(*opt_joined, [&facts] (const Narrowing::value_type &fact) {
			const Narrowing::const_iterator it_fact = facts.find(fact.first);
			return it_fact == facts.end() || it_fact->second != fact.second;
		});
	};
	const std::function<void(const std::vector<StatementPtr> &, const TokenSpan &, bool)> branch = [&] (const std::vector<StatementPtr> &statements,
	                                                                                                    const TokenSpan &span, bool reachable) {
		BodyChecker          bc_branch = *this;
		const ExpressionInfo ei_result = bc_branch.block(statements, expected, !discard);
		if (reachable) {
			fs_flow.loopMayBreak |= bc_branch.fs_flow.loopMayBreak;
			fs_flow.loopMayContinue |= bc_branch.fs_flow.loopMayContinue;
			if (ei_result.fallsThrough) join(bc_branch.fs_flow.narrowed);
		}
		if (!ei_result.fallsThrough || discard) return;
		if (statements.empty() || !dynamic_cast<const ExpressionStatement *>(statements.back().get())) {
			fail(Code::MISSING_BRANCH_VALUE, span, "Conditional branch needs a value");
		}
		if (!opt_common) {
			opt_common = ei_result.types;
			return;
		}
		const ResultTypes &v_next  = ei_result.types;
		const TokenSpan    ts_tail = statements.back()->get_span();
		if (v_next.size() != opt_common->size()) {
			const Code dc_code = v_next.size() > 1 || opt_common->size() > 1 ? Code::BRANCH_RESULT_ARITY : Code::BRANCH_TYPE_MISMATCH;
			fail(dc_code, ts_tail, std::format("Conditional branches produce {} and {} results", opt_common->size(), v_next.size()));
		}
		for (std::size_t st_slot = 0; st_slot < v_next.size(); ++st_slot) {
			ResolvedType &rt_common = (*opt_common)[st_slot];
			if (v_next[st_slot] == rt_common) continue;
			if (expected && expected->size() == v_next.size() && analyzer.accepts((*expected)[st_slot], rt_common) &&
			    analyzer.accepts((*expected)[st_slot], v_next[st_slot])) {
				rt_common = (*expected)[st_slot];
			} else {
				fail(Code::BRANCH_TYPE_MISMATCH, ts_tail,
				     std::format("Conditional result {} has types {} and {}", st_slot + 1, rt_common.spelling(), v_next[st_slot].spelling()));
			}
		}
	};
	bool b_remaining = true;
	for (const ConditionalBranch &cb_branch : node.branches) {
		const std::optional<ResolvedType> opt_test = scalar(*cb_branch.condition);
		if (opt_test && *opt_test != ResolvedType(kBoolean)) {
			fail(Code::NON_BOOLEAN_CONDITION, cb_branch.condition->get_span(),
			     std::format("An {} condition must be {}", to_string(kIf), to_string(kBoolean)));
		}
		const BooleanLiteral *bl_test = dynamic_cast<const BooleanLiteral *>(cb_branch.condition.get());
		b_remaining &= opt_test.has_value();
		const Narrowing n_incoming = fs_flow.narrowed;
		refine(*cb_branch.condition, true);
		branch(cb_branch.statements, cb_branch.span, b_remaining && (!bl_test || bl_test->value));
		fs_flow.narrowed = n_incoming;
		refine(*cb_branch.condition, false);
		b_remaining &= !bl_test || !bl_test->value;
	}
	if (node.otherwise) {
		branch(*node.otherwise, *node.otherwiseSpan, b_remaining);
	} else if (!discard && (b_remaining || opt_joined)) {
		fail(Code::MISSING_ELSE, node.get_span(), std::format("A conditional value requires {}", to_string(kElse)));
	} else if (b_remaining) {
		join(fs_flow.narrowed);
	}
	if (!opt_joined) return ExpressionInfo::never();
	fs_flow.narrowed = std::move(*opt_joined);
	return { opt_common.value_or(ResultTypes{}) };
}

void BodyChecker::assign (const Token &target, const std::optional<ResolvedType> &value) {
	const ExpressionInfo ei_binding = name(target);
	checkFieldWrite(ei_binding, target.get_span());
	if (pd_default && readOnlyDefaultRoot(NameExpression(target))) {
		fail(Code::INVALID_PARAMETER_DEFAULT, target.get_span(), "Parameter defaults can only read the receiver and earlier parameters",
		     &pd_default->name);
	}
	const Binding *b_local = local(target.get_lexeme());
	if (b_local && (!b_local->mutableBinding || b_local->deferred)) {
		fail(Code::IMMUTABLE_BINDING, target.get_span(), "Cannot assign to immutable binding '" + target.get_lexeme() + "'", b_local->name);
	}
	if (!ei_binding.binding && (!ei_binding.symbol || ei_binding.symbol->kind != SymbolKind::Field)) {
		fail(Code::EXPECTED_BINDING, target.get_span(), "Expected a writable binding");
	}
	if (ei_binding.symbol && std::holds_alternative<const EnumFieldDeclaration *>(ei_binding.symbol->declaration)) {
		fail(Code::INVALID_FIELD_TARGET, target.get_span(), "Enum payload fields are read-only", &ei_binding.symbol->name);
	}
	if (b_local) {
		if (value && b_local->type) require({ *value }, { *b_local->type }, target.get_span());
		fs_flow.narrowed.erase(b_local->name);
	} else if (value) {
		require({ *value }, ei_binding.types, target.get_span());
	}
}

bool BodyChecker::receive (const std::vector<ReceivingTarget> &targets, const Expression &producer, const std::vector<TypeReference> *annotations,
                           const bool mutableBinding) {
	ResultTypes expected;
	if (annotations) {
		if (annotations->size() > 1 && annotations->size() != targets.size())
			fail(Code::RECEPTION_ANNOTATIONS, producer.get_span(), "Specify one type per result or one shared type for a scalar");
		for (const TypeReference &annotation : *annotations) {
			ResolvedType type = bodyType(annotation);
			if (type.name == to_string(kVoid)) fail(Code::VOID_TYPE, annotation.get_span(), "A binding requires a value type");
			expected.push_back(std::move(type));
		}
	} else {
		for (const ReceivingTarget &target : targets) {
			if (std::holds_alternative<ReceivingTarget::Discard>(target.value))
				expected.emplace_back();
			else if (const Binding *binding = local(target.get_token().get_lexeme()))
				expected.push_back(binding->type.value_or(ResolvedType{}));
			else
				expected.push_back(name(target.get_token()).types.front());
		}
	}
	ExpressionInfo result = expression(producer, expected.empty() ? nullptr : &expected);
	if (!result.fallsThrough) {
		for (std::size_t st_slot = 0; st_slot < targets.size(); ++st_slot) {
			if (std::holds_alternative<ReceivingTarget::Discard>(targets[st_slot].value)) continue;
			const Token &t_target = targets[st_slot].get_token();
			if (annotations) {
				bind(t_target, expected.empty() ? std::nullopt : std::optional{ expected[expected.size() == 1 ? 0 : st_slot] }, mutableBinding);
			} else {
				assign(t_target, std::nullopt);
			}
		}
		return false;
	}
	if (result.resultKind() == ResultKind::Empty) fail(Code::VOID_VALUE, producer.get_span(), "Assignment requires a value");
	const bool broadcast = result.resultKind() == ResultKind::Single;
	if (!broadcast && result.types.size() != targets.size()) fail(Code::RECEPTION_ARITY, producer.get_span(), "Target count does not match result count");
	if (annotations && !expected.empty() && ((broadcast && expected.size() > 1) || (!broadcast && expected.size() == 1)))
		fail(Code::RECEPTION_ANNOTATIONS, producer.get_span(), "Use one shared annotation for a scalar or one annotation per result");
	if (broadcast && !annotations) {
		if (const auto shared = std::ranges::find_if(expected, [] (const ResolvedType &t) { return !t.name.empty(); });
		    shared != expected.end() && !analyzer.accepts(*shared, result.types.front()) &&
		    std::ranges::all_of(expected, [&] (const ResolvedType &t) { return t.name.empty() || t == *shared; }))
			result = scalarInfo(producer, &*shared);
	}
	for (std::size_t i = 0; i < targets.size(); ++i) {
		const ResolvedType &actual   = result.types[broadcast ? 0 : i];
		const ResolvedType *required = expected.empty() ? nullptr : &expected[expected.size() == 1 ? 0 : i];
		if (required && !required->name.empty()) require({ actual }, { *required }, producer.get_span());
		if (std::holds_alternative<ReceivingTarget::Discard>(targets[i].value)) continue;
		const Token &target = targets[i].get_token();
		if (annotations) {
			if (!required && actual.name == to_string(kNull))
				fail(Code::EXPLICIT_NULLABLE_TYPE, target.get_span(),
				     std::format("A {} initializer requires an explicit nullable type", to_string(kNull)));
			bind(target, required ? *required : actual, mutableBinding);
		} else
			assign(target, actual);
	}
	return true;
}

void BodyChecker::writable (const Expression &target) {
	if (const auto *field = dynamic_cast<const FieldAccessExpression *>(&target)) {
		const ExpressionInfo selected = this->field(*field);
		if (selected.symbol->kind != SymbolKind::Field || std::holds_alternative<const EnumFieldDeclaration *>(selected.symbol->declaration)) {
			fail(Code::INVALID_FIELD_TARGET, field->name.get_span(), "Enum cases and payload fields are read-only");
		}
		if (!std::holds_alternative<const StructFieldDeclaration *>(selected.symbol->declaration)) {
			return;
		}
		writable(*field->receiver);
	} else if (const auto *index = dynamic_cast<const IndexExpression *>(&target)) {
		writable(*index->array);
	} else if (const auto *name = dynamic_cast<const NameExpression *>(&target)) {
		if (const Binding *b = local(name->name.get_lexeme()); b && (!b->mutableBinding || b->deferred)) {
			fail(Code::IMMUTABLE_BINDING, name->get_span(), "Cannot modify an immutable value", b->name);
		}
	} else {
		fail(Code::INVALID_FIELD_TARGET, target.get_span(), "Assignment needs writable storage");
	}
}

std::optional<ExpressionInfo> BodyChecker::updateOperator (const Statement &statement, const ResolvedType &receiver, const Token &op,
                                                           const ExpressionPtr &rhs) {
	const std::vector<Candidate> candidates = analyzer.members(receiver, op.get_lexeme(), SymbolKind::Operator);
	if (candidates.empty()) {
		return std::nullopt;
	}
	std::vector<CallArgument> arguments;
	if (rhs) {
		arguments.push_back({ .expression = rhs });
	}
	const ExpressionInfo selected = invoke(op, statement.get_span(), arguments, candidates);
	if (record) {
		bf_facts.writes[&statement] = selected;
	}
	return selected;
}

bool BodyChecker::writeMember (const Statement &statement, const ExpressionPtr &target, const Token &op, const ExpressionPtr &rhs,
                               const ExpressionPtr &compound) {
	if (readOnlyDefaultRoot(*target)) {
		fail(Code::INVALID_PARAMETER_DEFAULT, target->get_span(), "Parameter defaults can only read the receiver and earlier parameters",
		     &pd_default->name);
	}
	if (const auto *index = dynamic_cast<const IndexExpression *>(target.get())) {
		const ExpressionInfo ei_array = scalarInfo(*index->array);
		if (ei_array.fallsThrough && !ei_array.types.front().array() && ei_array.types.front().declaration) {
			const ResolvedType &receiver = ei_array.types.front();
			checkFieldWrite(ei_array, target->get_span());
			const auto setters = analyzer.members(receiver, to_string(oIndexWrite), SymbolKind::Operator);
			if (setters.empty()) {
				fail(Code::INVALID_FIELD_TARGET, target->get_span(),
				     std::format("Indexed assignment needs a {} operator", to_string(oIndexWrite)));
			}
			if (compound || rhs) {
				const ExpressionInfo selected = invoke(
				        op, statement.get_span(), { { .expression = index->index }, { .expression = compound ? compound : rhs } }, setters);
				if (record) {
					bf_facts.writes[&statement] = selected;
				}
				return selected.fallsThrough;
			}
			// An indexed postfix update reads the element and stores its updated value through the setter.
			if (const std::optional<ResolvedType> opt_read = scalar(*target); opt_read && !opt_read->integer() && !opt_read->floating()) {
				fail(Code::UNARY_OPERAND, op.get_span(), "Postfix update needs a numeric element");
			}
			const ExpressionInfo selected = invoke(op, statement.get_span(), { { .expression = index->index }, { .expression = target } }, setters);
			if (record) {
				bf_facts.writes[&statement] = selected;
			}
			return selected.fallsThrough;
		}
	}
	const ExpressionInfo ei_target = scalarInfo(*target);
	if (!ei_target.fallsThrough) {
		if (rhs) scalar(*rhs);
		return false;
	}
	checkFieldWrite(ei_target, target->get_span());
	const ResolvedType &rt = ei_target.types.front();
	writable(*target);
	if (compound || !rhs) {
		if (const std::optional<ExpressionInfo> opt_call = updateOperator(statement, rt, op, rhs)) return opt_call->fallsThrough;
	}
	if (compound || rhs) {
		return expect(compound ? *compound : *rhs, rt);
	} else if (!rt.integer() && !rt.floating()) {
		fail(Code::UNARY_OPERAND, op.get_span(), "Update needs a numeric target");
	}
	return true;
}

bool BodyChecker::statement (const Statement &node) {
	if (const auto *s = dynamic_cast<const ExpressionStatement *>(&node)) {
		return expression(*s->expression, nullptr, true).fallsThrough;
	}
	if (const auto *s = dynamic_cast<const ReturnStatement *>(&node)) {
		if (pd_default && !b_nestedFunction) {
			fail(Code::INVALID_PARAMETER_DEFAULT, s->span, std::format("'{}' cannot leave a parameter default", to_string(kReturn)),
			     &pd_default->name);
		}
		if (!b_nestedFunction && rp_receiverPhase != ReceiverPhase::Ready) {
			fail(Code::PARTIAL_CLASS_INSTANCE, s->span, std::format("'{}' cannot leave class initialization", to_string(kReturn)));
		}
		if (!s->value && !returns.empty()) {
			fail(Code::RETURN_VALUE_REQUIRED, s->span, "A return value is required");
		}
		if (s->value) {
			checkReturn(*s->value);
		}
		return false;
	}
	if (const auto *s = dynamic_cast<const BindingDeclaration *>(&node)) {
		std::optional<ResolvedType> annotation;
		if (s->type) {
			annotation = bodyType(*s->type);
		}
		const std::optional<ResolvedType> opt_actual = scalar(*s->initializer, annotation ? &*annotation : nullptr);
		if (annotation && opt_actual) {
			require({ *opt_actual }, { *annotation }, s->initializer->get_span());
		}
		if (!annotation && opt_actual && opt_actual->name == to_string(kNull)) {
			fail(Code::EXPLICIT_NULLABLE_TYPE, s->span, std::format("A {} initializer requires an explicit nullable type", to_string(kNull)));
		}
		bind(s->name, annotation ? annotation : opt_actual, s->isMutable,
		     dynamic_cast<const FunctionBlockExpression *>(s->initializer.get()) != nullptr);
		return opt_actual.has_value();
	} else if (const auto *s = dynamic_cast<const BindingGroupDeclaration *>(&node)) {
		return receive(s->targets, *s->initializer, &s->types, s->isMutable);
	} else if (const auto *s = dynamic_cast<const AssignmentStatement *>(&node)) {
		const Binding                    *binding  = local(s->name.get_lexeme());
		const std::optional<ResolvedType> opt_type = binding ? binding->type : std::optional{ name(s->name).types.front() };
		if (const BinaryExpression *compound = s->get_compound()) {
			const std::optional<ResolvedType> opt_left = scalar(*compound->left);
			if (opt_left) {
				if (const std::optional<ExpressionInfo> opt_update = updateOperator(node, *opt_left, *s->compoundOperator, compound->right)) {
					assign(s->name, opt_type);
					return opt_update->fallsThrough;
				}
			}
		}
		const std::optional<ResolvedType> opt_value = scalar(*s->value, opt_type ? &*opt_type : nullptr);
		assign(s->name, opt_value);
		return opt_value.has_value();
	} else if (const auto *s = dynamic_cast<const MultiAssignmentStatement *>(&node)) {
		return receive(s->targets, *s->value, nullptr);
	} else if (const auto *s = dynamic_cast<const UpdateStatement *>(&node)) {
		const std::optional<ResolvedType> opt_target = scalar(*s->target);
		if (opt_target && !updateOperator(node, *opt_target, s->op, nullptr) && !opt_target->integer() && !opt_target->floating()) {
			fail(Code::UNARY_OPERAND, s->op.get_span(), "Update needs a numeric binding or a matching operator");
		}
		assign(s->name, opt_target);
		return opt_target.has_value();
	} else if (const auto *s = dynamic_cast<const ElementAssignmentStatement *>(&node)) {
		return writeMember(node, s->target, s->op, s->value, s->compound);
	} else if (const auto *s = dynamic_cast<const FieldWriteStatement *>(&node)) {
		return writeMember(node, s->target, s->op, s->value, s->compound);
	} else if (const auto *s = dynamic_cast<const LoopJumpStatement *>(&node)) {
		if (!loopDepth) {
			fail(Code::INVALID_LOOP_JUMP, s->get_span(), "Loop jump outside a loop");
		}
		if (s->isContinue()) {
			fs_flow.loopMayContinue = true;
		} else {
			fs_flow.loopMayBreak = true;
		}
		return false;
	} else if (const auto *s = dynamic_cast<const LoopStatement *>(&node)) {
		++loopDepth;
		// A mutable local can change between iterations; only the loop test establishes fresh facts.
		for (const auto &scope : scopes) {
			for (const auto &entries : scope | std::views::values) {
				if (entries.mutableBinding) {
					fs_flow.narrowed.erase(entries.name);
				}
			}
		}
		const bool outerBreak = std::exchange(fs_flow.loopMayBreak, false), outerContinue = std::exchange(fs_flow.loopMayContinue, false);
		bool       fallsThrough = true;
		if (const auto *f = dynamic_cast<const ForStatement *>(s)) {
			std::optional<ResolvedType> annotation;
			if (f->binding.type) {
				annotation = bodyType(*f->binding.type);
			}
			std::optional<ResolvedType> opt_element;
			if (const ForSource::Range *range = std::get_if<ForSource::Range>(&f->source.value)) {
				opt_element                               = scalar(*range->start, annotation ? &*annotation : nullptr);
				const std::optional<ResolvedType> opt_end = scalar(*range->end, opt_element ? &*opt_element :
				                                                                annotation  ? &*annotation :
				                                                                              nullptr);
				fallsThrough                              = opt_element.has_value() && opt_end.has_value();
				if ((opt_element && !opt_element->integer()) || (opt_end && !opt_end->integer())) {
					fail(Code::INVALID_FOR_LOOP, range->span, "Range endpoint must be an integer");
				}
				if (opt_element && opt_end) require({ *opt_end }, { *opt_element }, range->end->get_span());
				if (!opt_element) opt_element = opt_end;
			} else {
				const std::optional<ResolvedType> opt_source = scalar(*std::get<ForSource::Elements>(f->source.value).expression);
				if (!opt_source) {
					fallsThrough = false;
				} else if (opt_source->array()) {
					opt_element = opt_source->element();
				} else if (opt_source->name == "range" && !opt_source->arguments().empty()) {
					opt_element = opt_source->arguments().front();
				} else {
					fail(Code::INVALID_FOR_LOOP, f->source.get_span(), "Expected an array or range");
				}
			}
			if (annotation && opt_element) {
				require({ *opt_element }, { *annotation }, f->binding.get_span());
			}
			scopes.emplace_back();
			bind(f->binding.name, annotation ? annotation : opt_element, true);
			if (f->stepper) {
				remember(f->stepper->target, f->binding.name);
				if (f->stepper->value) {
					if (opt_element)
						expect(*f->stepper->value, *opt_element);
					else
						scalar(*f->stepper->value);
				}
			}
			block(f->body.statements);
			scopes.pop_back();
		} else {
			const auto          *w    = dynamic_cast<const WhileStatement *>(s);
			const auto          *d    = dynamic_cast<const DoWhileStatement *>(s);
			const ExpressionPtr &test = w ? w->condition : d->condition;
			if (w) {
				fallsThrough = expect(*test, ResolvedType(kBoolean));
				refine(*test, true);
			}
			const ExpressionInfo body            = block(w ? w->body.statements : d->body.statements);
			const bool           b_testContinues = w || expect(*test, ResolvedType(kBoolean));
			const auto          *literal         = dynamic_cast<const BooleanLiteral *>(test.get());
			fallsThrough &= fs_flow.loopMayBreak ||
			                (b_testContinues && !(literal && literal->value) && (w || body.fallsThrough || fs_flow.loopMayContinue));
		}
		--loopDepth;
		for (const auto &scope : scopes) {
			for (const auto &entry : scope | std::views::values) {
				if (entry.mutableBinding) {
					fs_flow.narrowed.erase(entry.name);
				}
			}
		}
		fs_flow.loopMayBreak    = outerBreak;
		fs_flow.loopMayContinue = outerContinue;
		return fallsThrough;
	} else {
		fail(Code::INTERNAL, node.get_span(), "Unrecognized statement in semantic analysis");
	}
	return true;
}

}
