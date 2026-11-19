// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <ast/decl/enum_declaration.hpp>
#include <ast/expr/provider_expressions.hpp>
#include <ast/stmt/do_while_statement.hpp>
#include <ast/stmt/for_statement.hpp>
#include <ast/stmt/while_statement.hpp>
#include <semantic/checker.hpp>

namespace cloth::semantic {
namespace {

using Identity = std::size_t;

// Identity zero denotes a value without a tracked object.
enum class Presence { Unknown, Present, Absent };

struct Handle {
	Identity identity                         = 0;
	Presence presence                         = Presence::Unknown;
	bool     operator==(const Handle &) const = default;
};

using Values = std::vector<Handle>;
enum class Lifetime { Unknown, Live, Dead, Destroying };

struct Activation {
	const Symbol *selection;
	ResolvedType  host;
	bool          directOnly                           = false;
	bool          operator==(const Activation &) const = default;
};

struct ObjectFacts {
	Lifetime                  lifetime = Lifetime::Unknown;
	std::set<const Token *>   deletions;
	std::optional<Activation> active;
	bool                      operator==(const ObjectFacts &) const = default;
};

struct Path {
	std::map<const Token *, Handle> bindings;
	std::map<Identity, ObjectFacts> objects;
	Values                          values;
	bool                            reachable = true;
};

using OptionalPath = std::optional<Path>;

struct Branches {
	OptionalPath yes, no;
};

struct LoopPaths {
	OptionalPath breaks, continues;
};

struct StorageOwner {
	const Expression *receiver = nullptr;
	bool              implicit = false;
};

bool directSelf (const Expression &node) {
	const auto *ne_name = dynamic_cast<const NameExpression *>(&node);
	return ne_name && ne_name->name.get_lexeme() == to_string(kSelf);
}

ObjectFacts object (const Identity identity, const Path &path) {
	const auto it_entry = path.objects.find(identity);
	return it_entry == path.objects.end() ? ObjectFacts{} : it_entry->second;
}

ObjectFacts common (const ObjectFacts &left, const ObjectFacts &right) {
	ObjectFacts of_result;
	if (left.lifetime == right.lifetime) of_result.lifetime = left.lifetime;
	if (of_result.lifetime == Lifetime::Dead) {
		of_result.deletions = left.deletions;
		of_result.deletions.insert(right.deletions.begin(), right.deletions.end());
	}
	if (left.active && right.active && left.active->selection == right.active->selection && left.active->host == right.active->host) {
		of_result.active = left.active;
		of_result.active->directOnly |= right.active->directOnly;
	}
	return of_result;
}

void unknownEffect (Path &path) {
	for (auto &val : path.objects | std::views::values) {
		val.active.reset();
		if (val.lifetime == Lifetime::Live) val.lifetime = Lifetime::Unknown;
	}
}

class LifetimeChecker {
    public:
	LifetimeChecker (Analyzer &analyzer, const Analyzer::LifetimeBody &body) : a_analyzer(analyzer), lb_job(body), bf_facts(*body.facts) {
	}

	void check ();

    private:
	Analyzer                                                &a_analyzer;
	const Analyzer::LifetimeBody                            &lb_job;
	BodyFacts                                               &bf_facts;
	std::map<const LoopTarget *, LoopPaths>                  m_jumps;
	std::map<const Token *, const FunctionBlockExpression *> m_deferred;
	std::map<const GuardedReceiver *, Handle>                m_guarded;
	// Only enclosing writes observe projected class receivers. Capture them before evaluating the RHS.
	std::map<const Expression *, Handle> m_observed;
	Identity                             id_next   = 1;
	bool                                 b_emit    = true;
	OptionalPath                        *p_returns = nullptr;

