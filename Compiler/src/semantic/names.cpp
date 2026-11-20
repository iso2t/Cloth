// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <utility>

#include <ast/stmt/do_while_statement.hpp>
#include <ast/stmt/for_statement.hpp>
#include <ast/stmt/while_statement.hpp>
#include <semantic/checker.hpp>

namespace cloth::semantic {

void Analyzer::useWidthAnnotation (const TypeReference &reference, const Symbol &owner) {
	if (reference.get_application()) b_hasTypeApplications = true;
	v_widthAnnotations.emplace_back(&reference, &owner);
}

void Analyzer::checkWidthAnnotations () const {
	if (v_widthAnnotations.empty()) return;
	// Generic and dispatch contracts bind type names before specialization. An ordinary body resolves its annotations at its selected width.
	if (m_genericOwners.empty() && !b_hasTypeApplications && std::ranges::none_of(v_order, [&] (const Symbol *symbol) {
		    const DeclarationInfo   &di_info   = declarations.at(symbol);
		    const MethodDeclaration *md_method = dynamic_cast<const MethodDeclaration *>(callable(*symbol));
		    return asInterface(symbol) || di_info.parents.size() > 1 || (symbol->kind == SymbolKind::State && !di_info.owner) ||
		           (md_method && md_method->virtualPrefix);
	    }))
		return;
	for (const std::pair<const TypeReference *, const Symbol *> &entry : v_widthAnnotations) (void)checkedType(*entry.first, *entry.second);
}

// A width template has lexical bindings before there is a peer to supply types for its body.
void BodyChecker::resolveBodyNames (const FunctionBody &body) {
	record = false;
	if (const BlockBody *bb_block = std::get_if<BlockBody>(&body.value))
		resolveNames(bb_block->statements);
	else if (const ExpressionBody *eb_body = std::get_if<ExpressionBody>(&body.value))
		resolveNames(*eb_body->expression);
}

void BodyChecker::resolveNames (const std::vector<StatementPtr> &statements) {
	scopes.emplace_back();
	for (const StatementPtr &sp_statement : statements) resolveNames(*sp_statement);
	scopes.pop_back();
}

void BodyChecker::resolveNames (const Statement &node) {
	if (const BindingDeclaration *bd_local = dynamic_cast<const BindingDeclaration *>(&node)) {
		if (bd_local->type) analyzer.useWidthAnnotation(*bd_local->type, context);
		resolveNames(*bd_local->initializer);
		bind(bd_local->name, {}, bd_local->isMutable, dynamic_cast<const FunctionBlockExpression *>(bd_local->initializer.get()) != nullptr);
	} else if (const BindingGroupDeclaration *bg_group = dynamic_cast<const BindingGroupDeclaration *>(&node)) {
		for (const TypeReference &tr_slot : bg_group->types)
			if (!tr_slot.is_placeholder()) analyzer.useWidthAnnotation(tr_slot, context);
		resolveNames(*bg_group->initializer);
		for (const ReceivingTarget &rt_target : bg_group->targets)
			if (const ReceivingTarget::Named *nt_name = std::get_if<ReceivingTarget::Named>(&rt_target.value))
				bind(nt_name->token, {}, bg_group->isMutable);
	} else if (const ExpressionStatement *es_value = dynamic_cast<const ExpressionStatement *>(&node)) {
		resolveNames(*es_value->expression);
	} else if (const ReturnStatement *rs_return = dynamic_cast<const ReturnStatement *>(&node)) {
		if (rs_return->value) resolveNames(*rs_return->value);
	} else if (const AssignmentStatement *as_write = dynamic_cast<const AssignmentStatement *>(&node)) {
		if (!assignmentBinding(as_write->name)) name(as_write->name, true);
		resolveNames(*as_write->value);
	} else if (const MultiAssignmentStatement *ma_group = dynamic_cast<const MultiAssignmentStatement *>(&node)) {
		for (const ReceivingTarget &rt_target : ma_group->targets) {
			if (const ReceivingTarget::Named *nt_name = std::get_if<ReceivingTarget::Named>(&rt_target.value)) {
				if (!assignmentBinding(nt_name->token)) name(nt_name->token, true);
			}
		}
		resolveNames(*ma_group->value);
	} else if (const LoopStatement *ls_loop = dynamic_cast<const LoopStatement *>(&node)) {
		const LoopTarget *lt_previous = std::exchange(lt_loop, ls_loop->get_loop_target().get());
		if (const ForStatement *fs_for = dynamic_cast<const ForStatement *>(&node)) {
			resolveNames(fs_for->source.get_expression());
			if (fs_for->binding.type) analyzer.useWidthAnnotation(*fs_for->binding.type, context);
			scopes.emplace_back();
			bind(fs_for->binding.name, {}, true);
			if (fs_for->stepper) {
				name(fs_for->stepper->target, true);
				if (fs_for->stepper->value) resolveNames(*fs_for->stepper->value);
			}
			resolveNames(fs_for->body.statements);
			scopes.pop_back();
		} else if (const WhileStatement *ws_while = dynamic_cast<const WhileStatement *>(&node)) {
			resolveNames(*ws_while->condition);
			resolveNames(ws_while->body.statements);
		} else {
			const DoWhileStatement &ds_do = static_cast<const DoWhileStatement &>(node);
			resolveNames(ds_do.body.statements);
			resolveNames(*ds_do.condition);
		}
		lt_loop = lt_previous;
	} else if (dynamic_cast<const LoopJumpStatement *>(&node)) {
		if (!lt_loop) fail(Code::INVALID_LOOP_JUMP, node.get_span(), "Loop jump outside a loop");
	} else {
		const std::pair<ExpressionPtr, ExpressionPtr> ep_operands = updateOperands(node);
		resolveNames(*ep_operands.first);
		if (ep_operands.second) resolveNames(*ep_operands.second);
	}
}

void BodyChecker::resolveNames (const Expression &node) {
	if (const NameExpression *ne_name = dynamic_cast<const NameExpression *>(&node)) {
		name(ne_name->name);
	} else if (const CallSite *cs_call = dynamic_cast<const CallSite *>(&node)) {
		if (const CallExpression *ce_call = dynamic_cast<const CallExpression *>(&node)) {
			const std::vector<Candidate> v_candidates = callCandidates(*ce_call);
			if (ce_call->typeArguments) checkTypeArguments(*ce_call->typeArguments);
			if (v_candidates.empty() && !builtinFunctionNamed(ce_call->name.get_lexeme()))
				fail(Code::UNKNOWN_NAME, ce_call->name.get_span(), "Unknown callable '" + ce_call->name.get_lexeme() + "'");
			// Complete function applications bind before a peer is available. Defaults and placeholders use call-site inference.
			if (ce_call->typeArguments && !v_candidates.empty() && v_candidates.front().symbol->kind == SymbolKind::Function &&
			    !ce_call->typeArguments->hasPlaceholders()) {
				const std::vector<TypeReference> &v_types     = ce_call->typeArguments->arguments;
				const bool                        b_completes = std::ranges::any_of(v_candidates, [&] (const Candidate &candidate) {
					const TypeParameterList *tpl_parameters = typeParameters(*candidate.symbol);
					return tpl_parameters && tpl_parameters->parameters.size() > v_types.size() &&
					       tpl_parameters->parameters[v_types.size()].defaultType.has_value();
				});
				if (!b_completes) {
					for (const TypeReference &tr_argument : v_types)
						if (tr_argument.capturesWidth())
							fail(Code::UNSUPPORTED_TYPE, tr_argument.get_span(),
							     "A closed function application needs a concrete numeric width");
				}
			}
		} else if (const MemberCallExpression *mc_call = dynamic_cast<const MemberCallExpression *>(&node)) {
			resolveNames(*mc_call->receiver);
			if (mc_call->typeArguments) checkTypeArguments(*mc_call->typeArguments);
			if (isSelfReference(*mc_call->receiver) && analyzer.members(selfType(), mc_call->name.get_lexeme(), SymbolKind::Method).empty())
				unknownMember(*mc_call, selfType().spelling());
		} else if (const ClassCreationExpression *ce_new = dynamic_cast<const ClassCreationExpression *>(&node)) {
			const std::optional<ResolvedType> opt_type = receiverType(*ce_new->receiver, true);
			if (!opt_type || (!opt_type->parameter && !asClass(opt_type->declaration)))
				fail(Code::INVALID_CLASS_LIFECYCLE, ce_new->receiver->get_span(), "Construction requires a class type");
		} else if (const StateCallExpression *sc_state = dynamic_cast<const StateCallExpression *>(&node)) {
			resolveNames(*sc_state->receiver);
			if (sc_state->typeArguments) checkTypeArguments(*sc_state->typeArguments);
		}
		validateArgumentLabels(cs_call->suppliedArguments);
		for (const CallArgument &ca_argument : cs_call->suppliedArguments) resolveNames(*ca_argument.expression);
	} else if (const FieldAccessExpression *fa_field = dynamic_cast<const FieldAccessExpression *>(&node)) {
		if (isSelfReference(*fa_field->receiver) || receiverType(*fa_field->receiver))
			field(*fa_field);
		else
			resolveNames(*fa_field->receiver);
	} else if (const CastExpression *ce_cast = dynamic_cast<const CastExpression *>(&node)) {
		resolveNames(*ce_cast->operand);
		const Token *t_name = ce_cast->target.get_bare_identifier();
		if (!t_name || !analyzer.hasStateName(context, t_name->get_lexeme())) analyzer.useWidthAnnotation(ce_cast->target, context);
	} else if (const TypeCheckExpression *tc_test = dynamic_cast<const TypeCheckExpression *>(&node)) {
		resolveNames(*tc_test->operand);
		const Token *t_name = tc_test->target.get_bare_identifier();
		if (tc_test->valueCandidate && !receiverType(*tc_test->valueCandidate))
			resolveNames(*tc_test->valueCandidate);
		else if (!t_name || (!hasValueName(*t_name) && !analyzer.hasStateName(context, t_name->get_lexeme())))
			analyzer.useWidthAnnotation(tc_test->target, context);
	} else if (const TypeReceiverExpression *tr_type = dynamic_cast<const TypeReceiverExpression *>(&node)) {
		analyzer.useWidthAnnotation(tr_type->annotation, context);
	} else if (const ProviderAccessExpression *pa_provider = dynamic_cast<const ProviderAccessExpression *>(&node)) {
		if (!receiverType(*pa_provider->receiver)) resolveNames(*pa_provider->receiver);
	} else if (const FunctionBlockExpression *fb_block = dynamic_cast<const FunctionBlockExpression *>(&node)) {
		if (fb_block->declaration.returnTypes.empty())
			fail(Code::BLOCK_RETURN_TYPE_REQUIRED, node.get_span(), "A deferred binding requires a result type");
		for (const TypeReference &tr_result : fb_block->declaration.returnTypes) analyzer.useWidthAnnotation(tr_result, context);
		const LoopTarget *lt_previous = std::exchange(lt_loop, nullptr);
		resolveBodyNames(fb_block->declaration.body);
		lt_loop = lt_previous;
	} else
		visitOperands(
		        node, [&] (const Expression &operand) { resolveNames(operand); },
		        [&] (const std::vector<StatementPtr> &statements) { resolveNames(statements); });
}

}
