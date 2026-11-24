// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <ast/expr/expression.hpp>
#include <ast/expr/expressions.hpp>
#include <ast/expr/member_call_expression.hpp>
#include <ast/stmt/do_while_statement.hpp>
#include <ast/stmt/for_statement.hpp>
#include <ast/stmt/loop_control.hpp>
#include <ast/stmt/statement.hpp>
#include <ast/stmt/statements.hpp>
#include <ast/stmt/while_statement.hpp>
#include <semantic/checked_program.hpp>
#include <semantic/checker.hpp>
#include <semantic/symbol_table.hpp>
#include <token/token.hpp>

namespace cloth::semantic {
namespace {
// A declaration inside the loop can shadow an incoming proof. Only a deferred binding can write outer storage when read.
void shadow (std::map<std::string, Binding> &scope, const Token &name, std::shared_ptr<const WriteEffects> writes = {}) {
	scope.try_emplace(name.getNameKey(),
	                  Binding{ .type = {}, .name = &name, .mutableBinding = false, .deferred = bool(writes), .presenceWrites = std::move(writes) });
}
}

BodyChecker::SourceFlow BodyChecker::SourceFlow::then (SourceFlow next) const {
	if (!fallsThrough) return *this;
	next.breaks |= breaks;
	next.continues |= continues;
	return next;
}

BodyChecker::SourceFlow BodyChecker::SourceFlow::either (const SourceFlow &other) const {
	return { .fallsThrough = fallsThrough || other.fallsThrough, .breaks = breaks || other.breaks, .continues = continues || other.continues };
}

BodyChecker::SourceFlow BodyChecker::collectWrites (const Expression &node, WriteEffects &writes) {
	if (const auto *bl_value = dynamic_cast<const BooleanLiteral *>(&node)) return { .truth = bl_value->value };
	if (const auto *ie_test = dynamic_cast<const IfExpression *>(&node)) {
		SourceFlow sf_remaining, sf_result{ .fallsThrough = false };
		for (const ConditionalBranch &cb_branch : ie_test->branches) {
			const SourceFlow sf_condition = collectWrites(*cb_branch.condition, writes);
			const SourceFlow sf_body      = collectWrites(cb_branch.statements, writes);
			sf_remaining                  = sf_remaining.then(sf_condition);
			if (sf_condition.truth != false) sf_result = sf_result.either(sf_remaining.then(sf_body));
			if (sf_condition.truth == true) sf_remaining = { .fallsThrough = false };
		}
		if (ie_test->otherwise) sf_remaining = sf_remaining.then(collectWrites(*ie_test->otherwise, writes));
		return sf_result.either(sf_remaining);
	}
	if (const auto *be_test = dynamic_cast<const BinaryExpression *>(&node);
	    be_test && (be_test->op.getOperator() == OAnd || be_test->op.getOperator() == OOr)) {
		const SourceFlow sf_left   = collectWrites(*be_test->left, writes);
		const SourceFlow sf_right  = collectWrites(*be_test->right, writes);
		const bool       b_short   = be_test->op.getOperator() == OOr;
		SourceFlow       sf_result = sf_left;
		if (sf_left.truth != b_short) sf_result = sf_left.then(sf_left.truth ? sf_right : SourceFlow{}.either(sf_right));
		sf_result.truth = sf_left.truth == b_short || sf_right.truth == b_short ? std::optional{ b_short } :
		                  sf_left.truth && sf_right.truth                       ? std::optional{ !b_short } :
		                                                                          std::nullopt;
		return sf_result;
	}
	if (const auto *ue_test = dynamic_cast<const UnaryExpression *>(&node); ue_test && ue_test->op.getOperator() == ONot) {
		SourceFlow sf_operand = collectWrites(*ue_test->operand, writes);
		if (sf_operand.truth) sf_operand.truth = !*sf_operand.truth;
		return sf_operand;
	}
	if (const auto *ee_fallback = dynamic_cast<const ElvisExpression *>(&node)) {
		const SourceFlow sf_left  = collectWrites(*ee_fallback->left, writes);
		const SourceFlow sf_right = collectWrites(*ee_fallback->fallback, writes);
		SourceFlow sf_result      = sf_left.then(dynamic_cast<const NullLiteral *>(ee_fallback->left.get()) ? sf_right : SourceFlow{}.either(sf_right));
		sf_result.truth.reset();
		return sf_result;
	}
	if (const auto *ga_access = dynamic_cast<const GuardedAccessExpression *>(&node)) {
		const SourceFlow sf_receiver = collectWrites(*ga_access->getReceiver(), writes);
		return sf_receiver.then(SourceFlow{}.either(collectWrites(*ga_access->continuation, writes)));
	}
	SourceFlow sf_operands;
	if (const auto *ne_name = dynamic_cast<const NameExpression *>(&node)) {
		if (const Binding *b_local = local(ne_name->name.getNameKey()); b_local && b_local->presenceWrites) {
			const WriteEffects &we_block = *b_local->presenceWrites;
			writes.places.insert(we_block.places.begin(), we_block.places.end());
			writes.receivers.insert(we_block.receivers.begin(), we_block.receivers.end());
		}
	} else if (const auto *mc_call = dynamic_cast<const MemberCallExpression *>(&node)) {
		if (const std::optional<TypedPlace> tp_receiver = locate(*mc_call->receiver)) {
			const ResolvedType rt_type = tp_receiver->type.nullable() ? tp_receiver->type.element() : tp_receiver->type;
			if (rt_type.array()) {
				const std::optional<ArrayMutation> am_operation = arrayMutationNamed(mc_call->name.getNameKey());
				if (!mc_call->typeArguments && am_operation && *am_operation != ArrayMutation::Reserve)
					writes.receivers.insert(tp_receiver->place);
			} else if (asStruct(rt_type.declaration) && !analyzer.members(rt_type, mc_call->name.getNameKey(), SymbolKind::Method).empty())
				writes.receivers.insert(tp_receiver->place);
		}
	}
	visitOperands(
	        node, [&] (const Expression &operand) { sf_operands = sf_operands.then(collectWrites(operand, writes)); },
	        [&] (const std::vector<StatementPtr> &statements) { sf_operands = sf_operands.then(collectWrites(statements, writes)); });
	if (const auto *ce_call = dynamic_cast<const CallExpression *>(&node)) {
		const std::string &s_name = ce_call->name.getNameKey();
		if (!local(s_name)) {
			if (builtinNeverReturns(s_name) && callCandidates(*ce_call).empty()) sf_operands.fallsThrough = false;
			if (hasInstance() && asStruct(owner) && !builtinFunctionNamed(s_name) &&
			    !analyzer.members(selfType(), s_name, SymbolKind::Method, MemberLookup::CallContract).empty())
				writes.receivers.insert({ .root = &context.name });
		}
	} else if (const auto *tc_test = dynamic_cast<const TypeCheckExpression *>(&node)) {
		// A dotted value starts at its lexical binding. For a bare right operand a type of that name takes precedence.
		if (tc_test->valueCandidate)
			sf_operands = sf_operands.then(collectWrites(*tc_test->valueCandidate, writes));
		else if (const Token *t_name = tc_test->target.getBareIdentifier();
		         t_name && local(t_name->getNameKey()) && !analyzer.hasTypeName(context, *t_name))
			sf_operands = sf_operands.then(collectWrites(NameExpression(*t_name), writes));
	}
	sf_operands.truth.reset();
	return sf_operands;
}

BodyChecker::SourceFlow BodyChecker::collectWrites (const std::vector<StatementPtr> &statements, WriteEffects &writes) {
	const std::function fn_write = [&] (const Expression &target) {
		if (const std::optional<TypedPlace> tp_target = locate(target)) writes.places.insert(tp_target->place);
	};
	SourceFlow sf_result;
	scopes.emplace_back();
	for (const StatementPtr &sp_statement : statements) {
		if (const auto *bd_local = dynamic_cast<const BindingDeclaration *>(sp_statement.get())) {
			if (const auto *fb_block = dynamic_cast<const FunctionBlockExpression *>(bd_local->initializer.get())) {
				const std::shared_ptr<const WriteEffects> we_block = deferredPresenceWrites(*fb_block);
				shadow(scopes.back(), bd_local->name, we_block);
			} else {
				sf_result = sf_result.then(collectWrites(*bd_local->initializer, writes));
				shadow(scopes.back(), bd_local->name);
			}
		} else if (const auto *bg_group = dynamic_cast<const BindingGroupDeclaration *>(sp_statement.get())) {
			const auto                               *fb_block = dynamic_cast<const FunctionBlockExpression *>(bg_group->initializer.get());
			const std::shared_ptr<const WriteEffects> we_block = fb_block ? deferredPresenceWrites(*fb_block) : nullptr;
			if (!fb_block) sf_result = sf_result.then(collectWrites(*bg_group->initializer, writes));
			for (const ReceivingTarget &rt_slot : bg_group->targets)
				if (const ReceivingTarget::Named *nt_name = std::get_if<ReceivingTarget::Named>(&rt_slot.value))
					shadow(scopes.back(), nt_name->token, we_block);
		} else if (const auto *as_write = dynamic_cast<const AssignmentStatement *>(sp_statement.get())) {
			fn_write(NameExpression(as_write->name));
			sf_result = sf_result.then(collectWrites(*as_write->value, writes));
		} else if (const auto *ma_group = dynamic_cast<const MultiAssignmentStatement *>(sp_statement.get())) {
			for (const ReceivingTarget &rt_slot : ma_group->targets)
				if (const ReceivingTarget::Named *nt_name = std::get_if<ReceivingTarget::Named>(&rt_slot.value))
					fn_write(NameExpression(nt_name->token));
			sf_result = sf_result.then(collectWrites(*ma_group->value, writes));
		} else if (const auto *es_value = dynamic_cast<const ExpressionStatement *>(sp_statement.get()))
			sf_result = sf_result.then(collectWrites(*es_value->expression, writes));
		else if (const auto *rs_return = dynamic_cast<const ReturnStatement *>(sp_statement.get())) {
			if (rs_return->value) sf_result = sf_result.then(collectWrites(*rs_return->value, writes));
			sf_result.fallsThrough = false;
		} else if (const auto *ls_loop = dynamic_cast<const LoopStatement *>(sp_statement.get())) {
			if (const auto *fs_for = dynamic_cast<const ForStatement *>(ls_loop))
				sf_result = sf_result.then(collectWrites(fs_for->source.getExpression(), writes));
			sf_result = sf_result.then(collectRepeatedWrites(*ls_loop, writes));
		} else if (const auto *lj_jump = dynamic_cast<const LoopJumpStatement *>(sp_statement.get())) {
			sf_result = sf_result.then({ .fallsThrough = false, .breaks = !lj_jump->isContinue(), .continues = lj_jump->isContinue() });
		} else {
			const std::pair<ExpressionPtr, ExpressionPtr> ep_operands = updateOperands(*sp_statement);
			fn_write(*ep_operands.first);
			sf_result = sf_result.then(collectWrites(*ep_operands.first, writes));
			if (ep_operands.second) sf_result = sf_result.then(collectWrites(*ep_operands.second, writes));
		}
	}
	scopes.pop_back();
	return sf_result;
}

BodyChecker::SourceFlow BodyChecker::collectRepeatedWrites (const LoopStatement &node, WriteEffects &writes, bool *canRepeat) {
	if (const auto *ws_while = dynamic_cast<const WhileStatement *>(&node)) {
		const SourceFlow sf_test = collectWrites(*ws_while->condition, writes);
		const SourceFlow sf_body = collectWrites(ws_while->body.statements, writes);
		return sf_test.then({ .fallsThrough = sf_test.truth != true || sf_body.breaks });
	} else if (const auto *fs_for = dynamic_cast<const ForStatement *>(&node)) {
		scopes.emplace_back();
		shadow(scopes.back(), fs_for->binding.name);
		const SourceFlow sf_body = collectWrites(fs_for->body.statements, writes);
		SourceFlow       sf_step;
		if (fs_for->stepper && fs_for->stepper->value) sf_step = collectWrites(*fs_for->stepper->value, writes);
		scopes.pop_back();
		return sf_body.fallsThrough || sf_body.continues ? SourceFlow{}.either(sf_step) : SourceFlow{};
	} else {
		const auto      &ds_do   = static_cast<const DoWhileStatement &>(node);
		const SourceFlow sf_body = collectWrites(ds_do.body.statements, writes);
		const SourceFlow sf_test = collectWrites(*ds_do.condition, writes);
		const bool       b_tests = sf_body.fallsThrough || sf_body.continues;
		if (canRepeat) *canRepeat = b_tests && sf_test.fallsThrough && sf_test.truth != false;
		return { .fallsThrough = sf_body.breaks || (b_tests && sf_test.fallsThrough && sf_test.truth != true),
			 .breaks       = b_tests && sf_test.breaks,
			 .continues    = b_tests && sf_test.continues };
	}
}

std::shared_ptr<const WriteEffects> BodyChecker::deferredPresenceWrites (const FunctionBlockExpression &node) {
	std::shared_ptr<WriteEffects> we_writes = std::make_shared<WriteEffects>();
	collectWrites(std::get<BlockBody>(node.declaration.body.value).statements, *we_writes);
	return we_writes;
}

BodyChecker::Narrowing BodyChecker::loopPresence (const LoopStatement &node, bool &canRepeat) {
	Narrowing n_entry = fs_flow.narrowed;
	if (std::ranges::none_of(n_entry, [] (const Narrowing::value_type &entry) { return entry.first.fact == PlaceFact::Presence; })) return n_entry;
	WriteEffects we_writes;
	collectRepeatedWrites(node, we_writes, &canRepeat);
	std::erase_if(n_entry, [&] (const Narrowing::value_type &entry) {
		const StoragePlace &sp_proof = entry.first;
		return sp_proof.fact == PlaceFact::Presence &&
		       (sharedPlace(sp_proof) || std::ranges::any_of(we_writes.places, [&] (const StoragePlace &place) { return place.covers(sp_proof); }) ||
		        std::ranges::any_of(we_writes.receivers, [&] (const StoragePlace &place) { return place.covers(sp_proof, true); }));
	});
	return n_entry;
}
}