	Handle                            fresh (const ResolvedType &type);
	Values                            typedValues (const ExpressionInfo &info);
	void                              join (OptionalPath &paths, const Path &other);
	static bool                       equivalent (const Path &left, const Path &right, const Path &entry);
	void                              access (const Handle &handle, const Path &path, const TokenSpan &span, bool permit, bool &runtime) const;
	[[nodiscard]] const TypeDispatch *table (const ResolvedType &type) const;
	void                              enter (Path &path, Handle handle, const Symbol &selection, const ResolvedType &host);
	void                       selectState (const ExpressionInfo &call, Handle receiver, bool self, const Token &name, Path &path, ReceiverChecks &checks);
	void                       defaults (const ExpressionInfo &call, Path &path) const;
	Handle                     scalar (const Expression &node, Path &path);
	Values                     expression (const Expression &node, Path &path);
	Values                     conditional (const IfExpression &node, Path &path);
	Values                     guardedAccess (const GuardedAccessExpression &node, Path &path);
	void                       binary (const BinaryExpression &node, const ExpressionInfo &info, Path &path, ReceiverChecks &checks);
	Branches                   condition (const Expression &node, Path &path);
	void                       statements (const std::vector<StatementPtr> &body, Path &path);
	void                       statement (const Statement &node, Path &path);
	void                       loop (const LoopStatement &node, Path &path);
	void                       arguments (const CallSite &call, Path &path);
	void                       store (const Token &name, Handle handle, Path &path);
	Values                     runDeferred (const FunctionBlockExpression &block, Path &path);
	[[nodiscard]] const Token *binding (const Token &name) const;
	[[nodiscard]] StorageOwner storageOwner (const Expression &target) const;
	void   write (const Statement &node, const Expression &target, const ExpressionPtr &value, const ExpressionPtr &compound, Path &path);
	Values call (const CallSite &node, const ExpressionInfo &info, Path &path, ReceiverChecks &checks);
};

Handle LifetimeChecker::fresh (const ResolvedType &type) {
	const bool b_optional  = type.nullable();
	const bool b_reference = a_analyzer.referenceType(b_optional ? type.element() : type);
	const bool b_present   = !b_optional && (b_reference || !type.parameter) && type.name != to_string(kNull);
	return { .identity = b_reference ? id_next++ : 0, .presence = b_present ? Presence::Present : Presence::Unknown };
}

Values LifetimeChecker::typedValues (const ExpressionInfo &info) {
	Values v_result;
	for (const ResolvedType &rt_type : info.types) v_result.push_back(fresh(rt_type));
	return v_result;
}

void LifetimeChecker::join (OptionalPath &paths, const Path &other) {
	if (!other.reachable) return;
	if (!paths) {
		paths = other;
		return;
	}
	const Path &left = *paths;
	Path        p_joined;
	for (const std::map<Identity, ObjectFacts>::value_type &entry : left.objects) {
		if (other.objects.contains(entry.first)) p_joined.objects.emplace(entry.first, common(entry.second, object(entry.first, other)));
	}
	// Bindings that select the same pair on both edges still alias one another after the join.
	std::map<std::pair<Identity, Identity>, Identity> m_pairs;
	const std::function                               merge = [&] (const Handle a, Handle b) {
		Identity id_object = a.identity;
		if (id_object != b.identity) {
			const std::pair<std::map<std::pair<Identity, Identity>, Identity>::iterator, bool> it_entry =
			        m_pairs.try_emplace({ id_object, b.identity }, id_next);
			if (it_entry.second) ++id_next;
			id_object                   = it_entry.first->second;
			p_joined.objects[id_object] = common(object(a.identity, left), object(b.identity, other));
		}
		return Handle{ id_object, a.presence == b.presence ? a.presence : Presence::Unknown };
	};
	for (const std::map<const Token *, Handle>::value_type &entry : left.bindings) {
		const auto it_entry = other.bindings.find(entry.first);
		if (it_entry != other.bindings.end()) p_joined.bindings.emplace(entry.first, merge(entry.second, it_entry->second));
	}
	if (left.values.size() == other.values.size()) {
		for (std::size_t st_index = 0; st_index < left.values.size(); ++st_index)
			p_joined.values.push_back(merge(left.values[st_index], other.values[st_index]));
	}
	paths = std::move(p_joined);
}

bool LifetimeChecker::equivalent (const Path &left, const Path &right, const Path &entry) {
	// Compare alias partitions independently of the fresh numbers allocated for joins.
	std::map<Identity, Identity> forward, reverse;
	for (const auto &[identity, presence] : entry.bindings | std::views::values) {
		forward[identity] = reverse[identity] = identity;
	}
	for (const auto &key : entry.objects | std::views::keys) {
		if (object(key, left) != object(key, right)) return false;
		forward[key] = reverse[key] = key;
	}
	if (left.bindings.size() != right.bindings.size()) return false;
	for (const auto &[token, handle] : left.bindings) {
		const auto it_entry = right.bindings.find(token);
		if (it_entry == right.bindings.end()) return false;
		const Handle a = handle, b = it_entry->second;
		if (a.presence != b.presence || object(a.identity, left) != object(b.identity, right)) return false;
		if (forward.try_emplace(a.identity, b.identity).first->second != b.identity ||
		    reverse.try_emplace(b.identity, a.identity).first->second != a.identity)
			return false;
	}
	return true;
}

void LifetimeChecker::access (const Handle &handle, const Path &path, const TokenSpan &span, bool permit, bool &runtime) const {
	if (!handle.identity || !path.reachable) return;
	const ObjectFacts of_object = object(handle.identity, path);
	if (b_emit && (of_object.lifetime == Lifetime::Dead || (of_object.lifetime == Lifetime::Destroying && !permit))) {
		const std::string             s_message = of_object.lifetime == Lifetime::Dead ? "Access to a deleted class instance" :
		                                                                                 "This access is invalid while the instance is being destroyed";
		std::vector<cde::SourceLabel> v_labels{ { .span = span, .text = s_message } };
		for (const Token *t_delete : of_object.deletions) v_labels.push_back({ t_delete->get_span(), "instance deleted here" });
		if (of_object.lifetime == Lifetime::Destroying) v_labels.push_back({ lb_job.context->name.get_span(), "destructor declared here" });
		throw cde::ClothDiagnostic(Code::DELETED_CLASS_ACCESS, s_message, std::move(v_labels));
	}
	runtime |= of_object.lifetime == Lifetime::Unknown;
}

const TypeDispatch *LifetimeChecker::table (const ResolvedType &type) const {
	const std::map<ResolvedType, TypeDispatch>::const_iterator it_entry = a_analyzer.program.dispatchTables.find(type);
	return it_entry == a_analyzer.program.dispatchTables.end() ? nullptr : &it_entry->second;
}

void LifetimeChecker::enter (Path &path, Handle handle, const Symbol &selection, const ResolvedType &host) {
	const TypeDispatch *td_host = table(host);
	if (!td_host) {
		unknownEffect(path);
		return;
	}
	const auto it_entry = td_host->states.find(selection.name.get_lexeme());
	if (it_entry == td_host->states.end() || it_entry->second.selection != &selection) {
		unknownEffect(path);
		return;
	}
	for (const SymbolScope::value_type &entry : it_entry->second.definition->members) {
		if (std::ranges::any_of(entry.second, [] (const Symbol &member) { return member.kind == SymbolKind::StateInitializer; })) {
			unknownEffect(path);
			return;
		}
	}
	// Other bindings may refer to this instance. Entering a state without a hook preserves liveness.
	for (std::map<Identity, ObjectFacts>::value_type &entry : path.objects) entry.second.active.reset();
	if (handle.identity) path.objects[handle.identity].active = Activation{ &selection, host };
}

void LifetimeChecker::selectState (const ExpressionInfo &call, Handle receiver, bool self, const Token &name, Path &path, ReceiverChecks &checks) {
	if (!path.reachable || !call.dispatch) return;
	const std::optional<Activation> active = object(receiver.identity, path).active;
	if (!active || (active->directOnly && !self)) {
		checks.activeState = true;
		return;
	}
	const TypeDispatch *td_host     = table(active->host);
	const StateMethod  *sm_contract = nullptr;
	for (const TypeDispatch *td_view : { td_host, table(call.dispatch->receiver) }) {
		if (!td_view) continue;
		const std::vector<StateMethod>::const_iterator it_entry =
		        std::ranges::find(td_view->stateMethods, call.dispatch->contract, &StateMethod::contract);
		if (it_entry != td_view->stateMethods.end()) {
			sm_contract = &*it_entry;
			break;
		}
	}
	if (b_emit && (!sm_contract || !sm_contract->implementations.contains(active->selection)))
		fail(Code::INVALID_STATE_DISPATCH, name.get_span(),
		     "State '" + active->selection->name.get_lexeme() + "' does not implement '" + name.get_lexeme() + "'", &active->selection->name);
	checks.knownState = active->selection;
}

void LifetimeChecker::defaults (const ExpressionInfo &call, Path &path) const {
	if (!path.reachable || !call.argumentContract) return;
	const BodyFacts &bf_contract = call.contractInstance ? call.contractInstance->body : static_cast<const BodyFacts &>(a_analyzer.program);
	for (const std::size_t st_index : call.omittedParameters) {
		const ParameterDeclaration &pd_default = callable(*call.argumentContract)->parameters[st_index];
		const std::unordered_map<const ParameterDeclaration *, DefaultEffects>::const_iterator it_entry = bf_contract.defaultEffects.find(&pd_default);
		if (it_entry == bf_contract.defaultEffects.end() || it_entry->second.mayCallOrMutate) unknownEffect(path);
		if (it_entry != bf_contract.defaultEffects.end() && !it_entry->second.fallsThrough) {
			path.reachable = false;
			return;
		}
	}
}

const Token *LifetimeChecker::binding (const Token &name) const {
	const std::unordered_map<const Token *, const Token *>::const_iterator it_entry = bf_facts.references.find(&name);
	return it_entry == bf_facts.references.end() ? &name : it_entry->second;
}

void LifetimeChecker::store (const Token &name, Handle handle, Path &path) {
	const std::unordered_map<const Token *, ResolvedType>::const_iterator it_entry = bf_facts.localTypes.find(&name);
	if (it_entry == bf_facts.localTypes.end()) return;
	if (!handle.identity && handle.presence == Presence::Unknown) handle = fresh(it_entry->second);
	if (handle.identity || it_entry->second.nullable()) path.bindings[&name] = handle;
}

Handle LifetimeChecker::scalar (const Expression &node, Path &path) {
	const Values                                         v_result = expression(node, path);
	const Handle                                         h_value  = v_result.size() == 1 ? v_result.front() : Handle{};
	const std::map<const Expression *, Handle>::iterator it_owner = m_observed.find(&node);
	if (it_owner != m_observed.end()) it_owner->second = h_value;
	return h_value;
}

void LifetimeChecker::arguments (const CallSite &call, Path &path) {
	for (const CallArgument &ca_arg : call.suppliedArguments) {
		if (!path.reachable) break;
		expression(*ca_arg.expression, path);
	}
}

Values LifetimeChecker::call (const CallSite &node, const ExpressionInfo &info, Path &path, ReceiverChecks &checks) {
	const MemberCallExpression    *mc_method   = dynamic_cast<const MemberCallExpression *>(&node);
	const StateCallExpression     *sc_state    = dynamic_cast<const StateCallExpression *>(&node);
	const ClassCreationExpression *cc_new      = dynamic_cast<const ClassCreationExpression *>(&node);
	const Expression              *e_receiver  = mc_method ? mc_method->receiver.get() : sc_state ? sc_state->receiver.get() : nullptr;
	const bool                     b_method    = info.symbol && info.symbol->kind == SymbolKind::Method;
	const StorageOwner             arrayOwner  = info.arrayMutation && e_receiver ? storageOwner(*e_receiver) : StorageOwner{};
	if (arrayOwner.receiver) m_observed.try_emplace(arrayOwner.receiver);
	const Handle h_receiver  = e_receiver ? scalar(*e_receiver, path) : b_method ? path.bindings[&lb_job.context->name] : Handle{};
	const Handle storedOwner = arrayOwner.receiver ? m_observed.at(arrayOwner.receiver) :
	                           arrayOwner.implicit ? path.bindings[&lb_job.context->name] :
	                                                 Handle{};
	if (arrayOwner.receiver) m_observed.erase(arrayOwner.receiver);
	const bool                     b_self      = !e_receiver || directSelf(*e_receiver);
	const TokenSpan                ts_receiver = e_receiver ? e_receiver->get_span() : node.name.get_span();
	if (path.reachable && !sc_state && b_method) access(h_receiver, path, ts_receiver, b_self, checks.beforeArguments);
	arguments(node, path);
	defaults(info, path);
	if (!path.reachable) return {};
	if (sc_state || b_method) access(h_receiver, path, ts_receiver, b_self, checks.atAccess);
	if (info.arrayMutation)
		access(storedOwner, path, ts_receiver, arrayOwner.implicit || (arrayOwner.receiver && directSelf(*arrayOwner.receiver)), checks.atAccess);
	if (sc_state) selectState(info, h_receiver, b_self, node.name, path, checks);
	if (info.binding && m_deferred.contains(info.binding))
		return runDeferred(*m_deferred.at(info.binding), path);
	else if (info.argumentContract)
		unknownEffect(path);
	Values v_result = typedValues(info);
	if (cc_new && !v_result.empty() && v_result.front().identity) {
		const ResolvedType &rt_host = info.types.front();
		if (info.symbol && a_analyzer.preservesNewReceiver(rt_host, *info.symbol)) {
			const TypeDispatch *td_host = table(rt_host);
			if (td_host && td_host->initialState) enter(path, v_result.front(), *td_host->initialState, rt_host);
			ObjectFacts &of_new = path.objects[v_result.front().identity];
			if (!td_host || !td_host->initialState || of_new.active) of_new.lifetime = Lifetime::Live;
		}
	}
	return v_result;
}

Branches LifetimeChecker::condition (const Expression &node, Path &path) {
	if (!path.reachable) return {};
	const std::unordered_map<const Expression *, ExpressionInfo>::iterator it_info = bf_facts.expressions.find(&node);
	if (const UnaryExpression *ue_not = dynamic_cast<const UnaryExpression *>(&node);
	    ue_not && ue_not->op.get_operator() == oNot && it_info != bf_facts.expressions.end() && !it_info->second.argumentContract) {
		Branches br_paths = condition(*ue_not->operand, path);
		std::swap(br_paths.yes, br_paths.no);
		return br_paths;
	}
	if (const BinaryExpression *be_test = dynamic_cast<const BinaryExpression *>(&node)) {
		const Operator o_operation = be_test->op.get_operator();
		if ((o_operation == oEqual || o_operation == oNotEqual || o_operation == oIdentity) && it_info != bf_facts.expressions.end() &&
		    !it_info->second.argumentContract) {
			const Expression *e_operand = dynamic_cast<const NullLiteral *>(be_test->left.get())  ? be_test->right.get() :
			                              dynamic_cast<const NullLiteral *>(be_test->right.get()) ? be_test->left.get() :
			                                                                                        nullptr;
			if (e_operand) {
				const Handle h_value = scalar(*e_operand, path);
				if (!path.reachable) return {};
				Branches      br_paths{ path, path };
				OptionalPath &opt_present = o_operation == oNotEqual ? br_paths.yes : br_paths.no;
				OptionalPath &opt_absent  = o_operation == oNotEqual ? br_paths.no : br_paths.yes;
				if (h_value.presence == Presence::Absent) opt_present.reset();
				if (h_value.presence == Presence::Present) opt_absent.reset();
				const ExpressionInfo &ei_operand = bf_facts.expressions.at(e_operand);
				for (OptionalPath *opt_path : { &opt_present, &opt_absent }) {
					if (!*opt_path) continue;
					for (std::map<const Token *, Handle>::value_type &entry : (*opt_path)->bindings) {
						if (entry.first != ei_operand.binding && (!h_value.identity || entry.second.identity != h_value.identity))
							continue;
						entry.second.presence = opt_path == &opt_present ? Presence::Present : Presence::Absent;
					}
				}
				return br_paths;
			}
		}
		if (o_operation == oAnd || o_operation == oOr) {
			Branches      br_left = condition(*be_test->left, path);
			OptionalPath &p_next  = o_operation == oAnd ? br_left.yes : br_left.no;
			if (!p_next) return br_left;
			Branches            br_right = condition(*be_test->right, *p_next);
			const OptionalPath &p_short  = o_operation == oAnd ? br_right.no : br_right.yes;
			if (p_short) join(o_operation == oAnd ? br_left.no : br_left.yes, *p_short);
			p_next = o_operation == oAnd ? br_right.yes : br_right.no;
			return br_left;
		}
	}
	if (const TypeCheckExpression *tc_test = dynamic_cast<const TypeCheckExpression *>(&node);
	    tc_test && it_info != bf_facts.expressions.end() && it_info->second.symbol &&
	    (it_info->second.symbol->kind == SymbolKind::State || it_info->second.symbol->kind == SymbolKind::StateAttachment)) {
		const Handle   h_receiver = scalar(*tc_test->operand, path);
		ReceiverChecks rc_check;
		access(h_receiver, path, tc_test->operand->get_span(), directSelf(*tc_test->operand), rc_check.atAccess);
		if (b_emit) it_info->second.receiverChecks.atAccess |= rc_check.atAccess;
		if (!path.reachable) return {};
		Branches                        br_paths{ path, path };
		const Symbol                   *sym_target = it_info->second.symbol;
		const std::optional<Activation> active     = object(h_receiver.identity, path).active;
		if (active && active->selection == sym_target)
			br_paths.no.reset();
		else if (active)
			br_paths.yes.reset();
		if (h_receiver.identity && br_paths.yes) {
			const ResolvedType &rt_host = lb_job.state && directSelf(*tc_test->operand) && lb_job.host ?
			                                      *lb_job.host :
			                                      bf_facts.expressions.at(tc_test->operand.get()).types.front();
			br_paths.yes->objects[h_receiver.identity].active =
			        Activation{ sym_target, rt_host, directSelf(*tc_test->operand) && (!active || active->directOnly) };
		}
		return br_paths;
	}
	expression(node, path);
	if (!path.reachable) return {};
	if (const BooleanLiteral *bl_literal = dynamic_cast<const BooleanLiteral *>(&node))
		return bl_literal->value ? Branches{ path, {} } : Branches{ {}, path };
	return { path, path };
}

Values LifetimeChecker::conditional (const IfExpression &node, Path &path) {
	Values       v_result;
	OptionalPath remaining = path, after;
	for (const ConditionalBranch &cb_branch : node.branches) {
		if (!remaining) break;
		Branches br_paths = condition(*cb_branch.condition, *remaining);
		if (br_paths.yes) {
			statements(cb_branch.statements, *br_paths.yes);
			join(after, *br_paths.yes);
		}
		remaining = std::move(br_paths.no);
	}
	if (remaining) {
		if (node.otherwise)
			statements(*node.otherwise, *remaining);
		else
			remaining->values.clear();
		join(after, *remaining);
	}
	if (after) {
		path     = std::move(*after);
		v_result = std::move(path.values);
	} else
		path.reachable = false;
	return v_result;
}

Values LifetimeChecker::guardedAccess (const GuardedAccessExpression &node, Path &path) {
	Values v_result;
	Handle h_value = scalar(*node.get_receiver(), path);
	if (path.reachable && h_value.presence != Presence::Absent) {
		OptionalPath after;
		if (h_value.presence != Presence::Present) {
			path.values = { { 0, Presence::Absent } };
			after       = path;
		}
		h_value.presence              = Presence::Present;
		m_guarded[node.payload.get()] = h_value;
		path.values                   = expression(*node.continuation, path);
		join(after, path);
		m_guarded.erase(node.payload.get());
		if (after) {
			path     = std::move(*after);
			v_result = std::move(path.values);
		}
	} else
		v_result = { h_value };
	return v_result;
}

void LifetimeChecker::binary (const BinaryExpression &node, const ExpressionInfo &info, Path &path, ReceiverChecks &checks) {
	const Operator o_operation = node.op.get_operator();
	if (o_operation == oAnd || o_operation == oOr) {
		Branches br_paths = condition(node, path);
		if (br_paths.no) join(br_paths.yes, *br_paths.no);
		if (br_paths.yes)
			path = std::move(*br_paths.yes);
		else
			path.reachable = false;
	} else {
		const Handle h_left  = scalar(*node.left, path);
		const bool   b_right = info.argumentContract &&
		                     dynamic_cast<const MethodDeclaration &>(*callable(*info.argumentContract)).operatorMember->rightReceiver;
		if (info.argumentContract && !b_right) access(h_left, path, node.left->get_span(), directSelf(*node.left), checks.beforeArguments);
		const Handle h_right = scalar(*node.right, path);
		if (info.argumentContract) {
			const Expression &e_operand = b_right ? *node.right : *node.left;
			access(b_right ? h_right : h_left, path, e_operand.get_span(), directSelf(e_operand), checks.atAccess);
			unknownEffect(path);
		} else if (isEquality(o_operation) && (o_operation != oIdentity || info.checkedIdentity) && h_left.presence == Presence::Present &&
		           h_right.presence == Presence::Present) {
			access(h_left, path, node.left->get_span(), directSelf(*node.left), checks.atAccess);
			access(h_right, path, node.right->get_span(), directSelf(*node.right), checks.atAccess);
		}
	}
}

Values LifetimeChecker::expression (const Expression &node, Path &path) {
	if (!path.reachable) return {};
	const std::unordered_map<const Expression *, ExpressionInfo>::iterator it_entry = bf_facts.expressions.find(&node);
	if (it_entry == bf_facts.expressions.end()) return {};
	ExpressionInfo &ei_info = it_entry->second;
	ReceiverChecks  rc_checks;
	Values          v_result;
	if (const NameExpression *ne_name = dynamic_cast<const NameExpression *>(&node)) {
		if (ei_info.binding && m_deferred.contains(ei_info.binding)) {
			v_result = runDeferred(*m_deferred.at(ei_info.binding), path);
		} else if (ei_info.binding || directSelf(node)) {
			const Token                                          *t_key      = ei_info.binding ? ei_info.binding : &lb_job.context->name;
			const std::map<const Token *, Handle>::const_iterator it_binding = path.bindings.find(t_key);
			if (it_binding != path.bindings.end()) {
				Handle h_value = it_binding->second;
				if (!ei_info.types.empty() && !ei_info.types.front().nullable() && !ei_info.types.front().parameter)
					h_value.presence = Presence::Present;
				v_result.push_back(h_value);
			}
		} else if (ei_info.symbol && ei_info.symbol->kind == SymbolKind::Field) {
			access(path.bindings[&lb_job.context->name], path, ne_name->get_span(), true, rc_checks.atAccess);
		}
	} else if (dynamic_cast<const NullLiteral *>(&node)) {
		v_result.push_back({ 0, Presence::Absent });
	} else if (const IfExpression *ie_if = dynamic_cast<const IfExpression *>(&node)) {
		v_result = conditional(*ie_if, path);
	} else if (const FieldAccessExpression *fa_field = dynamic_cast<const FieldAccessExpression *>(&node)) {
		const Handle h_value = scalar(*fa_field->receiver, path);
		if (ei_info.symbol && ei_info.symbol->kind == SymbolKind::Field)
			access(h_value, path, node.get_span(), directSelf(*fa_field->receiver), rc_checks.atAccess);
	} else if (const ClassDeletionExpression *cd_delete = dynamic_cast<const ClassDeletionExpression *>(&node)) {
		const Handle h_value = scalar(*cd_delete->receiver, path);
		access(h_value, path, node.get_span(), false, rc_checks.atAccess);
		if (path.reachable && h_value.presence != Presence::Absent) {
			unknownEffect(path);
			if (h_value.identity && h_value.presence == Presence::Present) {
				ObjectFacts &of_deleted = path.objects[h_value.identity];
				of_deleted.lifetime     = Lifetime::Dead;
				of_deleted.deletions.insert(&cd_delete->keyword);
			}
		}
	} else if (const CallSite *cs_call = dynamic_cast<const CallSite *>(&node)) {
		v_result = call(*cs_call, ei_info, path, rc_checks);
	} else if (const GuardedReceiver *gr_value = dynamic_cast<const GuardedReceiver *>(&node)) {
		v_result = { m_guarded.at(gr_value) };
	} else if (const GuardedAccessExpression *ga_access = dynamic_cast<const GuardedAccessExpression *>(&node)) {
		v_result = guardedAccess(*ga_access, path);
	} else if (const ElvisExpression *ee_expr = dynamic_cast<const ElvisExpression *>(&node)) {
		const Handle h_value = scalar(*ee_expr->left, path);
		if (h_value.presence == Presence::Present)
			v_result = { h_value };
		else if (h_value.presence == Presence::Absent)
			v_result = expression(*ee_expr->fallback, path);
		else if (path.reachable) {
			Path p_present     = path;
			p_present.values   = { { h_value.identity, Presence::Present } };
			OptionalPath after = std::move(p_present);
			path.values        = expression(*ee_expr->fallback, path);
			join(after, path);
			path     = std::move(*after);
			v_result = std::move(path.values);
		}
	} else if (const TypeCheckExpression *tc_test = dynamic_cast<const TypeCheckExpression *>(&node)) {
		const decltype(bf_facts.valueTests)::const_iterator it_value = bf_facts.valueTests.find(tc_test);
		if (it_value != bf_facts.valueTests.end())
			expression(*it_value->second, path);
		else {
			const Handle h_value = scalar(*tc_test->operand, path);
			if (ei_info.symbol && (asClass(ei_info.symbol) || asInterface(ei_info.symbol) || ei_info.symbol->kind == SymbolKind::State ||
			                       ei_info.symbol->kind == SymbolKind::StateAttachment))
				access(h_value, path, tc_test->operand->get_span(), directSelf(*tc_test->operand), rc_checks.atAccess);
		}
	} else if (const CastExpression *ce_cast = dynamic_cast<const CastExpression *>(&node)) {
		const Handle h_value = scalar(*ce_cast->operand, path);
		if (ei_info.symbol && (ei_info.symbol->kind == SymbolKind::State || ei_info.symbol->kind == SymbolKind::StateAttachment)) {
			access(h_value, path, ce_cast->operand->get_span(), false, rc_checks.atAccess);
			if (path.reachable)
				enter(path, h_value, *ei_info.symbol,
				      lb_job.state && directSelf(*ce_cast->operand) ? *lb_job.host :
				                                                      bf_facts.expressions.at(ce_cast->operand.get()).types.front());
		} else if (!ei_info.types.empty())
			v_result.push_back(h_value);
	} else if (const StateTransitionExpression *st_enter = dynamic_cast<const StateTransitionExpression *>(&node)) {
		const Handle h_value = scalar(*st_enter->receiver, path);
		if (st_enter->payload) expression(*st_enter->payload, path);
		access(h_value, path, st_enter->receiver->get_span(), false, rc_checks.atAccess);
		if (path.reachable && ei_info.symbol)
			enter(path, h_value, *ei_info.symbol,
			      lb_job.state && directSelf(*st_enter->receiver) ? *lb_job.host : bf_facts.expressions.at(st_enter->receiver.get()).types.front());
	} else if (const BinaryExpression *be_op = dynamic_cast<const BinaryExpression *>(&node)) {
		binary(*be_op, ei_info, path, rc_checks);
	} else if (const UnaryExpression *ue_op = dynamic_cast<const UnaryExpression *>(&node)) {
		const Handle h_value = scalar(*ue_op->operand, path);
		if (ei_info.argumentContract) {
			access(h_value, path, node.get_span(), directSelf(*ue_op->operand), rc_checks.atAccess);
			unknownEffect(path);
		}
	} else if (const IndexExpression *ie_index = dynamic_cast<const IndexExpression *>(&node)) {
		const Handle h_value = scalar(*ie_index->array, path);
		if (ei_info.argumentContract) access(h_value, path, ie_index->array->get_span(), directSelf(*ie_index->array), rc_checks.beforeArguments);
		expression(*ie_index->index, path);
		if (ei_info.argumentContract) {
			access(h_value, path, ie_index->array->get_span(), directSelf(*ie_index->array), rc_checks.atAccess);
			if (ei_info.indexed && ei_info.indexed->validator) {
				unknownEffect(path);
				access(h_value, path, ie_index->array->get_span(), directSelf(*ie_index->array), rc_checks.atAccess);
			}
			unknownEffect(path);
		}
	} else if (const ProviderAccessExpression *pa_provider = dynamic_cast<const ProviderAccessExpression *>(&node)) {
		const Handle h_value = scalar(*pa_provider->receiver, path);
		if (ei_info.argumentContract || pa_provider->name.get_lexeme() == provider::S_STRING) {
			access(h_value, path, pa_provider->receiver->get_span(), directSelf(*pa_provider->receiver), rc_checks.atAccess);
			if (ei_info.argumentContract || (ei_info.provider && ei_info.provider->callsFormatter)) unknownEffect(path);
		}
	} else if (const ResultListExpression *rl_results = dynamic_cast<const ResultListExpression *>(&node)) {
		for (const ExpressionPtr &ep_value : rl_results->values) v_result.push_back(scalar(*ep_value, path));
	} else if (const ArrayLiteral *al_array = dynamic_cast<const ArrayLiteral *>(&node)) {
		for (const ExpressionPtr &ep_value : al_array->elements) expression(*ep_value, path);
	} else if (const StructLiteral *sl_value = dynamic_cast<const StructLiteral *>(&node)) {
		for (const ExpressionPtr &ep_value : sl_value->fields) expression(*ep_value, path);
	} else if (const InterpolatedString *is_text = dynamic_cast<const InterpolatedString *>(&node)) {
		for (const ExpressionPtr &ep_value : is_text->parts) {
			const Handle h_value = scalar(*ep_value, path);
			if (h_value.identity) access(h_value, path, ep_value->get_span(), directSelf(*ep_value), rc_checks.atAccess);
			const decltype(bf_facts.textConversions)::const_iterator it_format = bf_facts.textConversions.find(ep_value.get());
			if (it_format != bf_facts.textConversions.end() && it_format->second.callsFormatter) unknownEffect(path);
		}
	}
	if (b_emit) {
		ReceiverChecks &rc_out = ei_info.receiverChecks;
		rc_out.beforeArguments |= rc_checks.beforeArguments;
		rc_out.atAccess |= rc_checks.atAccess;
		rc_out.activeState |= rc_checks.activeState;
		if (rc_checks.knownState) {
			if (rc_out.knownState && rc_out.knownState != rc_checks.knownState) rc_out.activeState = true;
			rc_out.knownState = rc_checks.knownState;
		}
		if (rc_out.activeState) rc_out.knownState = nullptr;
	}
	path.reachable &= ei_info.fallsThrough;
	if (!path.reachable) return {};
	return v_result.empty() ? typedValues(ei_info) : v_result;
}

StorageOwner LifetimeChecker::storageOwner (const Expression &target) const {
	const decltype(bf_facts.expressions)::const_iterator it_target = bf_facts.expressions.find(&target);
	const Symbol                                        *sym_field = it_target == bf_facts.expressions.end() ? nullptr : it_target->second.symbol;
	const bool b_classField = sym_field && sym_field->kind == SymbolKind::Field && asClass(a_analyzer.declarations.at(sym_field).owner);
	if (const FieldAccessExpression *fa_field = dynamic_cast<const FieldAccessExpression *>(&target)) {
		return b_classField ? StorageOwner{ fa_field->receiver.get() } : storageOwner(*fa_field->receiver);
	}
	if (const IndexExpression *ie_index = dynamic_cast<const IndexExpression *>(&target)) return storageOwner(*ie_index->array);
	return { nullptr, b_classField };
}

void LifetimeChecker::write (const Statement &node, const Expression &target, const ExpressionPtr &value, const ExpressionPtr &compound, Path &path) {
	const FieldAccessExpression *fa_field   = dynamic_cast<const FieldAccessExpression *>(&target);
	const IndexExpression       *ie_index   = dynamic_cast<const IndexExpression *>(&target);
	const Expression            &e_receiver = fa_field ? *fa_field->receiver : *ie_index->array;
	const StorageOwner           so_owner   = storageOwner(target);
	if (so_owner.receiver) m_observed.try_emplace(so_owner.receiver);
	const Handle h_receiver = scalar(e_receiver, path);
	const Handle h_owner    = so_owner.receiver ? m_observed.at(so_owner.receiver) : so_owner.implicit ? path.bindings[&lb_job.context->name] : Handle{};
	if (so_owner.receiver) m_observed.erase(so_owner.receiver);
	const bool     b_ownerSelf = so_owner.implicit || (so_owner.receiver && directSelf(*so_owner.receiver));
	ReceiverChecks rc_checks;
	access(h_owner, path, target.get_span(), b_ownerSelf, rc_checks.beforeArguments);
	const decltype(bf_facts.writes)::const_iterator it_write = bf_facts.writes.find(&node);
	const bool b_setter = ie_index && it_write != bf_facts.writes.end() && it_write->second.symbol->name.get_lexeme() == to_string(oIndexWrite);
	if (b_setter) access(h_receiver, path, e_receiver.get_span(), directSelf(e_receiver), rc_checks.beforeArguments);
	if (ie_index) expression(*ie_index->index, path);
	const decltype(bf_facts.expressions)::iterator it_read  = bf_facts.expressions.find(&target);
	const bool                                     b_getter = b_setter && it_read != bf_facts.expressions.end() && it_read->second.argumentContract;
	if (b_setter) {
		access(h_receiver, path, e_receiver.get_span(), directSelf(e_receiver), rc_checks.beforeArguments);
		unknownEffect(path);
	}
	if (b_getter) {
		bool b_runtime = false;
		access(h_receiver, path, e_receiver.get_span(), directSelf(e_receiver), b_runtime);
		if (b_emit) it_read->second.receiverChecks.atAccess |= b_runtime;
		unknownEffect(path);
	}
	const Handle h_target = it_read != bf_facts.expressions.end() && it_read->second.types.size() == 1 ? fresh(it_read->second.types.front()) : Handle{};
	const bool   b_update = it_write != bf_facts.writes.end() && !b_setter;
	const ExpressionInfo *ei_binary    = compound && bf_facts.expressions.contains(compound.get()) ? &bf_facts.expressions.at(compound.get()) : nullptr;
	const bool            b_binaryCall = ei_binary && ei_binary->argumentContract;
	const bool b_right = b_binaryCall && dynamic_cast<const MethodDeclaration &>(*callable(*ei_binary->argumentContract)).operatorMember->rightReceiver;
	if (b_update || (b_binaryCall && !b_right)) access(h_target, path, target.get_span(), false, rc_checks.beforeArguments);
	const Handle h_rhs = value ? scalar(*value, path) : Handle{};
	if (b_binaryCall) {
		access(b_right ? h_rhs : h_target, path, target.get_span(), false, rc_checks.atAccess);
		unknownEffect(path);
	}
	if (b_update) access(h_target, path, target.get_span(), false, rc_checks.atAccess);
	if (b_setter) {
		access(h_receiver, path, e_receiver.get_span(), directSelf(e_receiver), rc_checks.atAccess);
		unknownEffect(path);
		access(h_receiver, path, e_receiver.get_span(), directSelf(e_receiver), rc_checks.atAccess);
	}
	access(h_owner, path, target.get_span(), b_ownerSelf, rc_checks.atAccess);
	if (it_write != bf_facts.writes.end()) unknownEffect(path);
	if (b_emit) {
		ReceiverChecks &rc_write = bf_facts.writeChecks[&node];
		rc_write.beforeArguments |= rc_checks.beforeArguments;
		rc_write.atAccess |= rc_checks.atAccess;
	}
}

void LifetimeChecker::statements (const std::vector<StatementPtr> &body, Path &path) {
	path.values.clear();
	for (const StatementPtr &sp_statement : body) {
		if (!path.reachable) break;
		statement(*sp_statement, path);
	}
}

Values LifetimeChecker::runDeferred (const FunctionBlockExpression &block, Path &path) {
	OptionalPath  returned;
	OptionalPath *p_outer = std::exchange(p_returns, &returned);
	statements(std::get<BlockBody>(block.declaration.body.value).statements, path);
	p_returns = p_outer;
	join(returned, path);
	if (!returned) {
		path.reachable = false;
		return {};
	}
	path = std::move(*returned);
	return std::move(path.values);
}

void LifetimeChecker::statement (const Statement &node, Path &path) {
	path.values.clear();
	if (const ExpressionStatement *es_expression = dynamic_cast<const ExpressionStatement *>(&node)) {
		path.values = expression(*es_expression->expression, path);
	} else if (const ReturnStatement *rs_return = dynamic_cast<const ReturnStatement *>(&node)) {
		if (rs_return->value) path.values = expression(*rs_return->value, path);
		if (p_returns) join(*p_returns, path);
		path.reachable = false;
	} else if (const BindingDeclaration *bd_binding = dynamic_cast<const BindingDeclaration *>(&node)) {
		if (const FunctionBlockExpression *fb_body = dynamic_cast<const FunctionBlockExpression *>(bd_binding->initializer.get())) {
			m_deferred[&bd_binding->name] = fb_body;
			Path p_deferred               = path;
			unknownEffect(p_deferred);
			for (std::map<const Token *, Handle>::value_type &entry : p_deferred.bindings) {
				const decltype(bf_facts.localTypes)::const_iterator it_type = bf_facts.localTypes.find(entry.first);
				if (it_type != bf_facts.localTypes.end()) entry.second = fresh(it_type->second);
			}
			runDeferred(*fb_body, p_deferred);
		} else
			store(bd_binding->name, scalar(*bd_binding->initializer, path), path);
	} else if (const AssignmentStatement *as_assignment = dynamic_cast<const AssignmentStatement *>(&node)) {
		const Token   *t_binding = binding(as_assignment->name);
		const bool     b_field   = !bf_facts.localTypes.contains(t_binding);
		ReceiverChecks checks;
		if (b_field) access(path.bindings[&lb_job.context->name], path, as_assignment->name.get_span(), true, checks.beforeArguments);
		if (const BinaryExpression *be_compound = as_assignment->get_compound(); be_compound && bf_facts.writes.contains(&node)) {
			const Handle h_receiver = scalar(*be_compound->left, path);
			access(h_receiver, path, be_compound->left->get_span(), false, checks.beforeArguments);
			expression(*be_compound->right, path);
			access(h_receiver, path, be_compound->left->get_span(), false, checks.atAccess);
			unknownEffect(path);
		} else {
			const Handle h_value = scalar(*as_assignment->value, path);
			if (b_field)
				access(path.bindings[&lb_job.context->name], path, as_assignment->name.get_span(), true, checks.atAccess);
			else if (path.reachable)
				store(*t_binding, h_value, path);
		}
		if (b_emit && (b_field || bf_facts.writes.contains(&node))) {
			ReceiverChecks &rc_write = bf_facts.writeChecks[&node];
			rc_write.beforeArguments |= checks.beforeArguments;
			rc_write.atAccess |= checks.atAccess;
		}
	} else if (const BindingGroupDeclaration *bg_group = dynamic_cast<const BindingGroupDeclaration *>(&node)) {
		const Values values = expression(*bg_group->initializer, path);
		for (std::size_t st_index = 0; path.reachable && st_index < bg_group->targets.size(); ++st_index)
			if (std::holds_alternative<ReceivingTarget::Named>(bg_group->targets[st_index].value))
				store(bg_group->targets[st_index].get_token(), values.at(values.size() == 1 ? 0 : st_index), path);
	} else if (const MultiAssignmentStatement *ma_assignment = dynamic_cast<const MultiAssignmentStatement *>(&node)) {
		const Values values = expression(*ma_assignment->value, path);
		for (std::size_t st_index = 0; path.reachable && st_index < ma_assignment->targets.size(); ++st_index) {
			if (std::holds_alternative<ReceivingTarget::Discard>(ma_assignment->targets[st_index].value)) continue;
			const Token &t_name    = ma_assignment->targets[st_index].get_token();
			const Token *t_binding = binding(t_name);
			if (bf_facts.localTypes.contains(t_binding))
				store(*t_binding, values.at(values.size() == 1 ? 0 : st_index), path);
			else {
				bool b_runtime = false;
				access(path.bindings[&lb_job.context->name], path, t_name.get_span(), true, b_runtime);
				if (b_emit) bf_facts.writeChecks[&node].atAccess |= b_runtime;
			}
		}
	} else if (const FieldWriteStatement *fw_field = dynamic_cast<const FieldWriteStatement *>(&node)) {
		write(node, *fw_field->target, fw_field->value, fw_field->compound, path);
	} else if (const ElementAssignmentStatement *ea_element = dynamic_cast<const ElementAssignmentStatement *>(&node)) {
		write(node, *ea_element->target, ea_element->value, ea_element->compound, path);
	} else if (const UpdateStatement *us_update = dynamic_cast<const UpdateStatement *>(&node)) {
		const Handle h_value = scalar(*us_update->target, path);
		if (bf_facts.writes.contains(&node)) {
			ReceiverChecks checks;
			access(h_value, path, node.get_span(), false, checks.atAccess);
			if (b_emit) bf_facts.writeChecks[&node].atAccess |= checks.atAccess;
			unknownEffect(path);
		}
	} else if (const LoopJumpStatement *lj_jump = dynamic_cast<const LoopJumpStatement *>(&node)) {
		LoopPaths &lp_edges = m_jumps[bf_facts.jumpTargets.at(lj_jump)];
		join(lj_jump->isContinue() ? lp_edges.continues : lp_edges.breaks, path);
		path.reachable = false;
	} else if (const LoopStatement *ls_loop = dynamic_cast<const LoopStatement *>(&node))
		loop(*ls_loop, path);
}

void LifetimeChecker::loop (const LoopStatement &node, Path &path) {
	const ForStatement     *fs_for   = dynamic_cast<const ForStatement *>(&node);
	const WhileStatement   *ws_while = dynamic_cast<const WhileStatement *>(&node);
	const DoWhileStatement *ds_do    = dynamic_cast<const DoWhileStatement *>(&node);
	const LoopBody         &body     = fs_for ? fs_for->body : ws_while ? ws_while->body : ds_do->body;
	const LoopTarget       *target   = node.get_loop_target().get();
	if (fs_for) {
		if (const ForSource::Range *range = std::get_if<ForSource::Range>(&fs_for->source.value)) {
			expression(*range->start, path);
			expression(*range->end, path);
		} else {
			const Expression &source = *std::get<ForSource::Elements>(fs_for->source.value).expression;
			const Handle      handle = scalar(source, path);
			const auto        plan   = bf_facts.iterations.find(fs_for);
			if (plan != bf_facts.iterations.end() && plan->second.snapshot) {
				bool runtime = false;
				access(handle, path, source.get_span(), directSelf(source), runtime);
				if (b_emit) plan->second.snapshot->receiverChecks.atAccess |= runtime;
				unknownEffect(path);
			}
		}
	}
	if (!path.reachable) return;
	const Path                                             entry   = path;
	const std::function<Branches(LifetimeChecker &, Path)> iterate = [&] (LifetimeChecker &checker, Path header) {
		Branches paths = ws_while ? checker.condition(*ws_while->condition, header) :
		                            Branches{ header, fs_for ? OptionalPath{ header } : std::nullopt };
		if (!paths.yes) return paths;
		Path p_body = std::move(*paths.yes);
		if (fs_for) checker.store(fs_for->binding.name, {}, p_body);
		checker.statements(body.statements, p_body);
		LoopPaths lp_edges = std::move(checker.m_jumps[target]);
		checker.m_jumps.erase(target);
		checker.join(lp_edges.continues, p_body);
		if (lp_edges.continues && ds_do) {
			Branches tested    = checker.condition(*ds_do->condition, *lp_edges.continues);
			lp_edges.continues = std::move(tested.yes);
			if (tested.no) checker.join(paths.no, *tested.no);
		} else if (lp_edges.continues && fs_for && fs_for->stepper && fs_for->stepper->value) {
			checker.expression(*fs_for->stepper->value, *lp_edges.continues);
			if (!lp_edges.continues->reachable) lp_edges.continues.reset();
		}
		if (lp_edges.breaks) checker.join(paths.no, *lp_edges.breaks);
		return Branches{ std::move(lp_edges.continues), std::move(paths.no) };
	};
	Path                    header = entry;
	std::optional<Branches> stable;
	while (!header.bindings.empty() || !header.objects.empty()) {
		LifetimeChecker trial = *this;
		trial.b_emit          = false;
		OptionalPath returned;
		trial.p_returns      = p_returns ? &returned : nullptr;
		Branches edges       = iterate(trial, header);
		id_next              = trial.id_next;
		OptionalPath widened = header;
		if (edges.yes) join(widened, *edges.yes);
		if (!edges.yes || equivalent(header, *widened, entry)) {
			if (!b_emit) {
				m_jumps = std::move(trial.m_jumps);
				if (returned) join(*p_returns, *returned);
				stable = std::move(edges);
			}
			break;
		}
		header = std::move(*widened);
	}
	Branches result = stable ? std::move(*stable) : iterate(*this, std::move(header));
	if (result.no)
		path = std::move(*result.no);
	else
		path.reachable = false;
}

void LifetimeChecker::check () {
	const Symbol &sym_context = *lb_job.context;
	Path          p_entry;
	if (lb_job.host) {
		const Handle h_self = fresh(*lb_job.host);
		if (h_self.identity) {
			p_entry.bindings[&sym_context.name] = h_self;
			ObjectFacts &of_self                = p_entry.objects[h_self.identity];
			of_self.lifetime                    = sym_context.kind == SymbolKind::Destructor ? Lifetime::Destroying : Lifetime::Unknown;
			if (lb_job.state) {
				if (const TypeDispatch *td_host = table(*lb_job.host)) {
					const StateSelections::const_iterator it_entry = td_host->states.find(lb_job.state->name.get_lexeme());
					if (it_entry != td_host->states.end() && it_entry->second.definition == lb_job.state)
						of_self.active = Activation{ it_entry->second.selection, *lb_job.host,
							                     sym_context.kind != SymbolKind::StateInitializer };
				}
			}
		}
	}
	if (const CallableSignature *cs_callable = callable(sym_context)) {
		for (const ParameterDeclaration &pd_parameter : cs_callable->parameters) {
			if (pd_parameter.defaultValue) {
				Path defaults = p_entry;
				expression(*pd_parameter.defaultValue, defaults);
			}
			store(pd_parameter.name, {}, p_entry);
		}
		if (const ConstructorDeclaration *cd_declaration = dynamic_cast<const ConstructorDeclaration *>(cs_callable)) {
			arguments(cd_declaration->delegation, p_entry);
			unknownEffect(p_entry);
		}
		if (const FunctionBody *fb_body = cs_callable->executableBody()) {
			if (const BlockBody *bb_body = std::get_if<BlockBody>(&fb_body->value)) statements(bb_body->statements, p_entry);
			if (const ExpressionBody *eb_body = std::get_if<ExpressionBody>(&fb_body->value)) expression(*eb_body->expression, p_entry);
		}
	} else if (sym_context.kind == SymbolKind::StateInitializer) {
		const FunctionBody &fb_body = std::get<const StateInitializerDeclaration *>(sym_context.declaration)->body;
		statements(std::get<BlockBody>(fb_body.value).statements, p_entry);
	} else if (sym_context.kind == SymbolKind::State || sym_context.kind == SymbolKind::StateAttachment) {
		const StateSelectionDeclaration *ss_selection =
		        sym_context.kind == SymbolKind::StateAttachment ?
		                static_cast<const StateSelectionDeclaration *>(std::get<const StateAttachmentDeclaration *>(sym_context.declaration)) :
		                dynamic_cast<const NestedStateDeclaration *>(std::get<const StateBodyDeclaration *>(sym_context.declaration));
		if (ss_selection && ss_selection->initial && ss_selection->initial->payload) expression(*ss_selection->initial->payload, p_entry);
	} else if (sym_context.kind == SymbolKind::Field) {
		if (const ClassFieldDeclaration *const *cf_field = std::get_if<const ClassFieldDeclaration *>(&sym_context.declaration)) {
			if (const ClassDeclaration *cd_declaration = asClass(a_analyzer.declarations.at(&sym_context).owner))
				for (const ClassParameterDeclaration &cp_parameter : cd_declaration->parameters)
					store(cp_parameter.parameter.name, {}, p_entry);
			expression(*(*cf_field)->initializer, p_entry);
		} else if (const EnumFieldDeclaration *const *ef_field = std::get_if<const EnumFieldDeclaration *>(&sym_context.declaration);
		           ef_field && (*ef_field)->defaultValue)
			expression(*(*ef_field)->defaultValue, p_entry);
	} else if (sym_context.kind == SymbolKind::EnumCase) {
		for (const ExpressionPtr &ep_value : std::get<const EnumCaseDeclaration *>(sym_context.declaration)->values) expression(*ep_value, p_entry);
	} else if (const ClassDeclaration *cd_declaration = asClass(&sym_context)) {
		for (const ClassParameterDeclaration &cp_parameter : cd_declaration->parameters) store(cp_parameter.parameter.name, {}, p_entry);
		for (const ClassParentClause &parent : cd_declaration->parents)
			if (parent.constructorArguments)
				for (const CallArgument &arg : *parent.constructorArguments) expression(*arg.expression, p_entry);
	}
}

}

void Analyzer::recordLifetimeBody (LifetimeBody body, bool initializationMayCall) {
	if (initializationMayCall && body.host) m_initializationEffects[*body.host].insert(body.context);
	v_lifetimeBodies.push_back(std::move(body));
}

bool Analyzer::preservesNewReceiver (const ResolvedType &host, const Symbol &constructor) const {
	if (!asClass(host.declaration)) return false;
	// A base constructor may invoke code on the derived instance before its own constructor body starts.
	if (std::ranges::any_of(declarations.at(host.declaration).parents, [] (const ResolvedType &parent) { return asClass(parent.declaration) != nullptr; }))
		return false;
	const decltype(m_initializationEffects)::const_iterator it_effects = m_initializationEffects.find(host);
	if (it_effects == m_initializationEffects.end()) return true;
	const decltype(program.dispatchTables)::const_iterator it_host     = program.dispatchTables.find(host);
	const Symbol                                          *sym_initial = it_host == program.dispatchTables.end() ? nullptr : it_host->second.initialState;
	return std::ranges::none_of(it_effects->second, [&] (const Symbol *declaration) {
		return declaration == &constructor || declaration == sym_initial || declaration->kind == SymbolKind::Field;
	});
}

void Analyzer::checkLifetimes () {
	// Overloads, generic bodies, state tables, and declaration-site defaults are all available here.
	for (const LifetimeBody &lb_body : v_lifetimeBodies) {
		rt_activeAttachment = lb_body.state && !declarations.at(lb_body.state).owner && lb_body.host ? &*lb_body.host : nullptr;
		LifetimeChecker(*this, lb_body).check();
	}
	rt_activeAttachment = nullptr;
}

}
