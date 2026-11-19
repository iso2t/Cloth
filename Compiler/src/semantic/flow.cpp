// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <functional>
#include <iterator>
#include <ranges>
#include <utility>

#include <ast/stmt/do_while_statement.hpp>
#include <ast/stmt/for_statement.hpp>
#include <ast/stmt/while_statement.hpp>
#include <semantic/checker.hpp>

namespace cloth::semantic {

void BodyChecker::intersect (Narrowing &facts, const Narrowing &other) {
	for (Narrowing::iterator it = facts.begin(); it != facts.end();) {
		const Narrowing::const_iterator peer    = other.find(it->first);
		const bool                      b_shape = it->first.fact == PlaceFact::CurrentShape || it->first.fact == PlaceFact::RetainedShape;
		if (peer == other.end() || (!b_shape && it->second != peer->second))
			it = facts.erase(it);
		else {
			if (b_shape) it->second = commonShape(it->second, peer->second);
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

void BodyChecker::finishStatement (const Statement &node, bool b_normal, const Exits &ex_incoming) {
	if (record) {
		ControlFlow cf_result{ .fallsThrough = b_normal, .canReturn = fs_flow.exits.returned, .breaks = {}, .continues = {} };
		for (const auto &key : fs_flow.exits.breaks | std::views::keys) cf_result.breaks.insert(key);
		for (const auto &key : fs_flow.exits.continues | std::views::keys) cf_result.continues.insert(key);
		bf_facts.controlFlow.insert_or_assign(&node, std::move(cf_result));
	}
	mergeExits(fs_flow.exits, ex_incoming);
}

std::optional<BodyChecker::TypedPlace> BodyChecker::locate (const Expression &expression) const {
	std::optional<TypedPlace> tp_receiver;
	const Token              *t_field = nullptr;
	if (const auto *ne_name = dynamic_cast<const NameExpression *>(&expression)) {
		if (const Binding *b_local = local(ne_name->name.get_lexeme())) {
			if (!b_local->type || b_local->deferred) return std::nullopt;
			return TypedPlace{ { b_local->name }, *b_local->type };
		}
		if (!hasInstance()) return std::nullopt;
		tp_receiver = TypedPlace{ { &context.name }, selfType() };
		if (ne_name->name.get_lexeme() == to_string(kSelf)) return tp_receiver;
		t_field = &ne_name->name;
	} else if (const auto *fa_field = dynamic_cast<const FieldAccessExpression *>(&expression)) {
		tp_receiver = locate(*fa_field->receiver);
		t_field     = &fa_field->name;
	}
	if (!tp_receiver) return std::nullopt;
	const auto                   it_proof    = fs_flow.narrowed.find(tp_receiver->place);
	const ResolvedType          &rt_receiver = it_proof == fs_flow.narrowed.end() ? tp_receiver->type : it_proof->second;
	const std::vector<Candidate> v_fields    = analyzer.members(rt_receiver, t_field->get_lexeme(), SymbolKind::Field);
	if (v_fields.empty()) return std::nullopt;
	const Candidate &c_field = v_fields.front();
	tp_receiver->place.fields.push_back(c_field.symbol);
	tp_receiver->type = analyzer.substitute(*analyzer.declarations.at(c_field.symbol).value, c_field.arguments);
	return tp_receiver;
}

BodyChecker::ReceiverProofs BodyChecker::receiverProofs (const Expression *receiver) const {
	ReceiverProofs v_proofs;
	// Value receivers borrow storage. Remember each nullable projection needed to find it again after the arguments run.
	while (receiver) {
		const std::optional<TypedPlace> tp_part = locate(*receiver);
		if (tp_part && tp_part->type.nullable() && fs_flow.narrowed.contains(tp_part->place))
			v_proofs.push_back({ tp_part->place, receiver->get_span() });
		if (const FieldAccessExpression *fa_field = dynamic_cast<const FieldAccessExpression *>(receiver))
			receiver = fa_field->receiver.get();
		else if (const IndexExpression *ie_index = dynamic_cast<const IndexExpression *>(receiver))
			receiver = ie_index->array.get();
		else
			break;
	}
	return v_proofs;
}

void BodyChecker::requireReceiverProofs (const ReceiverProofs &proofs) const {
	for (const ReceiverProof &rp_proof : std::views::reverse(proofs))
		if (!fs_flow.narrowed.contains(rp_proof.place))
			fail(Code::NULLABLE_PAYLOAD_ACCESS, rp_proof.span, "Receiver storage must remain present after evaluating the arguments");
}

void BodyChecker::setPresence (const StoragePlace &place, const ResolvedType &declared, const std::optional<ResolvedType> &value) {
	if (declared.nullable() && value && !analyzer.mayBeAbsent(*value)) fs_flow.narrowed.insert_or_assign(place, declared.element());
}

void BodyChecker::assumePresent (const Expression &source) {
	const std::optional<TypedPlace> tp_source = locate(source);
	if (tp_source && tp_source->type.nullable()) fs_flow.narrowed.try_emplace(tp_source->place, tp_source->type.element());
}

void BodyChecker::narrowRead (ExpressionInfo &value, const StoragePlace &place) const {
	const auto it_proof = fs_flow.narrowed.find(place);
	if (it_proof != fs_flow.narrowed.end()) value.types.front() = it_proof->second;
}

void BodyChecker::invalidate (const StoragePlace &place, bool descendantsOnly) {
	if (we_capture) (descendantsOnly ? we_capture->receivers : we_capture->places).insert(place);
	std::erase_if(fs_flow.narrowed, [&] (const Narrowing::value_type &entry) {
		const StoragePlace &sp_proof = entry.first;
		if (sp_proof.fact == PlaceFact::RetainedShape) return false;
		if (descendantsOnly && sp_proof.fields.empty()) return false;
		if (sp_proof.root == place.root && (!descendantsOnly || sp_proof.fields.size() > place.fields.size()) &&
		    sp_proof.fields.size() >= place.fields.size() && std::equal(place.fields.begin(), place.fields.end(), sp_proof.fields.begin()))
			return true;
		// A write through a second handle can name the same class field.
		// Struct fields on independent value bindings occupy different storage. A shared class field
		// can itself contain a value struct, so match the suffix starting at the last class field.
		const auto it_shared = std::find_if(place.fields.rbegin(), place.fields.rend(),
		                                    [&] (const Symbol *field) { return asClass(analyzer.declarations.at(field).owner) != nullptr; });
		const auto it_end    = sp_proof.fields.end() - (descendantsOnly ? 1 : 0);
		return it_shared != place.fields.rend() &&
		       std::search(sp_proof.fields.begin(), it_end, std::prev(it_shared.base()), place.fields.end()) != it_end;
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
	std::erase_if(fs_flow.narrowed, [&] (const Narrowing::value_type &entry) {
		return !entry.first.fields.empty() && (entry.first.root == &context.name || std::ranges::any_of(entry.first.fields, [&] (const Symbol *field) {
			                                       return asClass(analyzer.declarations.at(field).owner) != nullptr;
		                                       }));
	});
}

void BodyChecker::applyWrites (const Binding &binding) {
	if (!binding.effects) return;
	for (const StoragePlace &sp_place : binding.effects->places) invalidate(sp_place);
	for (const StoragePlace &sp_receiver : binding.effects->receivers) invalidate(sp_receiver, true);
	if (binding.effects->sharedFields) invalidateShared();
}

BodyChecker::TestPaths BodyChecker::splitTest (const Expression &node, const ExpressionInfo &result) {
	if (!result.fallsThrough) return {};
	if (e_test == &node) return tp_test;
	TestPaths tp_paths{ .yes = fs_flow.narrowed, .no = fs_flow.narrowed };
	if (const auto *bl_test = dynamic_cast<const BooleanLiteral *>(&node)) {
		(bl_test->value ? tp_paths.no : tp_paths.yes).reset();
	} else if (const auto *be_test = dynamic_cast<const BinaryExpression *>(&node); be_test && !result.argumentContract) {
		const Operator o_op = be_test->op.get_operator();
		if (o_op != oEqual && o_op != oNotEqual && o_op != oIdentity) return tp_paths;
		const Expression               *e_operand = dynamic_cast<const NullLiteral *>(be_test->left.get())  ? be_test->right.get() :
		                                            dynamic_cast<const NullLiteral *>(be_test->right.get()) ? be_test->left.get() :
		                                                                                                      nullptr;
		const std::optional<TypedPlace> tp_place  = e_operand ? locate(*e_operand) : std::nullopt;
		if (tp_place && tp_place->type.nullable())
			(o_op == oNotEqual ? *tp_paths.yes : *tp_paths.no).try_emplace(tp_place->place, tp_place->type.element());
	} else if (const auto *tc_test = dynamic_cast<const TypeCheckExpression *>(&node);
	           tc_test && result.symbol && result.symbol->kind == SymbolKind::Type) {
		const std::optional<TypedPlace> tp_place  = locate(*tc_test->operand);
		const ResolvedType              rt_target = bodyType(tc_test->target);
		if (tp_place && analyzer.referenceType(rt_target) &&
		    analyzer.referenceType(tp_place->type.nullable() ? tp_place->type.element() : tp_place->type)) {
			const auto          it_known   = fs_flow.narrowed.find(tp_place->place);
			const ResolvedType &rt_source  = it_known != fs_flow.narrowed.end() ? it_known->second : tp_place->type;
			const ResolvedType  rt_present = rt_source.nullable() ? rt_source.element() : rt_source;
			tp_paths.yes->insert_or_assign(tp_place->place, analyzer.accepts(rt_target, rt_present) ? rt_present : rt_target);
		}
	}
	return tp_paths;
}

BodyChecker::TestPaths BodyChecker::test (const Expression &node) {
	const ExpressionInfo ei_test = scalarInfo(node);
	if (ei_test.fallsThrough && ei_test.types.front() != ResolvedType(kBoolean))
		fail(Code::NON_BOOLEAN_CONDITION, node.get_span(), "A condition must have type " + std::string(to_string(kBoolean)));
	return splitTest(node, ei_test);
}

bool BodyChecker::loop (const LoopStatement &node) {
	const auto       *fs_for    = dynamic_cast<const ForStatement *>(&node);
	const auto       *ws_while  = dynamic_cast<const WhileStatement *>(&node);
	const auto       *ds_do     = dynamic_cast<const DoWhileStatement *>(&node);
	const LoopTarget *lt_target = node.get_loop_target().get();
	const LoopBody   &lb_body   = fs_for ? fs_for->body : ws_while ? ws_while->body : ds_do->body;
	bool              b_source  = true;
	if (fs_for) {
		std::optional<ResolvedType> annotation;
		if (fs_for->binding.type) {
			annotation = bodyType(*fs_for->binding.type);
			if (*annotation == ResolvedType(kVoid))
				fail(Code::INVALID_FOR_LOOP, fs_for->binding.get_span(), "An iteration binding requires a value type");
		}
		std::optional<ResolvedType> opt_element;
		const Expression           &source   = fs_for->source.get_expression();
		std::optional<ResolvedType> opt_hint = annotation;
		const bool                  b_range  = std::holds_alternative<ForSource::Range>(fs_for->source.value);
		if (annotation && b_range) {
			opt_hint = ResolvedType(kRange);
			analyzer.applyArguments(*opt_hint, { *annotation });
		} else if (opt_hint)
			opt_hint->suffixes.push_back(arraySuffix());
		std::optional<ResolvedType> opt_source;
		try {
			opt_source = scalar(source, opt_hint ? &*opt_hint : nullptr);
		} catch (const cde::ClothDiagnostic &error) {
			if (!b_range || error.get_code() != Code::INVALID_RANGE_VALUE) throw;
			fail(Code::INVALID_FOR_LOOP, source.get_span(), "A range loop requires endpoints of one numeric type");
		}
		if (!opt_source) {
			b_source = false;
		} else if (opt_source->array()) {
			opt_element = opt_source->element();
			if (record) bf_facts.iterations[fs_for] = { IterationKind::Array, *opt_element };
		} else if (opt_source->name == to_string(kRange) && !opt_source->declaration && !opt_source->arguments().empty()) {
			opt_element = opt_source->arguments().front();
			if (record) bf_facts.iterations[fs_for] = { IterationKind::Range, *opt_element };
		} else {
			const std::vector<Candidate> group = analyzer.members(*opt_source, to_string(oForeach), SymbolKind::Operator);
			if (group.empty()) fail(Code::INVALID_FOR_LOOP, source.get_span(), "Expected a range, array, or collection with an iteration operator");
			if (readOnlyDefaultRoot(source))
				fail(Code::INVALID_PARAMETER_DEFAULT, source.get_span(), "A default cannot iterate a read-only receiver", &pd_default->name);
			ExpressionInfo                  call     = invoke(group.front().symbol->name, source.get_span(), {}, group);
			std::optional<cloth::Iteration> snapshot = call.types.size() == 1 ? analyzer.snapshotLayout(call.types.front()) : std::nullopt;
			if (!snapshot) fail(Code::INVALID_FOR_LOOP, source.get_span(), "Iteration requires the standard library snapshot");
			mutatingReceiver(source, *opt_source);
			opt_element        = snapshot->element;
			snapshot->snapshot = std::move(call);
			if (record) bf_facts.iterations[fs_for] = std::move(*snapshot);
		}
		if (!opt_element && !annotation)
			fail(Code::INVALID_FOR_LOOP, fs_for->source.get_span(), "A noncontinuing loop source requires an element annotation");
		if (annotation && opt_element) {
			require({ *opt_element }, { *annotation }, fs_for->binding.get_span(), Code::INVALID_FOR_LOOP);
		}
		if (!annotation && opt_element) checkInferredBinding(*opt_element, fs_for->source.get_span());
		scopes.emplace_back();
		bind(fs_for->binding.name, annotation ? annotation : opt_element, true);
	}
	const Narrowing n_entry = fs_flow.narrowed;

	struct Iteration {
		std::optional<Narrowing> back, exit;
		Exits                    outward;
	};

	const std::function iteration = [&] (BodyChecker &body) {
		Iteration it_result;
		bool      b_body = b_source;
		if (ws_while) {
			const TestPaths tp_paths = body.test(*ws_while->condition);
			it_result.exit           = tp_paths.no;
			b_body                   = tp_paths.yes.has_value();
			if (b_body) body.fs_flow.narrowed = *tp_paths.yes;
		} else if (fs_for && b_source)
			it_result.exit = body.fs_flow.narrowed;
		WriteEffects *we_entry = body.we_capture;
		if (!b_body) body.we_capture = nullptr;
		const LoopTarget *lt_outer = std::exchange(body.lt_loop, lt_target);
		Exits             ex_test  = std::exchange(body.fs_flow.exits, {});
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
		if (b_trailer) body.fs_flow.narrowed = *it_result.back;
		if (ds_do) {
			const TestPaths tp_paths = body.test(*ds_do->condition);
			if (b_trailer) {
				it_result.back = tp_paths.yes;
				if (tp_paths.no) join(it_result.exit, *tp_paths.no);
			}
		} else if (fs_for && fs_for->stepper) {
			body.remember(fs_for->stepper->target, fs_for->binding.name);
			if (const Expression *e_update = fs_for->stepper->update()) {
				const Binding                    &b_index = *body.local(fs_for->binding.name.get_lexeme());
				const std::optional<ResolvedType> rt_step = body.scalar(*e_update, b_index.type ? &*b_index.type : nullptr);
				if (rt_step && b_index.type) body.require({ *rt_step }, { *b_index.type }, fs_for->stepper->span);
				if (body.record && rt_step && rt_step->floating())
					requireFinite(*fs_for->stepper->value, body.bf_facts.expressions, Code::INVALID_FOR_LOOP);
				if (!rt_step) it_result.back.reset();
			}
			if (it_result.back) it_result.back = body.fs_flow.narrowed;
		}
		if (b_trailer) mergeExits(it_result.outward, body.fs_flow.exits);
		body.we_capture = we_entry;
		return it_result;
	};
	// A loop header can only lose incoming proofs. Check its body with the stable header before committing overloads.
	Narrowing                n_header = n_entry;
	std::optional<Iteration> opt_stable;
	while (!n_header.empty()) {
		BodyChecker bc_trial     = *this;
		bc_trial.record          = false;
		bc_trial.we_capture      = nullptr;
		bc_trial.fs_flow         = { n_header, {} };
		const Iteration it_trial = iteration(bc_trial);
		Narrowing       n_next   = n_header;
		if (it_trial.back) intersect(n_next, *it_trial.back);
		if (n_next == n_header) {
			if (!record && !we_capture) opt_stable = it_trial;
			break;
		}
		n_header = std::move(n_next);
	}
	Exits ex_incoming         = std::exchange(fs_flow.exits, {});
	fs_flow.narrowed          = std::move(n_header);
	const Iteration it_result = opt_stable ? std::move(*opt_stable) : iteration(*this);
	fs_flow.exits             = std::move(ex_incoming);
	mergeExits(fs_flow.exits, it_result.outward);
	if (it_result.exit) fs_flow.narrowed = *it_result.exit;
	if (fs_for) leaveScope();
	return it_result.exit.has_value();
}

}
