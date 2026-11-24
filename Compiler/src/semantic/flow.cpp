// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/language/statements/ControlFlow.rst

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include <ast/decl/declarations.hpp>
#include <ast/decl/struct_declaration.hpp>
#include <ast/expr/expression.hpp>
#include <ast/expr/expressions.hpp>
#include <ast/expr/struct_expressions.hpp>
#include <ast/stmt/do_while_statement.hpp>
#include <ast/stmt/for_statement.hpp>
#include <ast/stmt/loop_control.hpp>
#include <ast/stmt/statement.hpp>
#include <ast/stmt/while_statement.hpp>
#include <diagnostics.hpp>
#include <semantic/checked_program.hpp>
#include <semantic/checker.hpp>
#include <semantic/symbol_table.hpp>
#include <token/numeric_literals.hpp>
#include <token/token.hpp>

namespace cloth::semantic {
namespace {
unsigned classDepth (const Analyzer &analyzer, const Symbol *type) {
	unsigned u_depth = 0;
	while (type) {
		++u_depth;
		const ResultTypes &v_parents = analyzer.declarations.at(type).parents;
		const auto         it_base   = std::ranges::find_if(v_parents, [] (const ResolvedType &parent) { return asClass(parent.declaration); });
		type                         = it_base == v_parents.end() ? nullptr : it_base->declaration;
	}
	return u_depth;
}
}

bool StoragePlace::stable () const {
	if (path.empty()) return true;
	return std::holds_alternative<const Symbol *>(path.back()) && std::ranges::none_of(path, [] (const PlaceSegment &segment) {
		       const IntegerValue *iv_index = std::get_if<IntegerValue>(&segment);
		       return iv_index && iv_index->empty();
	       });
}

bool StoragePlace::indexed () const {
	return std::ranges::any_of(path, [] (const PlaceSegment &segment) { return std::holds_alternative<IntegerValue>(segment); });
}

bool StoragePlace::covers (const StoragePlace &proof, bool descendantsOnly) const {
	if (root != proof.root || path.size() > proof.path.size() || (descendantsOnly && path.size() == proof.path.size())) return false;
	return std::equal(path.begin(), path.end(), proof.path.begin(), [] (const PlaceSegment &write, const PlaceSegment &read) {
		const IntegerValue *iv_index = std::get_if<IntegerValue>(&write);
		return write == read || (iv_index && iv_index->empty() && std::holds_alternative<IntegerValue>(read));
	});
}

void BodyChecker::intersect (Narrowing &facts, const Narrowing &other) {
	for (auto it = facts.begin(); it != facts.end();) {
		const auto peer = other.find(it->first);
		if (const bool b_shape = it->first.fact == PlaceFact::CurrentShape || it->first.fact == PlaceFact::RetainedShape;
		    peer == other.end() || (!b_shape && it->second.type != peer->second.type))
			it = facts.erase(it);
		else {
			if (b_shape) it->second.type = commonShape(it->second.type, peer->second.type);
			std::erase_if(it->second.nominalTests, [&] (const ResolvedType &type) {
				return std::ranges::find(peer->second.nominalTests, type) == peer->second.nominalTests.end();
			});
			++it;
		}
	}
}

void BodyChecker::join (std::optional<Narrowing> &facts, const Narrowing &other) {
	if (facts)
		intersect(*facts, other);
	else
		facts = other;
}

void BodyChecker::mergeExits (Exits &target, const Exits &source) {
	target.returned |= source.returned;
	const std::function merge = [] (JumpPaths &target, const JumpPaths &source) {
		for (const JumpPaths::value_type &entry : source) {
			const std::pair<JumpPaths::iterator, bool> it_path = target.try_emplace(entry.first, entry.second);
			if (!it_path.second) intersect(it_path.first->second, entry.second);
		}
	};
	merge(target.breaks, source.breaks);
	merge(target.continues, source.continues);
}

bool BodyChecker::statement (const Statement &node) {
	const Exits ex_incoming = std::exchange(fs_flow.exits, {});
	const bool  b_normal    = checkStatement(node);
	finishStatement(node, b_normal, ex_incoming);
	return b_normal;
}

void BodyChecker::finishStatement (const Statement &node, const bool fallsThrough, const Exits &exIncoming) {
	if (record) {
		ControlFlow cf_result{ .fallsThrough = fallsThrough, .canReturn = fs_flow.exits.returned, .breaks = {}, .continues = {} };
		for (const auto &key : fs_flow.exits.breaks | std::views::keys) cf_result.breaks.insert(key);
		for (const auto &key : fs_flow.exits.continues | std::views::keys) cf_result.continues.insert(key);
		bf_facts.controlFlow.insert_or_assign(&node, std::move(cf_result));
	}
	mergeExits(fs_flow.exits, exIncoming);
}

std::optional<BodyChecker::TypedPlace> BodyChecker::locate (const Expression &expression) const {
	std::optional<TypedPlace> tp_receiver;
	const Token              *t_field  = nullptr;
	const auto               *ie_index = dynamic_cast<const IndexExpression *>(&expression);
	if (const auto *ne_name = dynamic_cast<const NameExpression *>(&expression)) {
		if (const Binding *b_local = local(ne_name->name.getNameKey())) {
			if (!b_local->type || b_local->deferred) return std::nullopt;
			return TypedPlace{ .place = { .root = b_local->name }, .type = *b_local->type };
		}
		if (!hasInstance()) return std::nullopt;
		if (ne_name->name.getLexeme() == toString(KState) && owner && owner->kind == SymbolKind::State)
			return TypedPlace{ .place = { .root = &owner->name },
				           .type  = analyzer.substitute(*analyzer.declarations.at(owner).value, m_arguments) };
		tp_receiver = TypedPlace{ .place = { .root = &context.name }, .type = selfType() };
		if (ne_name->name.getLexeme() == toString(KSelf)) return tp_receiver;
		t_field = &ne_name->name;
	} else if (const auto *fa_field = dynamic_cast<const FieldAccessExpression *>(&expression)) {
		tp_receiver = locate(*fa_field->receiver);
		t_field     = &fa_field->name;
	} else if (ie_index) {
		tp_receiver = locate(*ie_index->array);
	}
	if (!tp_receiver) return std::nullopt;
	const ResolvedType rt_receiver = readType(tp_receiver->place, tp_receiver->type);
	if (ie_index) {
		// An overloaded getter produces a value rather than an address into array storage.
		if (!rt_receiver.array()) return std::nullopt;
		const auto *il_index = dynamic_cast<const IntegerLiteral *>(ie_index->index.get());
		tp_receiver->place.path.emplace_back(il_index ? il_index->value : IntegerValue{});
		tp_receiver->type = rt_receiver.element();
		return tp_receiver;
	}
	const std::vector<Candidate> v_fields = analyzer.members(rt_receiver, t_field->getNameKey(), SymbolKind::Field);
	if (v_fields.empty()) return std::nullopt;
	const Candidate &c_field = v_fields.front();
	tp_receiver->place.path.emplace_back(c_field.symbol);
	tp_receiver->type = analyzer.substitute(*analyzer.declarations.at(c_field.symbol).value, c_field.arguments);
	return tp_receiver;
}

BodyChecker::ReceiverProofs BodyChecker::receiverProofs (const Expression *receiver) const {
	ReceiverProofs v_proofs;
	// Value receivers borrow storage. Remember each nullable projection needed to find it again after the arguments run.
	while (receiver) {
		const std::optional<TypedPlace> tp_part = locate(*receiver);
		if (tp_part && tp_part->type.nullable() && fs_flow.narrowed.contains(tp_part->place))
			v_proofs.push_back({ .place = tp_part->place, .span = receiver->getSpan() });
		if (const auto *fa_field = dynamic_cast<const FieldAccessExpression *>(receiver))
			receiver = fa_field->receiver.get();
		else if (const auto *ie_index = dynamic_cast<const IndexExpression *>(receiver))
			receiver = ie_index->array.get();
		else
			break;
	}
	return v_proofs;
}

void BodyChecker::requireReceiverProofs (const ReceiverProofs &proofs) const {
	for (const ReceiverProof &rp_proof : std::views::reverse(proofs))
		if (!fs_flow.narrowed.contains(rp_proof.place))
			fail(Code::NullablePayloadAccess, rp_proof.span, "Receiver storage must remain present after evaluating the arguments");
}

void BodyChecker::setPresence (const StoragePlace &place, const ResolvedType &declared, const std::optional<ResolvedType> &value) {
	if (declared.nullable() && value && !analyzer.mayBeAbsent(*value)) fs_flow.narrowed.insert_or_assign(place, FlowFact{ .type = declared.element() });
}

void BodyChecker::assumePresent (const Expression &source) {
	const std::optional<TypedPlace> tp_source = locate(source);
	if (tp_source && tp_source->place.stable() && tp_source->type.nullable()) fs_flow.narrowed.try_emplace(tp_source->place, tp_source->type.element());
}

ResolvedType BodyChecker::readType (const StoragePlace &place, const ResolvedType &declared) const {
	const auto it_fact = fs_flow.narrowed.find(place);
	if (it_fact == fs_flow.narrowed.end()) return declared;
	const FlowFact &ff_proof = it_fact->second;
	if (ff_proof.nominalTests.empty()) return ff_proof.type;
	ResolvedType rt_view = ff_proof.type;
	if (rt_view.parameter) {
		const ResultTypes v_bounds = analyzer.typeBounds(rt_view);
		const auto        it_class = std::ranges::find_if(v_bounds, [] (const ResolvedType &bound) { return asClass(bound.declaration); });
		if (!v_bounds.empty()) rt_view = it_class == v_bounds.end() ? v_bounds.front() : *it_class;
	}
	const bool                        b_class  = asClass(rt_view.declaration);
	const ResolvedType               *rt_class = nullptr;
	std::vector<const ResolvedType *> v_interfaces;
	for (const ResolvedType &rt_test : ff_proof.nominalTests) {
		const bool b_interface = asInterface(rt_test.declaration);
		if (!(b_class && b_interface) && analyzer.accepts(rt_test, ff_proof.type)) continue;
		if (b_interface)
			v_interfaces.push_back(&rt_test);
		else if ((!b_class || analyzer.accepts(rt_view, rt_test)) &&
		         (!rt_class || classDepth(analyzer, rt_test.declaration) > classDepth(analyzer, rt_class->declaration)))
			rt_class = &rt_test;
	}
	if (rt_class) return *rt_class;
	// An interface view must satisfy every surviving interface contract and be the unique such view.
	const ResolvedType *rt_interface = nullptr;
	for (const ResolvedType *rt_candidate : v_interfaces) {
		if (!std::ranges::all_of(v_interfaces, [&] (const ResolvedType *view) { return analyzer.accepts(*view, *rt_candidate); })) continue;
		if (rt_interface) return ff_proof.type;
		rt_interface = rt_candidate;
	}
	return rt_interface ? *rt_interface : ff_proof.type;
}

void BodyChecker::narrowRead (ExpressionInfo &value, const StoragePlace &place) const {
	value.types.front() = readType(place, value.types.front());
}

void BodyChecker::forgetPresence (Narrowing &facts, const StoragePlace &place) {
	std::erase_if(facts, [&] (const Narrowing::value_type &entry) { return entry.first.fact == PlaceFact::Presence && place.covers(entry.first); });
}

bool BodyChecker::sharedPlace (const StoragePlace &place) const {
	if (place.path.empty() || !analyzer.hasReferenceDeclarations()) return false;
	if (place.root == &context.name || (owner && owner->kind == SymbolKind::State && place.root == &owner->name)) return true;
	return std::ranges::any_of(place.path, [&] (const PlaceSegment &segment) {
		const Symbol *const *sym_field = std::get_if<const Symbol *>(&segment);
		return sym_field && asClass(analyzer.declarations.at(*sym_field).owner);
	});
}

void BodyChecker::invalidate (const StoragePlace &place, bool descendantsOnly) {
	if (we_capture) (descendantsOnly ? we_capture->receivers : we_capture->places).insert(place);
	if (place.fact != PlaceFact::Presence) {
		fs_flow.narrowed.erase(place);
		return;
	}
	const bool b_sharedWrite = !descendantsOnly && sharedPlace(place);
	std::erase_if(fs_flow.narrowed, [&] (const Narrowing::value_type &entry) {
		const StoragePlace &sp_proof = entry.first;
		if (sp_proof.fact == PlaceFact::RetainedShape) return false;
		if (place.covers(sp_proof, descendantsOnly)) return true;
		return b_sharedWrite && sharedPlace(sp_proof);
	});
}

void BodyChecker::mutatingReceiver (const Expression &receiver, const ResolvedType &type) {
	if (!type.declaration || type.declaration->kind != SymbolKind::Type ||
	    !dynamic_cast<const StructDeclaration *>(std::get<const TypeDeclaration *>(type.declaration->declaration)))
		return;
	if (const std::optional<TypedPlace> tp_receiver = locate(receiver)) invalidate(tp_receiver->place, true);
}

void BodyChecker::invalidateShared () {
	if (we_capture) we_capture->sharedFields = true;
	forgetShared();
}

void BodyChecker::forgetShared (const bool indexedOnly) {
	std::erase_if(fs_flow.narrowed,
	              [&] (const Narrowing::value_type &entry) { return (!indexedOnly || entry.first.indexed()) && sharedPlace(entry.first); });
}

void BodyChecker::forgetMutableMetadata (Narrowing &facts) const {
	if (facts.empty()) return;
	for (const std::map<std::string, Binding> &m_locals : scopes)
		for (const auto &val : m_locals | std::views::values)
			if (val.mutableBinding) facts.erase({ .root = val.name, .path = {}, .fact = PlaceFact::RepresentedType });
}

void BodyChecker::applyWrites (const Binding &binding) {
	if (!binding.effects) return;
	for (const StoragePlace &sp_place : binding.effects->places) invalidate(sp_place);
	for (const StoragePlace &sp_receiver : binding.effects->receivers) invalidate(sp_receiver, true);
	if (binding.effects->sharedFields && we_capture) we_capture->sharedFields = true;
	forgetShared();
}

BodyChecker::TestPaths BodyChecker::splitTest (const Expression &node, const ExpressionInfo &result) {
	if (result.fallsThrough && e_test == &node) return tp_test;
	TestPaths tp_paths{ .yes = { .facts = fs_flow.narrowed, .reachable = result.fallsThrough },
		            .no  = { .facts = fs_flow.narrowed, .reachable = result.fallsThrough } };
	if (!result.fallsThrough) return tp_paths;
	if (const std::optional<bool> opt_truth = result.integerTruth()) {
		(*opt_truth ? tp_paths.no : tp_paths.yes).reachable = false;
	} else if (const auto *be_test = dynamic_cast<const BinaryExpression *>(&node); be_test && !result.argumentContract) {
		const Operator o_op = be_test->op.getOperator();
		if (o_op != OEqual && o_op != ONotEqual && o_op != OIdentity) return tp_paths;
		const Expression               *e_operand = dynamic_cast<const NullLiteral *>(be_test->left.get())  ? be_test->right.get() :
		                                            dynamic_cast<const NullLiteral *>(be_test->right.get()) ? be_test->left.get() :
		                                                                                                      nullptr;
		const std::optional<TypedPlace> tp_place  = e_operand ? locate(*e_operand) : std::nullopt;
		if (tp_place && tp_place->place.stable() && tp_place->type.nullable()) {
			(o_op == ONotEqual ? tp_paths.yes : tp_paths.no).facts.try_emplace(tp_place->place, tp_place->type.element());
			forgetPresence((o_op == ONotEqual ? tp_paths.no : tp_paths.yes).facts, tp_place->place);
		}
	} else if (const auto *tc_test = dynamic_cast<const TypeCheckExpression *>(&node); tc_test && result.testedType && result.testedType->declaration) {
		const std::optional<TypedPlace> tp_place  = locate(*tc_test->operand);
		const ResolvedType             &rt_target = *result.testedType;
		if (tp_place && tp_place->place.stable() && analyzer.referenceType(rt_target)) {
			FlowFact &ff_proof =
			        tp_paths.yes.facts.try_emplace(tp_place->place, tp_place->type.nullable() ? tp_place->type.element() : tp_place->type)
			                .first->second;
			// Fields prove presence. Only a binding can expose the tested class or interface contract.
			if (tp_place->place.path.empty() && tp_place->place.root != &context.name && analyzer.referenceType(ff_proof.type)) {
				const std::span<const ResolvedType> sp_tests =
				        asInterface(rt_target.declaration) ? std::span(analyzer.dispatchFor(rt_target).interfaces) : std::span(&rt_target, 1);
				for (const ResolvedType &rt_test : sp_tests)
					if (std::ranges::find(ff_proof.nominalTests, rt_test) == ff_proof.nominalTests.end())
						ff_proof.nominalTests.push_back(rt_test);
			}
		}
	}
	return tp_paths;
}

ExpressionInfo BodyChecker::testOperand (const Expression &node) {
	if (record || ef_probeConstants) return scalarInfo(node);
	// Loop and overload trials must see the same constant truth values as the committed body.
	ExpressionFacts ef_constants;

	struct ResetFacts {
		ExpressionFacts *&facts;

		~ResetFacts () {
			facts = nullptr;
		}
	} const rf_scope{ ef_probeConstants };

	ef_probeConstants = &ef_constants;
	return scalarInfo(node);
}

BodyChecker::TestPaths BodyChecker::test (const Expression &node) {
	const ExpressionInfo ei_test = testOperand(node);
	if (ei_test.fallsThrough && ei_test.types.front() != ResolvedType(KBoolean))
		fail(Code::NonBooleanCondition, node.getSpan(), "A condition must have type " + std::string(toString(KBoolean)));
	return splitTest(node, ei_test);
}

bool BodyChecker::loop (const LoopStatement &node) {
	const auto       *fs_for    = dynamic_cast<const ForStatement *>(&node);
	const auto       *ws_while  = dynamic_cast<const WhileStatement *>(&node);
	const auto       *ds_do     = dynamic_cast<const DoWhileStatement *>(&node);
	const LoopTarget *lt_target = node.getLoopTarget().get();
	const LoopBody   &lb_body   = fs_for ? fs_for->body : ws_while ? ws_while->body : ds_do->body;
	bool              b_source  = true;
	if (fs_for) {
		std::optional<ResolvedType> annotation;
		if (fs_for->binding.type) {
			annotation = bodyType(*fs_for->binding.type);
			if (*annotation == ResolvedType(KVoid))
				fail(Code::InvalidForLoop, fs_for->binding.getSpan(), "An iteration binding requires a value type");
		}
		std::optional<ResolvedType> opt_element;
		const Expression           &source   = fs_for->source.getExpression();
		std::optional<ResolvedType> opt_hint = annotation;
		const bool                  b_range  = std::holds_alternative<ForSource::Range>(fs_for->source.value);
		if (annotation && b_range) {
			opt_hint = ResolvedType(KRange);
			analyzer.applyArguments(*opt_hint, { *annotation });
		} else if (opt_hint)
			opt_hint->suffixes.push_back(arraySuffix());
		std::optional<ResolvedType> opt_source;
		try {
			opt_source = scalar(source, opt_hint ? &*opt_hint : nullptr);
		} catch (const cde::ClothDiagnostic &error) {
			if (!b_range || error.getCode() != Code::InvalidRangeValue) throw;
			fail(Code::InvalidForLoop, source.getSpan(), "A range loop requires endpoints of one numeric type");
		}
		if (!opt_source) {
			b_source = false;
		} else if (opt_source->array()) {
			opt_element = opt_source->element();
			if (record) bf_facts.iterations[fs_for] = { .kind = IterationKind::Array, .element = *opt_element };
		} else if (const ResolvedType *rt_endpoint = rangeEndpoint(*opt_source)) {
			requirePresentOperand(*opt_source, source.getSpan());
			opt_element = *rt_endpoint;
			if (record) bf_facts.iterations[fs_for] = { .kind = IterationKind::Range, .element = *opt_element };
		} else {
			const std::vector<Candidate> group = analyzer.members(*opt_source, toString(OForeach), SymbolKind::Operator);
			if (group.empty()) fail(Code::InvalidForLoop, source.getSpan(), "Expected a range, array, or collection with an iteration operator");
			if (readOnlyDefaultRoot(source))
				fail(Code::InvalidParameterDefault, source.getSpan(), "A default cannot iterate a read-only receiver", &pd_default->name);
			ExpressionInfo           call     = invoke(group.front().symbol->name, source.getSpan(), {}, group);
			std::optional<Iteration> snapshot = call.types.size() == 1 ? analyzer.snapshotLayout(call.types.front()) : std::nullopt;
			if (!snapshot) fail(Code::InvalidForLoop, source.getSpan(), "Iteration requires the standard library snapshot");
			mutatingReceiver(source, *opt_source);
			opt_element        = snapshot->element;
			snapshot->snapshot = std::move(call);
			if (record) bf_facts.iterations[fs_for] = std::move(*snapshot);
		}
		if (!opt_element && !annotation)
			fail(Code::InvalidForLoop, fs_for->source.getSpan(), "A noncontinuing loop source requires an element annotation");
		if (annotation && opt_element) {
			require({ *opt_element }, { *annotation }, fs_for->binding.getSpan(), Code::InvalidForLoop);
		}
		if (!annotation && opt_element) checkInferredBinding(*opt_element, fs_for->source.getSpan());
		scopes.emplace_back();
		bind(fs_for->binding.name, annotation ? annotation : opt_element, true);
	}
	if (!ds_do) forgetMutableMetadata(fs_flow.narrowed);

	struct Iteration {
		std::optional<Narrowing> back, exit;
		Exits                    outward;
	};

	const std::function iteration = [&] (BodyChecker &body) {
		Iteration it_result;
		bool      b_body = b_source;
		if (ws_while) {
			const TestPaths tp_paths = body.test(*ws_while->condition);
			if (tp_paths.no.reachable) it_result.exit = tp_paths.no.facts;
			b_body                = tp_paths.yes.reachable;
			body.fs_flow.narrowed = tp_paths.yes.facts;
		} else if (fs_for && b_source)
			it_result.exit = body.fs_flow.narrowed;
		WriteEffects *we_entry = body.we_capture;
		if (!b_body) body.we_capture = nullptr;
		const LoopTarget *lt_outer = std::exchange(body.lt_loop, lt_target);
		Exits const       ex_test  = std::exchange(body.fs_flow.exits, {});
		const bool        b_normal = body.block(lb_body.statements).fallsThrough;
		body.lt_loop               = lt_outer;
		if (!b_body) body.fs_flow.exits = {};
		if (b_normal && b_body) it_result.back = body.fs_flow.narrowed;
		for (const bool b_continue : { false, true }) {
			JumpPaths &m_jumps = b_continue ? body.fs_flow.exits.continues : body.fs_flow.exits.breaks;
			const auto it_jump = m_jumps.find(lt_target);
			if (it_jump != m_jumps.end()) {
				join(b_continue ? it_result.back : it_result.exit, it_jump->second);
				m_jumps.erase(it_jump);
			}
		}
		it_result.outward = std::exchange(body.fs_flow.exits, {});
		mergeExits(it_result.outward, ex_test);
		const bool b_trailer = it_result.back.has_value();
		body.we_capture      = b_trailer ? we_entry : nullptr;
		if (b_trailer)
			body.fs_flow.narrowed = *it_result.back;
		else
			body.forgetMutableMetadata(body.fs_flow.narrowed);
		if (ds_do) {
			const TestPaths tp_paths = body.test(*ds_do->condition);
			if (b_trailer) {
				if (tp_paths.yes.reachable)
					it_result.back = tp_paths.yes.facts;
				else
					it_result.back.reset();
				if (tp_paths.no.reachable) join(it_result.exit, tp_paths.no.facts);
			}
		} else if (fs_for && fs_for->stepper) {
			body.remember(fs_for->stepper->target, fs_for->binding.name);
			if (const Expression *e_update = fs_for->stepper->update()) {
				const Binding                    &b_index = *body.local(fs_for->binding.name.getNameKey());
				const std::optional<ResolvedType> rt_step = body.scalar(*e_update, b_index.type ? &*b_index.type : nullptr);
				if (rt_step && b_index.type) body.require({ *rt_step }, { *b_index.type }, fs_for->stepper->span);
				if (body.record && rt_step && rt_step->floating())
					requireFinite(*fs_for->stepper->value, body.bf_facts.expressions, Code::InvalidForLoop);
				if (!rt_step) it_result.back.reset();
			}
			if (it_result.back) it_result.back = body.fs_flow.narrowed;
		}
		if (b_trailer) mergeExits(it_result.outward, body.fs_flow.exits);
		body.we_capture = we_entry;
		return it_result;
	};
	const std::function<Iteration(const Narrowing &)> fn_trial = [&] (const Narrowing &header) {
		BodyChecker bc_trial = *this;
		bc_trial.record      = false;
		bc_trial.we_capture  = nullptr;
		bc_trial.fs_flow     = { .narrowed = header, .exits = {} };
		return iteration(bc_trial);
	};
	// A pre-test loop checks every occurrence against its source writes, including paths that never execute.
	// A post-test loop keeps first-entry proofs when no path reaches a second iteration.
	bool                     b_sourceRepeats = false;
	Narrowing                n_header        = loopPresence(node, b_sourceRepeats);
	std::optional<Iteration> opt_trial;
	if (ds_do && n_header != fs_flow.narrowed) {
		bool b_writtenHeader = b_sourceRepeats;
		try {
			opt_trial = fn_trial(b_writtenHeader ? n_header : fs_flow.narrowed);
		} catch (const cde::ClothDiagnostic &) {
			// An overloaded condition can change whether the loop repeats. Prefer the source diagnostic if neither header type-checks.
			b_writtenHeader = !b_writtenHeader;
			try {
				opt_trial = fn_trial(b_writtenHeader ? n_header : fs_flow.narrowed);
			} catch (const cde::ClothDiagnostic &) {
				opt_trial.reset();
			}
			if (!opt_trial || opt_trial->back.has_value() != b_writtenHeader) throw;
		}
		const bool b_repeats = opt_trial->back.has_value();
		if (!b_repeats) n_header = fs_flow.narrowed;
		if (b_repeats != b_writtenHeader) opt_trial.reset();
	}
	// Other facts, such as array shapes, still join over the actual back edges.
	while (!n_header.empty()) {
		if (!opt_trial) opt_trial = fn_trial(n_header);
		Narrowing n_next = n_header;
		if (opt_trial->back) {
			if (ds_do) forgetMutableMetadata(n_next);
			intersect(n_next, *opt_trial->back);
		}
		if (n_next == n_header) break;
		opt_trial.reset();
		n_header = std::move(n_next);
	}
	Exits ex_incoming         = std::exchange(fs_flow.exits, {});
	fs_flow.narrowed          = std::move(n_header);
	const Iteration it_result = opt_trial && !record && !we_capture ? std::move(*opt_trial) : iteration(*this);
	fs_flow.exits             = std::move(ex_incoming);
	mergeExits(fs_flow.exits, it_result.outward);
	if (it_result.exit) fs_flow.narrowed = *it_result.exit;
	if (!ds_do || !it_result.exit) forgetMutableMetadata(fs_flow.narrowed);
	if (fs_for) leaveScope();
	return it_result.exit.has_value();
}

}
