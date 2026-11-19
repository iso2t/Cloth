// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <format>
#include <functional>
#include <ranges>

#include <ast/decl/enum_declaration.hpp>
#include <ast/stmt/do_while_statement.hpp>
#include <ast/stmt/for_statement.hpp>
#include <ast/stmt/statements.hpp>
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
	for (ResolvedType &rt_result : returns)
		if (rt_result.parameter) rt_result = analyzer.boundShape(rt_result);
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
	if (type && type->parameter) type = analyzer.boundShape(*type);
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
	if (const auto *fa_field = dynamic_cast<const FieldAccessExpression *>(&node)) return readOnlyDefaultRoot(*fa_field->receiver);
	if (const auto *ie_index = dynamic_cast<const IndexExpression *>(&node)) return readOnlyDefaultRoot(*ie_index->array);
	if (const auto *ce_cast = dynamic_cast<const CastExpression *>(&node)) return readOnlyDefaultRoot(*ce_cast->operand);
	if (const auto *gr_receiver = dynamic_cast<const GuardedReceiver *>(&node)) return readOnlyDefaultRoot(*gr_receiver->source);
	if (const auto *ga_access = dynamic_cast<const GuardedAccessExpression *>(&node)) return readOnlyDefaultRoot(*ga_access->continuation);
	const auto *ne_name = dynamic_cast<const NameExpression *>(&node);
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

