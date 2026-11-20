// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <format>

#include <semantic/checker.hpp>

namespace cloth::semantic {
namespace {
std::vector<std::size_t> dimensions (const ResolvedType &type) {
	std::vector<std::size_t> result;
	for (std::size_t st_dimension = type.suffixes.size(); st_dimension-- > 0;)
		if (type.suffixes[st_dimension] != to_string(oQuestion)) result.push_back(st_dimension);
	return result;
}
}

bool hasArrayShape (const ResolvedType &type) {
	return type.array() || (type.nullable() && type.element().array());
}

ResolvedType comparisonShape (ResolvedType type, const ArrayLiteral *literal) {
	// A literal supplies its own extents. An empty level keeps the peer's declared inner shape.
	std::vector<const Expression *> v_rows;
	if (literal) v_rows.push_back(literal);
	bool b_forget = !literal;
	for (std::size_t st_suffix = type.suffixes.size(); st_suffix-- > 0;) {
		std::string &s_suffix = type.suffixes[st_suffix];
		if (s_suffix == to_string(oQuestion)) {
			std::erase_if(v_rows, [] (const Expression *row) { return dynamic_cast<const NullLiteral *>(row); });
			continue;
		}
		if (!b_forget && v_rows.empty()) break;
		s_suffix = arraySuffix();
		if (b_forget) continue;
		std::vector<const Expression *> v_elements;
		for (const Expression *e_row : v_rows) {
			const ArrayLiteral *al_row = dynamic_cast<const ArrayLiteral *>(e_row);
			if (!al_row) {
				b_forget = true;
				break;
			}
			for (const ExpressionPtr &ep_element : al_row->elements) v_elements.push_back(ep_element.get());
		}
		v_rows = std::move(v_elements);
	}
	return type;
}

ResolvedType refineShape (ResolvedType target, const ResolvedType &source, const TokenSpan *use) {
	if (!hasArrayShape(target) || !hasArrayShape(source)) return target;
	const std::vector<std::size_t> v_destination = dimensions(target), v_source = dimensions(source);
	if (v_destination.size() != v_source.size()) return target;
	const std::string s_unknown = arraySuffix();
	for (std::size_t st_dimension = 0; st_dimension < v_destination.size(); ++st_dimension) {
		std::string       &s_extent   = target.suffixes[v_destination[st_dimension]];
		const std::string &s_incoming = source.suffixes[v_source[st_dimension]];
		if (s_extent == s_unknown)
			s_extent = s_incoming;
		else if (use && s_incoming != s_unknown && s_extent != s_incoming)
			fail(Code::INVALID_ARRAY, *use, std::format("Array dimension {} requires {}, found {}", st_dimension + 1, s_extent, s_incoming));
	}
	return target;
}

ResolvedType commonShape (ResolvedType left, const ResolvedType &right) {
	const std::vector<std::size_t> a = dimensions(left), b = dimensions(right);
	for (std::size_t st_dimension = 0; st_dimension < a.size(); ++st_dimension)
		if (st_dimension >= b.size() || left.suffixes[a[st_dimension]] != right.suffixes[b[st_dimension]])
			left.suffixes[a[st_dimension]] = arraySuffix();
	return left;
}

ResolvedType BodyChecker::shapeOf (const Expression &value, const ResolvedType &fallback, std::size_t slot) const {
	if (record) {
		const decltype(bf_facts.expressions)::const_iterator it_shape = bf_facts.expressions.find(&value);
		if (it_shape != bf_facts.expressions.end() && slot < it_shape->second.arrayShapes.size()) return it_shape->second.arrayShapes[slot];
	}
	return fallback;
}

void BodyChecker::receiveShape (const Token &binding, const ResolvedType &declared, const ResolvedType &shape, const TokenSpan &use) {
	if (!hasArrayShape(declared)) return;
	const StoragePlace              sp_retained{ &binding, {}, PlaceFact::RetainedShape }, sp_current{ &binding, {}, PlaceFact::CurrentShape };
	const Narrowing::const_iterator it_shape = fs_flow.narrowed.find(sp_retained);
	const ResolvedType rt_current  = refineShape(it_shape == fs_flow.narrowed.end() ? declared : it_shape->second.type, shape, record ? &use : nullptr);
	ResolvedType       rt_retained = rt_current;
	const std::size_t  st_outer    = dimensions(declared).front();
	rt_retained.suffixes[st_outer] = declared.suffixes[st_outer];
	fs_flow.narrowed.insert_or_assign(sp_retained, FlowFact{ rt_retained });
	fs_flow.narrowed.insert_or_assign(sp_current, FlowFact{ rt_current });
	// A dynamic inner extent is captured at initialization and compared on subsequent stores at runtime.
	if (record) bf_facts.arrayConstraints.try_emplace(&binding, rt_retained);
}

void BodyChecker::rowShape (const Expression &target, const Expression &value, const ResolvedType &incoming) {
	const Expression *e_root   = &target;
	std::size_t       st_depth = 0;
	while (const IndexExpression *index = dynamic_cast<const IndexExpression *>(e_root)) {
		++st_depth;
		e_root = index->array.get();
	}
	const NameExpression *name    = dynamic_cast<const NameExpression *>(e_root);
	const Binding        *binding = name ? local(name->name.get_lexeme()) : nullptr;
	if (!st_depth || !binding || !binding->type || !hasArrayShape(incoming)) return;
	const std::vector<std::size_t> v_axes = dimensions(*binding->type);
	if (st_depth >= v_axes.size()) return;
	const StoragePlace              sp_key     = StoragePlace{ binding->name, {}, PlaceFact::CurrentShape };
	const Narrowing::const_iterator it_current = fs_flow.narrowed.find(sp_key);
	ResolvedType                    shape      = it_current == fs_flow.narrowed.end() ? *binding->type : it_current->second.type;
	const std::vector<std::size_t>  v_rowAxes  = dimensions(incoming);
	if (st_depth + v_rowAxes.size() != v_axes.size()) return;
	for (std::size_t st_dimension = 0; st_dimension < v_rowAxes.size(); ++st_dimension)
		shape.suffixes[v_axes[st_depth + st_dimension]] = incoming.suffixes[v_rowAxes[st_dimension]];
	receiveShape(*binding->name, *binding->type, shape, value.get_span());
}

std::optional<ArrayMutation> arrayMutationNamed (std::string_view name) {
	static constexpr std::pair<std::string_view, ArrayMutation> V_MUTATIONS[] = { { "append", ArrayMutation::Append },
		                                                                      { "insert", ArrayMutation::Insert },
		                                                                      { "removeAt", ArrayMutation::RemoveAt },
		                                                                      { "clear", ArrayMutation::Clear },
		                                                                      { "reserve", ArrayMutation::Reserve } };
	for (const std::pair<std::string_view, ArrayMutation> &entry : V_MUTATIONS)
		if (name == entry.first) return entry.second;
	return {};
}

ExpressionInfo BodyChecker::mutateArray (const MemberCallExpression &call, const ExpressionInfo &receiver, ArrayMutation operation) {
	const ResolvedType &rt_array   = receiver.types.front();
	const ResolvedType  rt_element = rt_array.element();
	const bool          b_remove = operation == ArrayMutation::RemoveAt, b_clear = operation == ArrayMutation::Clear,
	           b_reserve       = operation == ArrayMutation::Reserve;
	const bool        b_insert = operation == ArrayMutation::Insert, b_add = operation == ArrayMutation::Append || b_insert;
	const std::size_t st_arity = b_clear ? 0u : b_insert ? 2u : 1u;
	checkBuiltinArguments(call, call.typeArguments ? &*call.typeArguments : nullptr, st_arity, st_arity);
	if (rt_array.suffixes.back() != arraySuffix())
		fail(Code::INVALID_ARRAY_MUTATION, call.name.get_span(), "Changing array storage requires a growable array");
	if (!b_reserve && dynamic_cast<const IndexExpression *>(call.receiver.get()))
		fail(Code::INVALID_ARRAY_MUTATION, call.receiver->get_span(), "An embedded row must retain its rectangular shape");
	checkFieldWrite(receiver, call.receiver->get_span());
	if (locate(*call.receiver)) writable(*call.receiver);
	const ReceiverProofs       v_proofs    = receiverProofs(call.suppliedArguments.empty() ? nullptr : call.receiver.get());
	ResolvedType               rt_shape    = shapeOf(*call.receiver, rt_array);
	bool                       b_continues = receiver.fallsThrough;
	std::optional<BodyChecker> opt_unreachable;
	for (std::size_t st_index = 0; st_index < call.suppliedArguments.size(); ++st_index) {
		const CallArgument               &ca_arg       = call.suppliedArguments[st_index];
		const bool                        b_element    = b_add && st_index + 1 == call.suppliedArguments.size();
		const ResolvedType                rt_parameter = b_element ? rt_element : ResolvedType(kUsize);
		BodyChecker                      &bc_operand   = onPath(b_continues, opt_unreachable);
		const std::optional<ResolvedType> opt_actual   = bc_operand.scalar(*ca_arg.expression, &rt_parameter);
		if (opt_actual) {
			if (!analyzer.accepts(rt_parameter, *opt_actual))
				fail(Code::INVALID_BUILTIN_ARGUMENT, ca_arg.get_span(),
				     std::format("'{}' requires {}, got {}", call.name.get_lexeme(), rt_parameter.spelling(), opt_actual->spelling()));
			if (b_element && hasArrayShape(rt_element)) {
				const TokenSpan   ts_argument = ca_arg.get_span();
				const std::string s_outer     = rt_shape.suffixes.back();
				rt_shape = refineShape(rt_shape.element(), bc_operand.shapeOf(*ca_arg.expression, *opt_actual), &ts_argument);
				rt_shape.suffixes.push_back(s_outer);
			}
		}
		b_continues &= opt_actual.has_value();
	}
	ExpressionInfo ei_result{ .types = b_remove ? ResultTypes{ rt_element } : ResultTypes{}, .fallsThrough = b_continues, .arrayMutation = operation };
	if (!b_continues) return ei_result;
	requireReceiverProofs(v_proofs);
	if (b_remove && hasArrayShape(rt_element)) ei_result.arrayShapes = { rt_shape.element() };
	if (!b_reserve) {
		rt_shape.suffixes.back()                  = b_clear ? arraySuffix("0") : arraySuffix();
		const std::optional<TypedPlace> opt_place = locate(*call.receiver);
		if (opt_place) {
			invalidate(opt_place->place, true);
			if (opt_place->place.path.empty()) {
				if (we_capture) we_capture->places.insert({ opt_place->place.root, {}, PlaceFact::CurrentShape });
				receiveShape(*opt_place->place.root, rt_array, rt_shape, call.get_span());
			}
		}
		if (!opt_place || sharedPlace(opt_place->place)) forgetShared(true);
	}
	return ei_result;
}

}
