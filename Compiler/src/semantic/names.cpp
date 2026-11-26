// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/language/source/Visibility.rst

#include <algorithm>
#include <format>
#include <map>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <ast/decl/class_declaration.hpp>
#include <ast/decl/declarations.hpp>
#include <ast/decl/state_declaration.hpp>
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
#include <ast/type/type_parameters.hpp>
#include <ast/type/type_reference.hpp>
#include <semantic/checked_program.hpp>
#include <semantic/checker.hpp>
#include <semantic/symbol_table.hpp>
#include <token/token.hpp>

namespace cloth::semantic {

void unknownName (const Token &name) {
	throw cde::ClothDiagnostic(Code::UnknownName, std::format("Unknown name '{}'", name.getLexeme()), { { .span = name.getSpan(), .text = "here" } },
	                           "Declare a visible binding or function with this name.");
}

void Analyzer::useWidthAnnotation (const TypeReference &reference, const Symbol &owner) {
	if (!widthSelector(owner)) return;
	if (reference.getApplication()) b_hasTypeApplications = true;
	v_widthAnnotations.emplace_back(&reference, &owner);
}

bool Analyzer::usesSpecializedDeclarations () const {
	return !m_genericOwners.empty() || b_hasTypeApplications || std::ranges::any_of(v_order, [&] (const Symbol *symbol) {
		const DeclarationInfo &di_info   = declarations.at(symbol);
		const auto            *md_method = dynamic_cast<const MethodDeclaration *>(callable(*symbol));
		return asInterface(symbol) || di_info.parents.size() > 1 || (symbol->kind == SymbolKind::State && !di_info.owner) ||
		       (md_method && md_method->virtualPrefix);
	});
}

void Analyzer::checkWidthAnnotations () const {
	// Generic and dispatch contracts bind type names before specialization. An ordinary body resolves its annotations at its selected width.
	if (v_widthAnnotations.empty() || !usesSpecializedDeclarations()) return;
	for (const std::pair<const TypeReference *, const Symbol *> &entry : v_widthAnnotations) (void)checkedType(*entry.first, *entry.second);
}

void BodyChecker::resolveDeclarationNames () {
	record = false;
	if (const CallableSignature *cs_callable = callable(context)) {
		if (context.kind == SymbolKind::Constructor) rp_receiverPhase = ReceiverPhase::ConstructorDefault;
		for (const ParameterDeclaration &pd_parameter : cs_callable->parameters) {
			if (pd_parameter.defaultValue) {
				BodyChecker bc_default = *this;
				bc_default.pd_default  = &pd_parameter;
				bc_default.resolveNames(*pd_parameter.defaultValue);
			}
			bind(pd_parameter.name, {}, pd_parameter.isMutable());
		}
		if (const auto *cd_constructor = dynamic_cast<const ConstructorDeclaration *>(cs_callable)) {
			rp_receiverPhase = ReceiverPhase::Delegation;
			for (const CallArgument &ca_argument : cd_constructor->delegation.suppliedArguments) resolveNames(*ca_argument.expression);
		}
		rp_receiverPhase = ReceiverPhase::Ready;
		if (const FunctionBody *fb_body = cs_callable->executableBody()) resolveNames(*fb_body);
	} else if (const ClassDeclaration *cd_class = asClass(context.kind == SymbolKind::Type ? &context : owner);
	           cd_class && (context.kind == SymbolKind::Type || context.kind == SymbolKind::Field)) {
		owner = context.kind == SymbolKind::Type ? &context : owner;
		for (const ClassParameterDeclaration &cp_parameter : cd_class->parameters)
			bind(cp_parameter.parameter.name, {}, cp_parameter.parameter.isMutable());
		if (context.kind == SymbolKind::Type) {
			rp_receiverPhase = ReceiverPhase::Delegation;
			for (const ClassParentClause &cp_parent : cd_class->parents)
				if (cp_parent.constructorArguments)
					for (const CallArgument &ca_argument : *cp_parent.constructorArguments) resolveNames(*ca_argument.expression);
		} else if (const ClassFieldDeclaration *const *cf_field = std::get_if<const ClassFieldDeclaration *>(&context.declaration)) {
			rp_receiverPhase = ReceiverPhase::FieldInitializer;
			if ((*cf_field)->initializer) resolveNames(*(*cf_field)->initializer);
		}
	} else if (context.kind == SymbolKind::StateInitializer) {
		resolveNames(std::get<const StateInitializerDeclaration *>(context.declaration)->body);
	} else if (const StateSelectionDeclaration *ss_state = stateSelection(context); ss_state && ss_state->initial && ss_state->initial->payload) {
		resolveNames(*ss_state->initial->payload);
	}
}

void BodyChecker::resolveNames (const FunctionBody &body) {
	if (const BlockBody *bb_block = std::get_if<BlockBody>(&body.value)) {
		for (const StatementPtr &sp_statement : bb_block->statements) resolveNames(*sp_statement);
	} else if (const ExpressionBody *eb_body = std::get_if<ExpressionBody>(&body.value)) {
		resolveNames(*eb_body->expression);
	}
}

void BodyChecker::resolveNames (const std::vector<StatementPtr> &statements) {
	scopes.emplace_back();
	for (const StatementPtr &sp_statement : statements) resolveNames(*sp_statement);
	scopes.pop_back();
}

void BodyChecker::resolveNames (const Statement &node) {
	if (const auto *bd_local = dynamic_cast<const BindingDeclaration *>(&node)) {
		checkBindingName(bd_local->name);
		if (bd_local->type) analyzer.useWidthAnnotation(*bd_local->type, context);
		resolveNames(*bd_local->initializer);
		bind(bd_local->name, {}, bd_local->isMutable, dynamic_cast<const FunctionBlockExpression *>(bd_local->initializer.get()) != nullptr);
	} else if (const auto *bg_group = dynamic_cast<const BindingGroupDeclaration *>(&node)) {
		std::map<std::string_view, const Token *> m_names;
		for (const ReceivingTarget &rt_target : bg_group->targets) {
			if (const ReceivingTarget::Named *nt_name = std::get_if<ReceivingTarget::Named>(&rt_target.value)) {
				checkBindingName(nt_name->token);
				const std::pair<std::map<std::string_view, const Token *>::iterator, bool> it_name =
				        m_names.emplace(nt_name->token.getNameKey(), &nt_name->token);
				if (!it_name.second) duplicateBinding(nt_name->token, *it_name.first->second);
			}
		}
		for (const TypeReference &tr_slot : bg_group->types)
			if (!tr_slot.isPlaceholder()) analyzer.useWidthAnnotation(tr_slot, context);
		resolveNames(*bg_group->initializer);
		const bool b_deferred = dynamic_cast<const FunctionBlockExpression *>(bg_group->initializer.get()) != nullptr;
		for (const ReceivingTarget &rt_target : bg_group->targets)
			if (const ReceivingTarget::Named *nt_name = std::get_if<ReceivingTarget::Named>(&rt_target.value))
				bind(nt_name->token, {}, bg_group->isMutable, b_deferred);
	} else if (const auto *es_value = dynamic_cast<const ExpressionStatement *>(&node)) {
		resolveNames(*es_value->expression);
	} else if (const auto *rs_return = dynamic_cast<const ReturnStatement *>(&node)) {
		checkReturnContext(*rs_return);
		if (rs_return->value) resolveNames(*rs_return->value);
	} else if (const auto *as_write = dynamic_cast<const AssignmentStatement *>(&node)) {
		assignmentTarget(as_write->name);
		resolveNames(*as_write->value);
	} else if (const auto *ma_group = dynamic_cast<const MultiAssignmentStatement *>(&node)) {
		for (const ReceivingTarget &rt_target : ma_group->targets) {
			if (const ReceivingTarget::Named *nt_name = std::get_if<ReceivingTarget::Named>(&rt_target.value)) {
				assignmentTarget(nt_name->token);
			}
		}
		resolveNames(*ma_group->value);
	} else if (const auto *ls_loop = dynamic_cast<const LoopStatement *>(&node)) {
		const LoopTarget *lt_previous = std::exchange(lt_loop, ls_loop->getLoopTarget().get());
		if (const auto *fs_for = dynamic_cast<const ForStatement *>(&node)) {
			resolveNames(fs_for->source.getExpression());
			if (fs_for->binding.type) analyzer.useWidthAnnotation(*fs_for->binding.type, context);
			scopes.emplace_back();
			bind(fs_for->binding.name, {}, true);
			if (fs_for->stepper) {
				assignmentTarget(fs_for->stepper->target);
				if (fs_for->stepper->value) resolveNames(*fs_for->stepper->value);
			}
			resolveNames(fs_for->body.statements);
			scopes.pop_back();
		} else if (const auto *ws_while = dynamic_cast<const WhileStatement *>(&node)) {
			resolveNames(*ws_while->condition);
			resolveNames(ws_while->body.statements);
		} else {
			const auto &ds_do = static_cast<const DoWhileStatement &>(node);
			resolveNames(ds_do.body.statements);
			resolveNames(*ds_do.condition);
		}
		lt_loop = lt_previous;
	} else if (const auto *us_update = dynamic_cast<const UpdateStatement *>(&node)) {
		assignmentTarget(us_update->name);
	} else if (const auto *lj_jump = dynamic_cast<const LoopJumpStatement *>(&node)) {
		if (!lt_loop)
			throw cde::ClothDiagnostic(Code::InvalidLoopJump, std::format("'{}' requires an enclosing loop body", lj_jump->keyword.getLexeme()),
			                           { { .span = lj_jump->keyword.getSpan(), .text = "here" } });
	} else {
		const std::pair<ExpressionPtr, ExpressionPtr> ep_operands = updateOperands(node);
		resolveNames(*ep_operands.first);
		if (ep_operands.second) resolveNames(*ep_operands.second);
	}
}

void BodyChecker::resolveCallName (const CallExpression &call) {
	validateArgumentLabels(call.suppliedArguments);
	const std::vector<Candidate> v_candidates = callCandidates(call);
	if (v_candidates.empty() && builtinFunctionNamed(call.name.getNameKey())) {
		if (call.typeArguments) checkBuiltinTypeArguments(call, *call.typeArguments);
		return;
	}
	if (call.typeArguments) checkTypeArguments(*call.typeArguments);
	if (v_candidates.empty()) unknownName(call.name);
	if (!call.typeArguments || v_candidates.front().symbol->kind != SymbolKind::Function) return;

	const std::vector<TypeReference> &v_types   = call.typeArguments->arguments;
	bool                              b_generic = false, b_accepts = false, b_usesDefault = false;
	for (const Candidate &c_candidate : v_candidates) {
		const TypeParameterList *tpl_parameters = typeParameters(*c_candidate.symbol);
		if (!tpl_parameters) continue;
		b_generic = true;
		if (acceptsTypeArgumentCount(tpl_parameters, v_types.size())) {
			b_accepts = true;
			b_usesDefault |= v_types.size() < tpl_parameters->parameters.size();
		}
	}
	if (!b_accepts) rejectTypeArgumentCount(*call.typeArguments, *v_candidates.front().symbol, b_generic);
	// Complete function applications bind before argument inference supplies a width.
	if (b_usesDefault || call.typeArguments->hasPlaceholders()) return;
	for (const TypeReference &tr_argument : v_types)
		if (tr_argument.capturesWidth())
			fail(Code::UnsupportedType, tr_argument.getSpan(), "A closed function application needs a concrete numeric width");
}

void BodyChecker::resolveNames (const Expression &node) {
	if (const auto *ne_name = dynamic_cast<const NameExpression *>(&node)) {
		name(ne_name->name, NameUse::Lexical);
	} else if (const auto *cs_call = dynamic_cast<const CallSite *>(&node)) {
		if (const auto *ce_call = dynamic_cast<const CallExpression *>(&node)) {
			resolveCallName(*ce_call);
		} else if (const auto *mc_call = dynamic_cast<const MemberCallExpression *>(&node)) {
			resolveNames(*mc_call->receiver);
			if (mc_call->typeArguments) checkTypeArguments(*mc_call->typeArguments);
			if (isSelfReference(*mc_call->receiver)) {
				const ResolvedType self = selfType();
				if (analyzer.members(self, mc_call->name.getNameKey(), SymbolKind::Method).empty() &&
				    (analyzer.primitiveOwner(self) || !builtinMethodNamed(mc_call->name.getNameKey(), self)))
					throw cde::ClothDiagnostic(Code::UnknownMember,
					                           std::format("Type '{}' has no member method '{}'", selfType().spelling(),
					                                       mc_call->name.getLexeme()),
					                           { { .span = mc_call->name.getSpan(), .text = "here" } });
				validateArgumentLabels(cs_call->suppliedArguments);
			}
		} else if (const auto *ce_new = dynamic_cast<const ClassCreationExpression *>(&node)) {
			const std::optional<ResolvedType> opt_type = receiverType(*ce_new->receiver, true);
			if (!opt_type || (!opt_type->parameter && !asClass(opt_type->declaration)))
				fail(Code::InvalidClassLifecycle, ce_new->receiver->getSpan(), "Construction requires a class type");
			validateArgumentLabels(cs_call->suppliedArguments);
		} else if (const auto *sc_state = dynamic_cast<const StateCallExpression *>(&node)) {
			validateArgumentLabels(cs_call->suppliedArguments);
			resolveNames(*sc_state->receiver);
			if (sc_state->typeArguments) checkTypeArguments(*sc_state->typeArguments);
		}
		for (const CallArgument &ca_argument : cs_call->suppliedArguments) resolveNames(*ca_argument.expression);
	} else if (const auto *fa_field = dynamic_cast<const FieldAccessExpression *>(&node)) {
		if (isSelfReference(*fa_field->receiver) || receiverType(*fa_field->receiver))
			field(*fa_field);
		else
			resolveNames(*fa_field->receiver);
	} else if (const auto *ce_cast = dynamic_cast<const CastExpression *>(&node)) {
		resolveNames(*ce_cast->operand);
		const Token *t_name = ce_cast->target.getBareIdentifier();
		if (!t_name || !analyzer.hasStateName(context, t_name->getNameKey())) analyzer.useWidthAnnotation(ce_cast->target, context);
	} else if (const auto *tc_test = dynamic_cast<const TypeCheckExpression *>(&node)) {
		const Token *t_name = tc_test->target.getBareIdentifier();
		if (t_name && !tc_test->valueCandidate && !hasValueName(*t_name) && !analyzer.hasTypeName(context, *t_name) &&
		    initialStateTest(*tc_test, t_name))
			return;
		resolveNames(*tc_test->operand);
		if (tc_test->valueCandidate && !receiverType(*tc_test->valueCandidate))
			resolveNames(*tc_test->valueCandidate);
		else if (!t_name || (!hasValueName(*t_name) && !analyzer.hasStateName(context, t_name->getNameKey())))
			analyzer.useWidthAnnotation(tc_test->target, context);
	} else if (const auto *tr_type = dynamic_cast<const TypeReceiverExpression *>(&node)) {
		analyzer.useWidthAnnotation(tr_type->annotation, context);
	} else if (const auto *pa_provider = dynamic_cast<const ProviderAccessExpression *>(&node)) {
		if (!receiverType(*pa_provider->receiver)) resolveNames(*pa_provider->receiver);
	} else if (const auto *fb_block = dynamic_cast<const FunctionBlockExpression *>(&node)) {
		if (fb_block->declaration.returnTypes.empty()) fail(Code::BlockReturnTypeRequired, node.getSpan(), "A deferred binding requires a result type");
		for (const TypeReference &tr_result : fb_block->declaration.returnTypes) analyzer.useWidthAnnotation(tr_result, context);
		BodyChecker bc_deferred       = *this;
		bc_deferred.lt_loop           = nullptr;
		bc_deferred.t_deferredBinding = &fb_block->declaration.name;
		bc_deferred.scopes.emplace_back();
		bc_deferred.resolveNames(fb_block->declaration.body);
	} else
		visitOperands(
		        node, [&] (const Expression &operand) { resolveNames(operand); },
		        [&] (const std::vector<StatementPtr> &statements) { resolveNames(statements); });
}

}