ResolvedType BodyChecker::bodyType (const TypeReference &reference, const bool allowOpen) const {
	ResolvedType rt_type = analyzer.checkedType(reference, context, m_arguments, allowOpen);
	if (record && !allowOpen) analyzer.useType(rt_type, context, reference.get_span());
	if (rt_type.parameter) return analyzer.boundShape(rt_type);
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
	if (context.kind == SymbolKind::Operator && context.name.get_operator() == oIndexValidate && target.receiverFieldPath == ReceiverFieldPath::Stored)
		fail(Code::INVALID_OPERATOR_DECLARATION, span, "An index validator can only read its receiver", &context.name);
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

bool BodyChecker::checkReturn (const Expression &node) {
	ExpressionInfo ei_result;
	try {
		ei_result = expression(node, &returns);
	} catch (const cde::ClothDiagnostic &error) {
		throw ReturnDiagnostic(error);
	}
	if (!ei_result.fallsThrough) return false;
	if (returns.empty() && !ei_result.types.empty()) {
		fail(Code::VALUE_IN_VOID, node.get_span(), "This callable declares no results", &context.name);
	}
	if (ei_result.types.size() != returns.size()) {
		const Code dc_code = returns.size() == 1 && ei_result.types.empty() ? Code::RETURN_TYPE : Code::RESULT_ARITY;
		fail(dc_code, node.get_span(), std::format("Expected {} result(s), found {}", returns.size(), ei_result.types.size()), &context.name);
	}
	const auto *rl_list = dynamic_cast<const ResultListExpression *>(&node);
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
	return true;
}

void BodyChecker::checkBody (const FunctionBody &body) {
	if (const BlockBody *block = std::get_if<BlockBody>(&body.value)) {
		bool                       fallsThrough = true;
		std::optional<BodyChecker> unreachable;
		for (const StatementPtr &stmt : block->statements) {
			const bool next = onPath(fallsThrough, unreachable).statement(*stmt);
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
	WriteEffects we_initialization;
	if (context.kind == SymbolKind::Constructor || context.kind == SymbolKind::Field || context.kind == SymbolKind::State ||
	    context.kind == SymbolKind::StateAttachment)
		we_capture = &we_initialization;
	typeBody();
	const Symbol *sym_state = owner && owner->kind == SymbolKind::State ? owner : nullptr;
	analyzer.recordLifetimeBody({ .context = &context,
	                              .facts   = &bf_facts,
	                              .host    = rt_attachment ? std::optional{ *rt_attachment } :
	                                         owner         ? std::optional{ selfType() } :
	                                                         std::nullopt,
	                              .state   = sym_state },
	                            we_initialization.sharedFields);
}

void BodyChecker::typeBody () {
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
				WriteEffects we_effects;
				bc_default.we_capture = &we_effects;
				const bool b_normal   = bc_default.expect(*param.defaultValue, info.parameters[i]);
				if (record)
					bf_facts.defaultEffects.insert_or_assign(&param, DefaultEffects{ .fallsThrough    = b_normal,
					                                                                 .mayCallOrMutate = we_effects.sharedFields });
			}
			bind(param.name, info.parameters[i], param.isMutable());
			receiveShape(param.name, info.parameters[i], info.parameters[i], param.name.get_span());
		}
		if (const auto *cd_constructor = dynamic_cast<const ConstructorDeclaration *>(signature)) {
			rp_receiverPhase = ReceiverPhase::Delegation;
			const Candidate              c_primary{ .symbol    = analyzer.declarations.at(owner).primaryConstructor,
				                                .arguments = analyzer.argumentsOf(selfType()) };
			const ConstructorDelegation &cd_delegation = cd_constructor->delegation;
			const ExpressionInfo         ei_call = bindCall(cd_delegation.name, cd_delegation.span, cd_delegation.suppliedArguments, { c_primary });
			if (record) {
				bf_facts.expressions[&cd_delegation] = ei_call;
				di_signature.delegationPreservesOwnership = we_capture && !we_capture->sharedFields && ei_call.fallsThrough &&
				                                            ei_call.omittedParameters.empty();
			}
			// Arguments to primary delegation are evaluated before the new receiver is available.
			if (ei_call.fallsThrough) invalidateShared();
			if (we_capture) we_capture->sharedFields = false;
		}
		rp_receiverPhase = ReceiverPhase::Ready;
		if (const FunctionBody *body = signature->executableBody()) {
			if (info.inferResult) {
				const ExpressionBody &inferred  = std::get<ExpressionBody>(body->value);
				const ExpressionInfo  ei_result = scalarInfo(*inferred.expression);
				if (!ei_result.fallsThrough || ei_result.types.front() == ResolvedType(kNull))
					fail(Code::INVALID_OPERATOR_DECLARATION, inferred.expression->get_span(), "An operator needs an inferable result type",
					     &context.name);
				di_signature.returns     = ei_result.types;
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
			if (cd->parents[i].constructorArguments && info.parents[i].declaration != &objectRoot()) {
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
			enumValue(**field, *(*field)->defaultValue, nullptr);
		}
	} else if (context.kind == SymbolKind::EnumCase) {
		const EnumCaseDeclaration &ec = *std::get<const EnumCaseDeclaration *>(context.declaration);
		if (ec.values.size() > ed->fields.size()) {
			fail(Code::INVALID_ENUM_MEMBER, ec.name.get_span(), "Too many enum payload values");
		}
		if (record) bf_facts.enumPayloads.try_emplace(&context);
		for (std::size_t i = 0; i < ed->fields.size(); ++i) {
			if (i < ec.values.size()) {
				enumValue(ed->fields[i], *ec.values[i], &ec.name);
			} else if (!ed->fields[i].defaultValue) {
				fail(Code::INVALID_ENUM_MEMBER, ec.name.get_span(), "Missing enum payload value", &ed->fields[i].name);
			}
			if (record) bf_facts.enumPayloads[&context].push_back((i < ec.values.size() ? ec.values[i] : ed->fields[i].defaultValue).get());
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

BodyChecker &BodyChecker::onPath (const bool reachable, std::optional<BodyChecker> &unreachable) {
	if (reachable) return *this;
	// Keep checking later source without letting its writes or jumps escape an exited path.
	if (!unreachable) {
		unreachable.emplace(*this);
		unreachable->we_capture = nullptr;
	}
	return *unreachable;
}

void BodyChecker::leaveScope () {
	if (const std::map<std::string, Binding> &m_locals = scopes.back(); !m_locals.empty()) {
		const std::function forget = [&] (Narrowing &facts) {
			std::erase_if(facts, [&] (const Narrowing::value_type &entry) {
				const Token &t_root   = *entry.first.root;
				const auto   it_local = m_locals.find(t_root.get_lexeme());
				return it_local != m_locals.end() && it_local->second.name == &t_root;
			});
		};
		forget(fs_flow.narrowed);
		for (JumpPaths *jp_edges : { &fs_flow.exits.breaks, &fs_flow.exits.continues })
			for (JumpPaths::value_type &entry : *jp_edges) forget(entry.second);
	}
	scopes.pop_back();
}

ExpressionInfo BodyChecker::block (const std::vector<StatementPtr> &statements, const ResultTypes *expected, const bool value) {
	scopes.emplace_back();

	struct ScopeExit {
		BodyChecker &checker;

		~ScopeExit () {
			checker.leaveScope();
		}
	} scopeExit{ *this };

	ExpressionInfo             result;
	std::optional<BodyChecker> unreachable;
	for (const StatementPtr &stmt : statements) {
		if (!result.fallsThrough) {
			onPath(false, unreachable).statement(*stmt);
			continue;
		}
		if (value && &stmt == &statements.back()) {
			if (const auto *last = dynamic_cast<const ExpressionStatement *>(stmt.get())) {
				const Exits ex_incoming = std::exchange(fs_flow.exits, {});
				result                  = expression(*last->expression, expected);
				finishStatement(*last, result.fallsThrough, ex_incoming);
				continue;
			}
		}
		const bool next = statement(*stmt);
		result.fallsThrough &= next;
	}
	return result;
}

ExpressionInfo BodyChecker::condition (const IfExpression &node, const ResultTypes *expected, const bool discard) {
	std::optional<ResultTypes>                              opt_common, shapes;
	std::optional<std::vector<std::optional<ResolvedType>>> opt_metadata;
	std::optional<Narrowing>                                opt_joined;
	Exits                                                   exits;
	const std::function branch = [&] (const std::vector<StatementPtr> &statements, const TokenSpan &span, bool reachable) {
		BodyChecker bc_branch   = *this;
		bc_branch.fs_flow.exits = {};
		if (!reachable) bc_branch.we_capture = nullptr;
		const ExpressionInfo ei_result = bc_branch.block(statements, expected, !discard);
		if (reachable) {
			mergeExits(exits, bc_branch.fs_flow.exits);
			if (ei_result.fallsThrough) {
				join(opt_joined, bc_branch.fs_flow.narrowed);
				if (!opt_metadata) {
					opt_metadata.emplace();
					for (std::size_t i = 0; i < ei_result.types.size(); ++i) opt_metadata->push_back(ei_result.representedAt(i));
				} else if (opt_metadata->size() == ei_result.types.size()) {
					for (std::size_t i = 0; i < opt_metadata->size(); ++i)
						if ((*opt_metadata)[i] != ei_result.representedAt(i)) (*opt_metadata)[i].reset();
				}
				const ResultTypes &next = ei_result.arrayShapes.empty() ? ei_result.types : ei_result.arrayShapes;
				if (!shapes)
					shapes = next;
				else if (shapes->size() == next.size())
					for (std::size_t i = 0; i < next.size(); ++i) (*shapes)[i] = commonShape((*shapes)[i], next[i]);
			}
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
		BodyChecker bc_test   = *this;
		bc_test.fs_flow.exits = {};
		if (!b_remaining) bc_test.we_capture = nullptr;
		const TestPaths tp_paths = bc_test.test(*cb_branch.condition);
		if (b_remaining) mergeExits(exits, bc_test.fs_flow.exits);
		fs_flow.narrowed = tp_paths.yes.value_or(bc_test.fs_flow.narrowed);
		branch(cb_branch.statements, cb_branch.span, b_remaining && tp_paths.yes.has_value());
		fs_flow.narrowed = tp_paths.no.value_or(bc_test.fs_flow.narrowed);
		b_remaining &= tp_paths.no.has_value();
	}
	if (node.otherwise) {
		branch(*node.otherwise, *node.otherwiseSpan, b_remaining);
	} else if (!discard && (b_remaining || opt_joined)) {
		fail(Code::MISSING_ELSE, node.get_span(), std::format("A conditional value requires {}", to_string(kElse)));
	} else if (b_remaining) {
		join(opt_joined, fs_flow.narrowed);
	}
	mergeExits(fs_flow.exits, exits);
	if (!opt_joined) return ExpressionInfo::never();
	fs_flow.narrowed = std::move(*opt_joined);
	if (shapes && !std::ranges::any_of(*shapes, hasArrayShape)) shapes.reset();
	ExpressionInfo ei_joined{ .types = opt_common.value_or(ResultTypes{}), .arrayShapes = shapes.value_or(ResultTypes{}) };
	if (opt_metadata && std::ranges::any_of(*opt_metadata, &std::optional<ResolvedType>::has_value)) {
		if (opt_metadata->size() == 1)
			ei_joined.representedType = opt_metadata->front();
		else
			ei_joined.representedResults = std::move(*opt_metadata);
	}
	return ei_joined;
}

void BodyChecker::assign (const Token &target, const std::optional<ResolvedType> &value, const Expression *source, std::size_t slot,
                          const std::optional<ResolvedType> &represented) {
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
		invalidate({ b_local->name });
		if (represented) fs_flow.narrowed.insert_or_assign({ b_local->name, {}, PlaceFact::RepresentedType }, *represented);
		if (b_local->type) {
			setPresence({ b_local->name }, *b_local->type, value);
			if (value && source) receiveShape(*b_local->name, *b_local->type, shapeOf(*source, *value, slot), source->get_span());
		}
	} else if (value) {
		const std::optional<TypedPlace> tp_target = locate(NameExpression(target));
		if (tp_target) {
			require({ *value }, { tp_target->type }, target.get_span());
			invalidate(tp_target->place);
			setPresence(tp_target->place, tp_target->type, value);
		}
	}
}

void BodyChecker::checkInferredBinding (const ResolvedType &type, const TokenSpan &span) const {
	ResolvedType rt_element = type;
	rt_element.suffixes.clear();
	if (analyzer.mayBeAbsent(rt_element) || std::ranges::find(type.suffixes, to_string(oQuestion)) != type.suffixes.end())
		fail(Code::EXPLICIT_NULLABLE_TYPE, span, "A nullable initializer requires an explicit destination type");
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
			if (!required) {
				const ResultListExpression *rl_values = dynamic_cast<const ResultListExpression *>(&producer);
				checkInferredBinding(actual, rl_values && !broadcast ? rl_values->values[i]->get_span() : producer.get_span());
			}
			bind(target, required ? *required : actual, mutableBinding);
			receiveShape(target, required ? *required : actual, shapeOf(producer, actual, broadcast ? 0 : i), producer.get_span());
			if (const std::optional<ResolvedType> opt_meta = result.representedAt(broadcast ? 0 : i))
				fs_flow.narrowed.insert_or_assign({ &target, {}, PlaceFact::RepresentedType }, *opt_meta);
			if (required) setPresence({ .root = &target }, *required, actual);
		} else
			assign(target, actual, &producer, broadcast ? 0 : i, result.representedAt(broadcast ? 0 : i));
	}
	return true;
}

void BodyChecker::writable (const Expression &target) {
	if (const auto *field = dynamic_cast<const FieldAccessExpression *>(&target)) {
		std::optional<ExpressionInfo> opt_probe;
		if (!record) {
			BodyChecker bc_probe = *this;
			bc_probe.we_capture  = nullptr;
			opt_probe            = bc_probe.field(*field);
		}
		const ExpressionInfo &selected = record ? bf_facts.expressions.at(field) : *opt_probe;
		if (!selected.symbol || selected.symbol->kind != SymbolKind::Field ||
		    std::holds_alternative<const EnumFieldDeclaration *>(selected.symbol->declaration)) {
			fail(Code::INVALID_FIELD_TARGET, field->name.get_span(), "Enum cases and payload fields are read-only");
		}
		if (!std::holds_alternative<const StructFieldDeclaration *>(selected.symbol->declaration)) {
			return;
		}
		writable(*field->receiver);
	} else if (const auto *index = dynamic_cast<const IndexExpression *>(&target)) {
		const decltype(bf_facts.expressions)::const_iterator it = bf_facts.expressions.find(&target);
		if (record && it != bf_facts.expressions.end() && it->second.indexed)
			fail(Code::INVALID_INDEX, target.get_span(), "An index getter returns a copy");
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
	ExpressionInfo selected;
	try {
		selected = invoke(op, statement.get_span(), arguments, candidates);
	} catch (const cde::ClothDiagnostic &error) {
		if (error.get_code() == Code::NO_MATCHING_CALL || error.get_code() == Code::CALL_ARITY) return {};
		if (error.get_code() == Code::AMBIGUOUS_CALL) fail(Code::OPERATOR_MATCH, op.get_span(), "Ambiguous update operator '" + op.get_lexeme() + "'");
		throw;
	}
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
	BodyChecker                    bc_store = *this;
	std::optional<CapturedOperand> opt_array;
	if (const IndexExpression *index = dynamic_cast<const IndexExpression *>(target.get())) {
		const ExpressionInfo ei_array = scalarInfo(*index->array);
		if (ei_array.fallsThrough && !ei_array.types.front().array() && ei_array.types.front().declaration)
			return writeIndex(statement, target, ei_array, op, rhs, compound);
		opt_array.emplace(CapturedOperand{ index->array.get(), ei_array, captured });
		bc_store.captured = &*opt_array;
		bc_store.fs_flow  = fs_flow;
	}
	const ExpressionInfo  ei_target = bc_store.scalarInfo(*target);
	const CapturedOperand co_target{ target.get(), ei_target, bc_store.captured };
	bc_store.captured      = &co_target;
	const bool b_continues = bc_store.writeStorage(statement, target, ei_target, op, rhs, compound);
	fs_flow                = std::move(bc_store.fs_flow);
	return b_continues;
}

bool BodyChecker::writeStorage (const Statement &statement, const ExpressionPtr &target, const ExpressionInfo &ei_target, const Token &op,
                                const ExpressionPtr &rhs, const ExpressionPtr &compound) {
	if (!ei_target.fallsThrough) {
		if (rhs) {
			BodyChecker bc_unreachable = *this;
			bc_unreachable.we_capture  = nullptr;
			bc_unreachable.scalar(*rhs);
		}
		return false;
	}
	checkFieldWrite(ei_target, target->get_span());
	const ResolvedType &rt = ei_target.types.front();
	writable(*target);
	if (compound || !rhs) {
		if (const std::optional<ExpressionInfo> opt_call = updateOperator(statement, rt, op, rhs)) {
			if (opt_call->fallsThrough) mutatingReceiver(*target, rt);
			return opt_call->fallsThrough;
		}
	}
	if (compound || rhs) {
		const std::optional<TypedPlace> tp_target  = locate(*target);
		const ResolvedType             &rt_storage = tp_target ? tp_target->type : rt;
		const Expression               &e_value    = compound ? *compound : *rhs;
		WriteEffects                    we_rhs;
		BodyChecker                     bc_value    = *this;
		bc_value.we_capture                         = &we_rhs;
		const std::optional<ResolvedType> opt_value = bc_value.scalar(e_value, &rt_storage);
		fs_flow                                     = std::move(bc_value.fs_flow);
		if (we_capture) {
			we_capture->places.insert(we_rhs.places.begin(), we_rhs.places.end());
			we_capture->receivers.insert(we_rhs.receivers.begin(), we_rhs.receivers.end());
			we_capture->sharedFields |= we_rhs.sharedFields;
		}
		if (opt_value) {
			require({ *opt_value }, { rt_storage }, e_value.get_span());
			const ResolvedType shape = shapeOf(e_value, *opt_value);
			const TokenSpan    span  = e_value.get_span();
			if (!ei_target.arrayShapes.empty()) refineShape(ei_target.arrayShapes.front(), shape, &span);
			rowShape(*target, e_value, shape);
		}
		if (tp_target) {
			invalidate(tp_target->place);
			// The RHS may have redirected the path since its receiver was captured.
			if (we_rhs.places.empty() && we_rhs.receivers.empty() && !we_rhs.sharedFields) setPresence(tp_target->place, rt_storage, opt_value);
		} else
			invalidateShared();
		return opt_value.has_value();
	} else if (!analyzer.numeric(rt)) {
		fail(Code::UNARY_OPERAND, op.get_span(), "Update needs a numeric target");
	}
	return true;
}

bool BodyChecker::checkStatement (const Statement &node) {
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
			if (checkReturn(*s->value)) fs_flow.exits.returned = true;
		} else
			fs_flow.exits.returned = true;
		return false;
	}
	if (const auto *s = dynamic_cast<const BindingDeclaration *>(&node)) {
		std::optional<ResolvedType> annotation;
		if (s->type) {
			annotation = bodyType(*s->type);
		}
		const ExpressionInfo              ei_value   = scalarInfo(*s->initializer, annotation ? &*annotation : nullptr);
		const std::optional<ResolvedType> opt_actual = ei_value.fallsThrough ? std::optional{ ei_value.types.front() } : std::nullopt;
		if (annotation && opt_actual) {
			require({ *opt_actual }, { *annotation }, s->initializer->get_span());
		}
		if (!annotation && opt_actual) checkInferredBinding(*opt_actual, s->initializer->get_span());
		bind(s->name, annotation ? annotation : opt_actual, s->isMutable,
		     dynamic_cast<const FunctionBlockExpression *>(s->initializer.get()) != nullptr);
		if (opt_actual && !dynamic_cast<const FunctionBlockExpression *>(s->initializer.get()))
			receiveShape(s->name, annotation ? *annotation : *opt_actual, shapeOf(*s->initializer, *opt_actual), s->initializer->get_span());
		if (ei_value.representedType) fs_flow.narrowed.insert_or_assign({ &s->name, {}, PlaceFact::RepresentedType }, *ei_value.representedType);
		if (annotation) setPresence({ &s->name }, *annotation, opt_actual);
		if (dynamic_cast<const FunctionBlockExpression *>(s->initializer.get())) scopes.back().at(s->name.get_lexeme()).effects = we_deferred;
		return opt_actual.has_value();
	}
	if (const auto *s = dynamic_cast<const BindingGroupDeclaration *>(&node)) {
		return receive(s->targets, *s->initializer, &s->types, s->isMutable);
	}
	if (const auto *s = dynamic_cast<const AssignmentStatement *>(&node)) {
		const Binding                    *binding  = local(s->name.get_lexeme());
		const std::optional<TypedPlace>   tp_field = binding ? std::nullopt : locate(NameExpression(s->name));
		const std::optional<ResolvedType> opt_type = binding ? binding->type : tp_field ? tp_field->type : name(s->name).types.front();
		if (const BinaryExpression *compound = s->get_compound()) {
			if (const std::optional<ResolvedType> opt_left = scalar(*compound->left)) {
				if (const std::optional<ExpressionInfo> opt_update = updateOperator(node, *opt_left, *s->compoundOperator, compound->right)) {
					assign(s->name, opt_left);
					return opt_update->fallsThrough;
				}
			}
		}
		const ExpressionInfo ei_value = scalarInfo(*s->value, opt_type ? &*opt_type : nullptr);
		assign(s->name, ei_value.fallsThrough ? std::optional{ ei_value.types.front() } : std::nullopt, s->value.get(), 0, ei_value.representedType);
		return ei_value.fallsThrough;
	}
	if (const auto *s = dynamic_cast<const MultiAssignmentStatement *>(&node)) {
		return receive(s->targets, *s->value, nullptr);
	}
	if (const auto *s = dynamic_cast<const UpdateStatement *>(&node)) {
		const std::optional<ResolvedType> opt_target = scalar(*s->target);
		if (opt_target && !updateOperator(node, *opt_target, s->op, nullptr) && !analyzer.numeric(*opt_target)) {
			fail(Code::UNARY_OPERAND, s->op.get_span(), "Update needs a numeric binding or a matching operator");
		}
		assign(s->name, opt_target);
		return opt_target.has_value();
	}
	if (const auto *s = dynamic_cast<const ElementAssignmentStatement *>(&node)) {
		return writeMember(node, s->target, s->op, s->value, s->compound);
	}
	if (const auto *s = dynamic_cast<const FieldWriteStatement *>(&node)) {
		return writeMember(node, s->target, s->op, s->value, s->compound);
	}
	if (const auto *s = dynamic_cast<const LoopJumpStatement *>(&node)) {
		if (!lt_loop) fail(Code::INVALID_LOOP_JUMP, s->get_span(), "Loop jump outside a loop");
		(s->isContinue() ? fs_flow.exits.continues : fs_flow.exits.breaks).emplace(lt_loop, fs_flow.narrowed);
		if (record) bf_facts.jumpTargets.insert_or_assign(s, lt_loop);
		return false;
	}
	if (const auto *ls_loop = dynamic_cast<const LoopStatement *>(&node)) {
		return loop(*ls_loop);
	}
	fail(Code::INTERNAL, node.get_span(), "Unrecognized statement in semantic analysis");
	return true;
}
}
