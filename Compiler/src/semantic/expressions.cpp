// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <format>

#include <ast/decl/enum_declaration.hpp>
#include <ast/expr/member_call_expression.hpp>
#include <ast/expr/provider_expressions.hpp>
#include <semantic/checker.hpp>
#include <token/token.hpp>

namespace cloth::semantic {
namespace {

ExpressionInfo value (ResolvedType type, const Symbol *symbol = nullptr, ReceiverFieldPath fieldPath = ReceiverFieldPath::None) {
	return { .types = { std::move(type) }, .symbol = symbol, .receiverFieldPath = fieldPath };
}

ResolvedType literalInteger (const IntegerLiteral &literal, const ResolvedType *expected, bool negative, TargetLayout layout, bool inferenceDefault) {
	const ResolvedType rt = expected && expected->integer() ? *expected : ResolvedType(kInteger);
	if (inferenceDefault) return rt;
	const bool          isUnsigned = rt.name == unsignedIntegerSpelling() || rt.name == to_string(kUsize);
	const unsigned      width      = rt.width ? rt.width : layout.sizeBits;
	const IntegerValue &words      = literal.value;
	const std::size_t   bits       = (words.size() - 1) * 32 + std::bit_width(words.back());
	const bool          minimum    = negative && !isUnsigned && bits == width && std::has_single_bit(words.back()) &&
	                     std::all_of(words.begin(), words.end() - 1, [] (std::uint32_t word) { return word == 0; });
	if ((negative && isUnsigned && bits) || (bits > width - (isUnsigned ? 0 : 1) && !minimum)) {
		fail(Code::INTEGER_RANGE, literal.get_span(), "Integer literal is outside " + rt.spelling());
	}
	return rt;
}

bool numericLiteral (const Expression &expr) {
	if (dynamic_cast<const NumberLiteral *>(&expr)) {
		return true;
	}
	const auto *u = dynamic_cast<const UnaryExpression *>(&expr);
	return u && (u->op.get_operator() == oPlus || u->op.get_operator() == oMinus) && numericLiteral(*u->operand);
}

bool suppliesFloatingContext (const ResolvedType &type) {
	return !type.declaration && !type.parameter &&
	       (type.name == to_string(kFloat) ||
	        (type.name == to_string(kRange) && !type.arguments().empty() && suppliesFloatingContext(type.arguments().front())));
}

bool arithmetic (Operator op) {
	return op == oPlus || op == oMinus || op == oMul || op == oDiv || op == oMod || op == oMulMul;
}

bool hasEqualityHint (const Expression &node) {
	return dynamic_cast<const NameExpression *>(&node) || dynamic_cast<const CallSite *>(&node) || dynamic_cast<const CastExpression *>(&node) ||
	       dynamic_cast<const IndexExpression *>(&node) || dynamic_cast<const FieldAccessExpression *>(&node) ||
	       dynamic_cast<const ProviderAccessExpression *>(&node) || dynamic_cast<const ElvisExpression *>(&node) ||
	       dynamic_cast<const GuardedAccessExpression *>(&node);
}

bool takesNumericContext (const Expression &operand) {
	if (const UnaryExpression *ue_value = dynamic_cast<const UnaryExpression *>(&operand))
		return ue_value->op.get_operator() != oNot && takesNumericContext(*ue_value->operand);
	if (const BinaryExpression *be_value = dynamic_cast<const BinaryExpression *>(&operand))
		return arithmetic(be_value->op.get_operator()) && (takesNumericContext(*be_value->left) || takesNumericContext(*be_value->right));
	return dynamic_cast<const NumberLiteral *>(&operand) || dynamic_cast<const IfExpression *>(&operand) || dynamic_cast<const ElvisExpression *>(&operand);
}

void mergeElementHint (std::optional<ResolvedType> &common, const std::optional<ResolvedType> &next, const TokenSpan &use, Code code) {
	if (!next) return;
	if (common && *common != *next) {
		const ResolvedType rt_merged = refineShape(*common, *next);
		if (rt_merged != refineShape(*next, *common))
			fail(code, use, std::format("Incompatible element types {} and {}", common->spelling(), next->spelling()));
		common = rt_merged;
	} else
		common = next;
}
}

const ResolvedType *rangeEndpoint (const ResolvedType &type) {
	if (type.name != to_string(kRange) || type.declaration || type.arguments().size() != 1) return nullptr;
	if (!type.suffixes.empty() && !(type.suffixes.size() == 1 && type.nullable())) return nullptr;
	return &type.arguments().front();
}

ElementEvidence elementEvidence (const Expression &operand) {
	if (const ArrayLiteral *al_literal = dynamic_cast<const ArrayLiteral *>(&operand)) {
		ElementEvidence ee_best = ElementEvidence::None;
		for (const ExpressionPtr &ep_element : al_literal->elements) ee_best = std::max(ee_best, elementEvidence(*ep_element));
		return ee_best;
	}
	if (dynamic_cast<const NullLiteral *>(&operand) || dynamic_cast<const StructLiteral *>(&operand)) return ElementEvidence::None;
	if (const UnaryExpression *ue_unary = dynamic_cast<const UnaryExpression *>(&operand)) {
		if (ue_unary->op.get_operator() == oPlus || ue_unary->op.get_operator() == oMinus) return elementEvidence(*ue_unary->operand);
	}
	if (const BinaryExpression *be_binary = dynamic_cast<const BinaryExpression *>(&operand)) {
		const Operator o_operation = be_binary->op.get_operator();
		if (arithmetic(o_operation) || o_operation == oRange || o_operation == oRangeInclusive)
			return std::max(elementEvidence(*be_binary->left), elementEvidence(*be_binary->right));
	}
	if (const IfExpression *ie_conditional = dynamic_cast<const IfExpression *>(&operand)) {
		const std::function<ElementEvidence(const std::vector<StatementPtr> &)> tail = [] (const std::vector<StatementPtr> &body) {
			const ExpressionStatement *es_last = body.empty() ? nullptr : dynamic_cast<const ExpressionStatement *>(body.back().get());
			return es_last ? elementEvidence(*es_last->expression) : ElementEvidence::None;
		};
		ElementEvidence ee_result = ie_conditional->otherwise ? tail(*ie_conditional->otherwise) : ElementEvidence::None;
		for (const ConditionalBranch &cb_branch : ie_conditional->branches) ee_result = std::max(ee_result, tail(cb_branch.statements));
		return ee_result;
	}
	return numericLiteral(operand) ? ElementEvidence::Literal : ElementEvidence::Typed;
}

ExpressionInfo BodyChecker::expression (const Expression &node, const ResultTypes *expected, bool discard, std::optional<FloatContext> floats,
                                        const ResolvedType *broadcast) {
	for (const CapturedOperand *operand = captured; operand; operand = operand->previous)
		if (operand->node == &node) {
			if (ef_probeConstants) ef_probeConstants->insert_or_assign(&node, operand->value);
			return operand->value;
		}
	if (!floats)
		floats = FloatContext{ (expected && std::ranges::any_of(*expected, suppliesFloatingContext)) ||
			               (broadcast && suppliesFloatingContext(*broadcast)) };
	ResultTypes         v_scalar;
	const IfExpression *ie_branch = dynamic_cast<const IfExpression *>(&node);
	if (broadcast && !ie_branch && !dynamic_cast<const ResultListExpression *>(&node)) {
		v_scalar.push_back(*broadcast);
		expected = &v_scalar;
	}
	ExpressionInfo info = [&] () -> ExpressionInfo {
		if (ie_branch) return condition(*ie_branch, expected, discard, *floats, broadcast);
		if (const BinaryExpression *be_operation = dynamic_cast<const BinaryExpression *>(&node)) {
			const ResolvedType *rt_hint = expected && expected->size() == 1 ? &expected->front() : nullptr;
			if (rt_hint && rt_hint->nullable()) {
				const ResolvedType rt_payload = rt_hint->element();
				return binary(*be_operation, &rt_payload, *floats);
			}
			return binary(*be_operation, rt_hint, *floats);
		}
		try {
			return infer(node, expected, discard, *floats);
		} catch (const cde::ClothDiagnostic &error) {
			if (dynamic_cast<const CallSite *>(&node) || dynamic_cast<const GuardedAccessExpression *>(&node) ||
			    dynamic_cast<const FieldAccessExpression *>(&node))
				throw IntrinsicDiagnostic(error);
			throw;
		}
	}();
	for (ResolvedType &rt_type : info.types)
		if (rt_type.parameter) rt_type = analyzer.boundShape(rt_type);
	if (validateAfter == &node && info.fallsThrough) invalidateShared();
	ExpressionFacts *ef_numeric = record ? &bf_facts.expressions : ef_probeConstants;
	if (ef_numeric) checkNumeric(node, info, *ef_numeric, analyzer.program.layout);
	if (record) {
		if (expected && info.fallsThrough && expected->size() == info.types.size())
			for (std::size_t i = 0; i < expected->size(); ++i) {
				if (!hasArrayShape((*expected)[i])) continue;
				const TokenSpan span = node.get_span();
				refineShape((*expected)[i], info.arrayShapes.empty() ? info.types[i] : info.arrayShapes[i], &span);
			}
		ExpressionInfo &ei_fact = bf_facts.expressions.insert_or_assign(&node, info).first->second;
		// A guarded chain can still need the declared type on its absent path. The exiting expression itself produces no values.
		if (!ei_fact.fallsThrough) ei_fact.types.clear();
	} else if (ef_probeConstants) {
		ef_probeConstants->insert_or_assign(&node, info);
	}
	return info;
}

std::optional<ResolvedType> BodyChecker::scalar (const Expression &node, const ResolvedType *expected, std::optional<FloatContext> floats) {
	ExpressionInfo ei_value = scalarInfo(node, expected, floats);
	if (!ei_value.fallsThrough) return std::nullopt;
	return std::move(ei_value.types.front());
}

ExpressionInfo BodyChecker::scalarInfo (const Expression &node, const ResolvedType *expected, std::optional<FloatContext> floats) {
	ResultTypes types;
	if (expected) {
		types.push_back(*expected);
	}
	const ExpressionInfo result = expression(node, expected ? &types : nullptr, false, floats);
	if (result.resultKind() == ResultKind::Empty || result.resultKind() == ResultKind::Multiple) {
		fail(result.types.empty() ? Code::VOID_VALUE : Code::MULTIPLE_RESULTS_IN_SCALAR, node.get_span(), "Expected one value");
	}
	return result;
}

ResolvedType BodyChecker::recordSelf (const Expression &node) {
	const ResolvedType rt_self = selfType();
	if (record) {
		bf_facts.expressions.insert_or_assign(&node, value(rt_self));
	}
	return rt_self;
}

ExpressionInfo BodyChecker::name (const Token &token, bool assignment) {
	const std::string &spelling = token.get_lexeme();
	if (const Binding *binding = local(spelling)) {
		remember(token, *binding->name);
		if (binding->deferred) forgetMutableMetadata(fs_flow.narrowed);
		if (!binding->type) return { .types = {}, .binding = binding->name, .fallsThrough = false };
		applyWrites(*binding);
		if (binding->deferred && *binding->type == ResolvedType(kVoid)) return { .types = {}, .binding = binding->name };
		ExpressionInfo ei_read{ { *binding->type }, nullptr, binding->name };
		narrowRead(ei_read, { binding->name });
		const Narrowing::const_iterator it_metadata = fs_flow.narrowed.find({ binding->name, {}, PlaceFact::RepresentedType });
		// Flow-derived metadata is checked on the selected path after overload resolution.
		if (record && it_metadata != fs_flow.narrowed.end()) ei_read.representedType = it_metadata->second.type;
		const Narrowing::const_iterator current = fs_flow.narrowed.find({ binding->name, {}, PlaceFact::CurrentShape });
		if (current != fs_flow.narrowed.end()) ei_read.arrayShapes = { refineShape(ei_read.types.front(), current->second.type) };
		return ei_read;
	}
	checkDefaultName(token);
	if (spelling == to_string(kSelf)) {
		if (!owner) {
			fail(Code::RECEIVER_OUTSIDE_METHOD, token.get_span(), std::format("{} requires a member body", to_string(kSelf)));
		}
		if (!hasInstance()) {
			fail(Code::PARTIAL_CLASS_INSTANCE, token.get_span(),
			     std::format("'{}' is unavailable before instance initialization", to_string(kSelf)));
		}
		if (rp_receiverPhase == ReceiverPhase::FieldInitializer) {
			fail(Code::PARTIAL_CLASS_INSTANCE, token.get_span(),
			     std::format("'{}' cannot be used as a value during field initialization", to_string(kSelf)));
		}
		return value(selfType());
	}
	if (spelling == to_string(kState) && owner && owner->kind == SymbolKind::State) {
		const ResolvedType rt_payload = analyzer.substitute(*analyzer.declarations.at(owner).value, m_arguments);
		if (rt_payload == ResolvedType(kVoid)) fail(Code::UNKNOWN_NAME, token.get_span(), "This state has no payload to read", &owner->name);
		remember(token, owner->name);
		return value(readType({ &owner->name }, rt_payload), owner);
	}
	const bool b_builtinName = builtinFunctionNamed(spelling);
	if (hasInstance() && !b_builtinName) {
		const std::vector<Candidate> fields = analyzer.members(selfType(), spelling, SymbolKind::Field);
		if (!fields.empty()) {
			const Candidate &field = fields.front();
			analyzer.requireAccess(*field.symbol, context, token.get_span());
			checkFieldRead(*field.symbol, token);
			remember(token, field.symbol->name);
			ExpressionInfo ei_field = value(analyzer.substitute(*analyzer.declarations.at(field.symbol).value, field.arguments), field.symbol,
			                                ReceiverFieldPath::Stored);
			narrowRead(ei_field, { &context.name, { field.symbol } });
			return ei_field;
		}
	}
	for (const Symbol *sym : b_builtinName ? analyzer.lookupFile(context, spelling) : analyzer.lookup(context, spelling)) {
		if (sym->kind == SymbolKind::Function) {
			if (assignment) fail(Code::EXPECTED_BINDING, token.get_span(), "Expected a writable binding", &sym->name);
			fail(Code::FUNCTION_VALUE, token.get_span(), "'" + spelling + "' is a function", &sym->name);
		}
		if (!b_builtinName && sym->kind == SymbolKind::Type) {
			fail(Code::TYPE_RECEIVER_VALUE, token.get_span(), "'" + spelling + "' is a type", &sym->name);
		}
	}
	if (b_builtinName) {
		if (assignment) fail(Code::EXPECTED_BINDING, token.get_span(), "Expected a writable binding");
		fail(Code::FUNCTION_VALUE, token.get_span(), "'" + spelling + "' is a function");
	}
	fail(Code::UNKNOWN_NAME, token.get_span(), "Unknown name '" + spelling + "'");
}

bool BodyChecker::isSelfReference (const Expression &node) {
	const NameExpression *ne_name = dynamic_cast<const NameExpression *>(&node);
	return ne_name && ne_name->name.get_lexeme() == to_string(kSelf);
}

bool BodyChecker::hasValueName (const Token &name) const {
	return local(name.get_lexeme()) || (hasInstance() && !analyzer.members(selfType(), name.get_lexeme(), SymbolKind::Field).empty());
}

std::optional<ResolvedType> BodyChecker::receiverType (const Expression &node, bool forConstruction) {
	std::optional<ResolvedType> opt_type;
	if (const TypeReceiverExpression *tr_receiver = dynamic_cast<const TypeReceiverExpression *>(&node)) {
		try {
			opt_type = bodyType(tr_receiver->annotation, forConstruction);
		} catch (const cde::ClothDiagnostic &error) {
			throw IntrinsicDiagnostic(error);
		}
	} else if (const std::optional<Token> opt_path = typeNameCandidate(node, [&] (const Token &root) {
		           const bool b_qualified = dynamic_cast<const FieldAccessExpression *>(&node) != nullptr;
		           return (forConstruction || !hasValueName(root)) && analyzer.hasTypeName(context, root, b_qualified);
	           })) {
		try {
			opt_type = bodyType(TypeReference(*opt_path), forConstruction);
		} catch (const cde::ClothDiagnostic &error) {
			if (error.get_code() != Code::UNSUPPORTED_TYPE) {
				throw;
			}
		}
	}
	if (opt_type && record) {
		ExpressionInfo ei_type  = value(ResolvedType(kTypeinfo), opt_type->declaration);
		ei_type.representedType = *opt_type;
		bf_facts.expressions.insert_or_assign(&node, std::move(ei_type));
	}
	return opt_type;
}

std::optional<BodyChecker::Receiver> BodyChecker::receiver (const Expression &node) {
	if (std::optional<ResolvedType> opt_type = receiverType(node)) return Receiver{ .type = std::move(*opt_type), .isType = true };
	ExpressionInfo ei_value = scalarInfo(node);
	if (ei_value.types.size() != 1) return std::nullopt;
	return Receiver{ .type         = std::move(ei_value.types.front()),
		         .fieldPath    = ei_value.receiverFieldPath,
		         .fallsThrough = ei_value.fallsThrough,
		         .represented  = ei_value.representedType };
}

ExpressionInfo BodyChecker::field (const FieldAccessExpression &node) {
	const bool                    b_self       = isSelfReference(*node.receiver);
	const std::optional<Receiver> opt_receiver = b_self && rp_receiverPhase == ReceiverPhase::FieldInitializer ? Receiver{ recordSelf(*node.receiver) } :
	                                                                                                             receiver(*node.receiver);
	if (!opt_receiver) return ExpressionInfo::never();
	const Receiver     &r_receiver = *opt_receiver;
	const ResolvedType &rt         = r_receiver.type;
	if (r_receiver.isType && !enumeration(rt)) {
		if (dynamic_cast<const TypeReceiverExpression *>(node.receiver.get()))
			fail(Code::TYPE_RECEIVER_VALUE, node.receiver->get_span(), "'" + rt.spelling() + "' is a type");
		fail(Code::UNKNOWN_NAME, node.receiver->get_span(), "No value named '" + rt.spelling() + "' is in scope");
	}
	if (rt.nullable() || rt == ResolvedType(kNull)) {
		fail(Code::NULLABLE_PAYLOAD_ACCESS, node.receiver->get_span(), std::format("Use {} to access a nullable receiver", to_string(oSafeDot)));
	}
	const std::optional<ResolvedType> opt_bound = r_receiver.isType ? std::nullopt : receiverClassBound(*node.receiver, rt);
	const std::vector<Candidate>      fields =
	        analyzer.members(opt_bound ? *opt_bound : rt, node.name.get_lexeme(), r_receiver.isType ? SymbolKind::EnumCase : SymbolKind::Field);
	if (fields.empty()) {
		Code dc_code = Code::INVALID_STRUCT_FIELD;
		if (enumeration(rt))
			dc_code = Code::INVALID_ENUM_MEMBER;
		else if (!analyzer.applicationsOf(rt, objectRoot()).empty())
			dc_code = Code::INVALID_CLASS_MEMBER;
		else if (analyzer.referenceType(rt))
			dc_code = Code::UNKNOWN_MEMBER;
		fail(dc_code, node.name.get_span(), "Unknown member '" + node.name.get_lexeme() + "' on " + rt.spelling());
	}
	const Candidate &member = fields.front();
	analyzer.requireAccess(*member.symbol, context, node.name.get_span());
	ReceiverFieldPath rfp_path = r_receiver.fieldPath;
	if (b_self) {
		checkFieldRead(*member.symbol, node.name);
		rfp_path = ReceiverFieldPath::Stored;
	} else if (rfp_path != ReceiverFieldPath::None && !std::holds_alternative<const StructFieldDeclaration *>(member.symbol->declaration)) {
		rfp_path = ReceiverFieldPath::Indirect;
	}
	remember(node.name, member.symbol->name);
	ExpressionInfo ei_field = value(analyzer.substitute(*analyzer.declarations.at(member.symbol).value, member.arguments), member.symbol, rfp_path);
	ei_field.fallsThrough   = r_receiver.fallsThrough;
	if (const std::optional<TypedPlace> tp_place = locate(node)) narrowRead(ei_field, tp_place->place);
	return ei_field;
}

std::optional<Candidate> BodyChecker::stateTarget (const ResolvedType &receiver, const Token &name, const Expression &operand) {
	const bool               b_attachedSelf = rt_attachment && isSelfReference(operand);
	const ResolvedType       host           = b_attachedSelf ? *rt_attachment : receiver.nullable() ? receiver.element() : receiver;
	std::optional<Candidate> opt_state;
	if (host.suffixes.empty() && asClass(host.declaration)) {
		const TypeDispatch                   &td_table = analyzer.dispatchFor(host);
		const StateSelections::const_iterator it_state = td_table.states.find(name.get_lexeme());
		if (it_state != td_table.states.end()) opt_state = Candidate{ it_state->second.selection, analyzer.argumentsOf(it_state->second.owner) };
	} else if (host.parameter) {
		for (SymbolKind kind : { SymbolKind::State, SymbolKind::StateAttachment }) {
			const std::vector<Candidate> v_states = analyzer.members(host, name.get_lexeme(), kind);
			if (!v_states.empty()) {
				opt_state = v_states.front();
				break;
			}
		}
	}
	if (opt_state) {
		if (receiver.nullable()) fail(Code::NULLABLE_PAYLOAD_ACCESS, name.get_span(), "A state operation requires a non-null receiver");
		if (!b_attachedSelf || analyzer.declarations.at(opt_state->symbol).attachedState != owner)
			analyzer.requireAccess(*opt_state->symbol, context, name.get_span());
		remember(name, opt_state->symbol->name);
	}
	if (!opt_state && (b_attachedSelf || analyzer.hasStateName(context, name.get_lexeme()))) {
		fail(Code::INVALID_STATE_TARGET, name.get_span(), host.spelling() + " does not support state '" + name.get_lexeme() + "'");
	}
	return opt_state;
}

bool BodyChecker::statePayload (const ExpressionPtr &payload, const ResolvedType &required, const TokenSpan &span, const Token &state) {
	if (required == ResolvedType(kVoid)) {
		if (payload) fail(Code::INVALID_STATE_PAYLOAD, payload->get_span(), "This state takes no payload", &state);
		return true;
	}
	if (!payload) fail(Code::INVALID_STATE_PAYLOAD, span, "Missing state payload of type " + required.spelling(), &state);
	const std::optional<ResolvedType> opt_actual = scalar(*payload, &required);
	if (opt_actual && !analyzer.accepts(required, *opt_actual)) {
		fail(Code::INVALID_STATE_PAYLOAD, payload->get_span(),
		     std::format("State '{}' expects payload {}, received {}", state.get_lexeme(), required.spelling(), opt_actual->spelling()), &state);
	}
	return opt_actual.has_value();
}

ExpressionInfo BodyChecker::transition (const Candidate &state, const ExpressionPtr &payload, const TokenSpan &span) {
	const ResolvedType required = analyzer.substitute(*analyzer.declarations.at(state.symbol).value, state.arguments);
	const bool         b_normal = statePayload(payload, required, span, state.symbol->name);
	invalidateShared();
	return { .types = {}, .symbol = state.symbol, .fallsThrough = b_normal };
}

ExpressionInfo BodyChecker::typeCheck (const TypeCheckExpression &node) {
	const Token  *identifier = node.target.get_bare_identifier();
	const bool                  b_unboundName = identifier && !analyzer.hasTypeName(context, *identifier);
	ExpressionPtr valueTarget;
	std::optional<ResolvedType> opt_type;
	if (node.valueCandidate) {
		opt_type = receiverType(*node.valueCandidate);
		if (!opt_type) valueTarget = node.valueCandidate;
	} else if (b_unboundName && hasValueName(*identifier)) {
		valueTarget = std::make_shared<NameExpression>(*identifier);
	}
	if (valueTarget) {
		std::shared_ptr<const BinaryExpression> test = std::make_shared<BinaryExpression>(node.operand, node.op, valueTarget);
		if (record) {
			test = bf_facts.valueTests.try_emplace(&node, test).first->second;
		}
		return expression(*test);
	}
	if (b_unboundName && rp_receiverPhase == ReceiverPhase::FieldInitializer && isSelfReference(*node.operand)) {
		if (const std::optional<Candidate> state = stateTarget(selfType(), *identifier, *node.operand)) {
			recordSelf(*node.operand);
			return value(ResolvedType(kBoolean), state->symbol);
		}
	}
	const std::optional<ResolvedType> opt_source = scalar(*node.operand);
	if (b_unboundName && opt_source) {
		if (const std::optional<Candidate> state = stateTarget(*opt_source, *identifier, *node.operand)) {
			return value(ResolvedType(kBoolean), state->symbol);
		}
	}
	if (opt_source && opt_source->name == to_string(kNull)) {
		fail(Code::EXPLICIT_NULLABLE_TYPE, node.operand->get_span(), "A type test needs a typed operand");
	}
	const ResolvedType target = opt_type ? *opt_type : bodyType(node.target);
	if (opt_source) {
		if (const std::optional<Candidate> opt_operator = selectContext(node.op, *opt_source, target, true))
			return callOperator(*opt_operator, *node.operand, *opt_source, node.op, node.get_span());
	}
	ExpressionInfo ei_test = opt_source ? value(ResolvedType(kBoolean), target.declaration) : ExpressionInfo::never();
	ei_test.testedType     = target;
	return ei_test;
}

std::optional<ResolvedType> BodyChecker::elementHint (const Expression &node, FloatContext floats) {
	if (dynamic_cast<const IntegerLiteral *>(&node) || dynamic_cast<const StructLiteral *>(&node)) return {};
	if (dynamic_cast<const FloatingPointLiteral *>(&node)) return floats.contextual ? std::nullopt : std::optional{ ResolvedType(kFloat) };
	if (const ArrayLiteral *al_array = dynamic_cast<const ArrayLiteral *>(&node)) {
		std::optional<ResolvedType> opt_element;
		for (const ExpressionPtr &ep_part : al_array->elements)
			mergeElementHint(opt_element, elementHint(*ep_part, floats), ep_part->get_span(), Code::INVALID_ARRAY);
		if (opt_element) opt_element->suffixes.push_back(arraySuffix(std::to_string(al_array->elements.size())));
		return opt_element;
	}
	if (const IfExpression *ie_conditional = dynamic_cast<const IfExpression *>(&node)) {
		std::optional<ResolvedType> opt_common;
		const std::function         branch = [&] (const std::vector<StatementPtr> &statements, const Narrowing &facts) {
                        BodyChecker bc_branch      = *this;
                        bc_branch.fs_flow.narrowed = facts;
                        bc_branch.scopes.emplace_back();
                        for (const StatementPtr &sp_item : statements) {
                                const ExpressionStatement *es_tail = dynamic_cast<const ExpressionStatement *>(sp_item.get());
                                if (&sp_item == &statements.back() && es_tail)
                                        mergeElementHint(opt_common, bc_branch.elementHint(*es_tail->expression, floats), es_tail->get_span(),
					                         Code::BRANCH_TYPE_MISMATCH);
                                else if (!bc_branch.statement(*sp_item))
                                        break;
                        }
		};
		BodyChecker bc_test = *this;
		for (const ConditionalBranch &cb_branch : ie_conditional->branches) {
			const TestPaths tp_paths = bc_test.test(*cb_branch.condition);
			branch(cb_branch.statements, tp_paths.yes.value_or(bc_test.fs_flow.narrowed));
			bc_test.fs_flow.narrowed = tp_paths.no.value_or(bc_test.fs_flow.narrowed);
		}
		if (ie_conditional->otherwise) branch(*ie_conditional->otherwise, bc_test.fs_flow.narrowed);
		return opt_common;
	}
	std::optional<ResolvedType> opt_hint;
	if (const BinaryExpression *be_range = dynamic_cast<const BinaryExpression *>(&node);
	    be_range && (be_range->op.get_operator() == oRange || be_range->op.get_operator() == oRangeInclusive)) {
		if (const std::optional<ExpressionInfo> opt_call = binaryOperator(*be_range, nullptr, true, {}))
			return opt_call->fallsThrough ? std::optional{ opt_call->types.front() } : std::nullopt;
		const std::optional<ResolvedType> opt_endpoint = rangeEndpointHint(*be_range->left, *be_range->right);
		if (!opt_endpoint) return {};
		ResolvedType rt_range(kRange);
		analyzer.applyArguments(rt_range, { *opt_endpoint });
		return scalar(node, &rt_range, floats);
	}
	if (const UnaryExpression *ue_op = dynamic_cast<const UnaryExpression *>(&node); ue_op && ue_op->op.get_operator() != oNot) {
		opt_hint = elementHint(*ue_op->operand, floats);
		if (!opt_hint) return {};
	} else if (const BinaryExpression *be_op = dynamic_cast<const BinaryExpression *>(&node); be_op && arithmetic(be_op->op.get_operator())) {
		opt_hint = elementHint(*be_op->left, floats);
		if (!opt_hint) opt_hint = elementHint(*be_op->right, floats);
		if (!opt_hint) return {};
	}
	return scalar(node, opt_hint ? &*opt_hint : nullptr, floats);
}

std::optional<ResolvedType> BodyChecker::rangeEndpointHint (const Expression &start, const Expression &end) {
	for (const Expression *ep_endpoint : { &start, &end }) {
		const std::optional<ResolvedType> opt_hint = comparisonHint(*ep_endpoint);
		if (opt_hint && analyzer.numeric(*opt_hint)) return opt_hint;
	}
	return {};
}

BodyChecker BodyChecker::comparisonProbe () const {
	BodyChecker bc_probe       = *this;
	bc_probe.record            = false;
	bc_probe.we_capture        = nullptr;
	bc_probe.ef_probeConstants = nullptr;
	return bc_probe;
}

std::optional<ResolvedType> BodyChecker::comparisonHint (const Expression &node, bool defaultLiterals) const {
	for (const CapturedOperand *co_value = captured; co_value; co_value = co_value->previous)
		if (co_value->node == &node)
			return co_value->value.resultKind() == ResultKind::Single ? std::optional{ co_value->value.types.front() } : std::nullopt;
	BodyChecker bc_probe = comparisonProbe();
	try {
		if (defaultLiterals) {
			bc_probe.b_inferenceDefaults = true;
			return bc_probe.scalar(node);
		}
		const std::optional<ResolvedType> opt_hint = bc_probe.elementHint(node, { true });
		if (opt_hint && dynamic_cast<const IfExpression *>(&node)) {
			// Only a branch that reaches the comparison can supply an operand type.
			return bc_probe.scalar(node, &*opt_hint, FloatContext{ true });
		}
		return opt_hint;
	} catch (const cde::ClothDiagnostic &error) {
		// The committed check supplies the peer's type and the effects of preceding operands.
		if (error.get_code() == Code::UNKNOWN_NAME) throw;
		return {};
	}
}

ExpressionInfo BodyChecker::arrayLiteralComparison (const BinaryExpression &node, const ArrayLiteral *left, const ArrayLiteral *right) {
	std::optional<ResolvedType> opt_peer;
	bool                        b_independent = false;
	if (left && right) {
		const std::optional<ResolvedType> opt_left = comparisonHint(*left), opt_right = comparisonHint(*right);
		b_independent = opt_left && opt_right && comparisonShape(*opt_left) != comparisonShape(*opt_right);
		opt_peer      = opt_left ? opt_left : opt_right;
		if (b_independent || (opt_peer && containsNullability(*opt_peer))) opt_peer.reset();
	}
	if (left && !opt_peer && !b_independent &&
	    (!right || (elementEvidence(*left) == ElementEvidence::None && elementEvidence(*right) != ElementEvidence::None))) {
		// The peer's actual overload determines its array type, including argument range checks.
		BodyChecker bc_probe = comparisonProbe();
		try {
			opt_peer = bc_probe.scalar(*node.right);
		} catch (const cde::ClothDiagnostic &) {
			// The right operand will be checked with the flow established by the left.
		}
	}

	const std::function operand = [] (BodyChecker &checker, const Expression &source, const ArrayLiteral *literal,
	                                  const std::optional<ResolvedType> &peer) {
		if (!literal || !peer || !hasArrayShape(*peer)) return checker.scalar(source);
		const ResolvedType rt_hint = comparisonShape(peer->nullable() ? peer->element() : *peer, literal);
		return checker.scalar(source, &rt_hint);
	};
	const std::optional<ResolvedType> opt_left = operand(*this, *node.left, left, opt_peer);
	std::optional<BodyChecker>        opt_unreachable;
	const std::optional<ResolvedType> opt_right =
	        operand(onPath(opt_left.has_value(), opt_unreachable), *node.right, right, opt_peer || b_independent ? opt_peer : opt_left);
	if (!opt_left || !opt_right) return ExpressionInfo::never();
	return equality(node, *opt_left, *opt_right);
}

std::optional<ExpressionInfo> BodyChecker::contextualRangeComparison (const BinaryExpression &node) {
	const std::function literal = [&] (const Expression &source, std::optional<ResolvedType> &hint) -> const BinaryExpression * {
		for (const CapturedOperand *co_value = captured; co_value; co_value = co_value->previous)
			if (co_value->node == &source) {
				if (co_value->value.resultKind() == ResultKind::Single) hint = co_value->value.types.front();
				return nullptr;
			}
		const BinaryExpression *be_range = dynamic_cast<const BinaryExpression *>(&source);
		if (!be_range || (be_range->op.get_operator() != oRange && be_range->op.get_operator() != oRangeInclusive)) return nullptr;
		if (analyzer.hasOperator(be_range->op.get_lexeme())) {
			BodyChecker bc_probe = comparisonProbe();
			try {
				if (const std::optional<ExpressionInfo> opt_call = bc_probe.binaryOperator(*be_range, nullptr, true, {})) {
					if (opt_call->resultKind() == ResultKind::Single) hint = opt_call->types.front();
					return nullptr;
				}
			} catch (const cde::ClothDiagnostic &) {
				// Resolve the operator with the effects of preceding operands in scope.
				return nullptr;
			}
		}
		return be_range;
	};
	std::optional<ResolvedType> opt_leftHint, opt_rightHint;
	const BinaryExpression     *be_left = literal(*node.left, opt_leftHint), *be_right = literal(*node.right, opt_rightHint);
	const Expression           *e_member = nullptr;
	std::optional<ResolvedType> opt_endpoint;
	if (be_left && be_right) {
		opt_endpoint = rangeEndpointHint(*be_left->left, *be_left->right);
		if (!opt_endpoint) opt_endpoint = rangeEndpointHint(*be_right->left, *be_right->right);
	} else {
		const std::function endpointOf = [&] (const Expression &source, const std::optional<ResolvedType> &hint) -> std::optional<ResolvedType> {
			if (const ResolvedType *rt_fixed = hint ? rangeEndpoint(*hint) : nullptr) return *rt_fixed;
			if (!dynamic_cast<const IfExpression *>(&source)) return {};
			// Literal-only branches can describe a range before their endpoint width is known.
			BodyChecker bc_probe                          = comparisonProbe();
			bc_probe.b_inferenceDefaults                  = true;
			const std::optional<ResolvedType> opt_natural = bc_probe.scalar(source);
			const ResolvedType               *rt_endpoint = opt_natural ? rangeEndpoint(*opt_natural) : nullptr;
			return rt_endpoint ? std::optional{ *rt_endpoint } : std::nullopt;
		};
		if (const BinaryExpression *be_literal = be_left ? be_left : be_right) {
			const Expression                 &e_peer   = be_left ? *node.right : *node.left;
			const std::optional<ResolvedType> opt_peer = comparisonHint(e_peer);
			opt_endpoint                               = endpointOf(e_peer, opt_peer);
			if (!opt_endpoint) {
				e_member = &e_peer;
				// The member's fixed type takes precedence over typed bounds and then literal defaults.
				opt_endpoint = opt_peer;
				for (const ExpressionPtr &ep_bound : { be_literal->left, be_literal->right })
					if (!opt_endpoint) opt_endpoint = comparisonHint(*ep_bound);
				for (const Expression *e_value : { &e_peer, be_literal->left.get(), be_literal->right.get() })
					if (!opt_endpoint) opt_endpoint = comparisonHint(*e_value, true);
				if (!opt_endpoint) opt_endpoint = ResolvedType(kInteger);
				if (!analyzer.numeric(*opt_endpoint))
					fail(Code::INVALID_RANGE_MEMBERSHIP, e_peer.get_span(), "Range membership requires a non-nullable numeric value");
			}
		} else {
			if (dynamic_cast<const NullLiteral *>(node.left.get()) || dynamic_cast<const NullLiteral *>(node.right.get())) return {};
			if (!opt_leftHint && hasEqualityHint(*node.left)) opt_leftHint = comparisonHint(*node.left);
			if (!opt_rightHint && hasEqualityHint(*node.right)) opt_rightHint = comparisonHint(*node.right);
			const ResolvedType *rt_left  = opt_leftHint ? rangeEndpoint(*opt_leftHint) : nullptr,
			                   *rt_right = opt_rightHint ? rangeEndpoint(*opt_rightHint) : nullptr;
			if (!rt_left && !rt_right) return {};
			opt_endpoint                                = rt_left ? *rt_left : *rt_right;
			const Expression                  &e_peer   = rt_left ? *node.right : *node.left;
			const std::optional<ResolvedType> &opt_hint = rt_left ? opt_rightHint : opt_leftHint;
			if (!endpointOf(e_peer, opt_hint ? opt_hint : comparisonHint(e_peer))) e_member = &e_peer;
		}
	}

	ResolvedType rt_range(kRange);
	if (opt_endpoint) analyzer.applyArguments(rt_range, { *opt_endpoint });
	std::exception_ptr  ex_bound;
	const std::function operand = [&] (BodyChecker &checker, const Expression &source, const BinaryExpression *range) -> std::optional<ResolvedType> {
		if (range && e_member) {
			ExpressionInfo ei_range;
			try {
				ei_range = checker.rangeValue(*range, &*opt_endpoint, Code::INVALID_RANGE_MEMBERSHIP);
			} catch (const cde::ClothDiagnostic &error) {
				if (!contextualFailure(error)) throw;
				// Check in source order, but diagnose the membership value before a mistyped bound.
				ex_bound = std::current_exception();
				return {};
			}
			ExpressionFacts *ef_body = checker.record ? &checker.bf_facts.expressions : checker.ef_probeConstants;
			if (ef_body) ef_body->insert_or_assign(range, ei_range);
			return ei_range.fallsThrough ? std::optional{ ei_range.types.front() } : std::nullopt;
		}
		const ResolvedType *rt_hint = opt_endpoint ? (&source == e_member ? &*opt_endpoint : &rt_range) : nullptr;
		return checker.scalar(source, rt_hint, FloatContext{ true });
	};
	const std::optional<ResolvedType> opt_left = operand(*this, *node.left, be_left);
	if (!opt_endpoint && opt_left && rangeEndpoint(*opt_left)) {
		opt_endpoint = *rangeEndpoint(*opt_left);
		analyzer.applyArguments(rt_range, { *opt_endpoint });
	}
	std::optional<BodyChecker>        opt_unreachable;
	const std::optional<ResolvedType> opt_right = operand(onPath(opt_left.has_value(), opt_unreachable), *node.right, be_right);
	if (ex_bound) {
		const std::optional<ResolvedType> &opt_member = be_left ? opt_right : opt_left;
		if (opt_member) requireRangeMember(*e_member, *opt_member, *opt_endpoint);
		std::rethrow_exception(ex_bound);
	}
	if (!opt_left || !opt_right) return ExpressionInfo::never();
	return equality(node, *opt_left, *opt_right);
}

ExpressionInfo BodyChecker::rangeValue (const BinaryExpression &node, const ResolvedType *endpoint, Code code) {
	std::optional<ResolvedType> required      = endpoint ? std::optional{ *endpoint } : rangeEndpointHint(*node.left, *node.right);
	const std::function         checkEndpoint = [&] (BodyChecker &checker, const Expression &operand) {
		const std::optional<ResolvedType> type = checker.scalar(operand, required ? &*required : nullptr, FloatContext{ true });
		if (!required) required = type;
		if (type && (!analyzer.numeric(*type) || *type != required)) fail(code, operand.get_span(), "Range endpoints require one exact numeric type");
		const ExpressionFacts *ef_numeric = checker.record ? &checker.bf_facts.expressions : checker.ef_probeConstants;
		if (!b_inferenceDefaults && ef_numeric && type && type->floating()) requireFinite(operand, *ef_numeric, code);
		return type.has_value();
	};
	const bool                 b_start = checkEndpoint(*this, *node.left);
	std::optional<BodyChecker> unreachable;
	const bool                 b_end = checkEndpoint(onPath(b_start, unreachable), *node.right);
	if (!b_start || !b_end) return ExpressionInfo::never();
	ResolvedType rt_range(kRange);
	analyzer.applyArguments(rt_range, { *required });
	return { .types = { std::move(rt_range) }, .range = RangeOperation::Construct };
}

ExpressionInfo BodyChecker::elvis (const ElvisExpression &node, const ResultTypes *expected, FloatContext floats) {
	const ResolvedType *rt_expected   = expected && expected->size() == 1 && !expected->front().name.empty() ? &expected->front() : nullptr;
	const bool          b_nullLiteral = dynamic_cast<const NullLiteral *>(node.left.get()) != nullptr;
	if (b_nullLiteral && !rt_expected) {
		const ExpressionInfo ei_fallback = scalarInfo(*node.fallback, nullptr, floats);
		if (!ei_fallback.fallsThrough || ei_fallback.types.front() == ResolvedType(kNull))
			fail(Code::EXPLICIT_NULLABLE_TYPE, node.get_span(), "The fallback must supply a nullable payload type");
		ResolvedType rt_nullable = ei_fallback.types.front();
		if (!rt_nullable.nullable()) rt_nullable.suffixes.emplace_back(to_string(oQuestion));
		scalar(*node.left, &rt_nullable);
		return { .types = ei_fallback.types, .representedType = record ? ei_fallback.representedType : std::nullopt };
	}
	std::optional<ResolvedType> opt_context;
	if (rt_expected && rt_expected->nullable())
		opt_context = *rt_expected;
	else if (b_nullLiteral) {
		opt_context = *rt_expected;
		opt_context->suffixes.emplace_back(to_string(oQuestion));
	}
	const ExpressionInfo ei_left = scalarInfo(*node.left, opt_context ? &*opt_context : nullptr, floats);
	if (!ei_left.fallsThrough) {
		std::optional<BodyChecker> opt_unreachable;
		onPath(false, opt_unreachable).scalar(*node.fallback, rt_expected, floats);
		return ExpressionInfo::never();
	}
	ResolvedType rt = ei_left.types.front();
	const std::optional<TypedPlace> tp_left = locate(*node.left);
	if (!rt.nullable() && tp_left && tp_left->type.nullable()) rt = tp_left->type;
	if (!rt.nullable()) {
		fail(Code::INVALID_ELVIS, node.op.get_span(), std::format("Left operand of {} must be nullable", to_string(oElvis)));
	}
	const ResolvedType rt_element = rt.element();
	if (rt_element.floating()) floats = { true, rt_element.width };
	BodyChecker bc_missing   = *this;
	bc_missing.fs_flow.exits = {};
	if (tp_left && tp_left->place.stable()) forgetPresence(bc_missing.fs_flow.narrowed, tp_left->place);
	if (!ei_left.types.front().nullable()) bc_missing.we_capture = nullptr;
	const ExpressionInfo ei_fallback = bc_missing.scalarInfo(*node.fallback, rt_expected && rt_expected->nullable() ? &rt : &rt_element, floats);
	if (b_nullLiteral) {
		mergeExits(fs_flow.exits, bc_missing.fs_flow.exits);
		fs_flow.narrowed = std::move(bc_missing.fs_flow.narrowed);
		if (!ei_fallback.fallsThrough) return { .types = { rt_element }, .fallsThrough = false };
	} else if (ei_left.types.front().nullable()) {
		assumePresent(*node.left);
		mergeExits(fs_flow.exits, bc_missing.fs_flow.exits);
		if (ei_fallback.fallsThrough) intersect(fs_flow.narrowed, bc_missing.fs_flow.narrowed);
	}
	ExpressionInfo ei_result = value(rt_element);
	if (ei_fallback.fallsThrough && !analyzer.accepts(rt_element, ei_fallback.types.front())) {
		if (!analyzer.accepts(rt, ei_fallback.types.front()))
			fail(Code::INVALID_ELVIS, node.fallback->get_span(), "Fallback requires " + rt_element.spelling() + " or " + rt.spelling());
		ei_result.types = { rt };
	}
	if (record) {
		if (b_nullLiteral)
			ei_result.representedType = ei_fallback.representedType;
		else if (!ei_fallback.fallsThrough || ei_left.representedType == ei_fallback.representedType)
			ei_result.representedType = ei_left.representedType;
	}
	return ei_result;
}

void BodyChecker::numericPeers (const Expression &node, NumericPeers &peers, bool contextualFloat) {
	if (peers.integer && (!contextualFloat || peers.floatingWidth)) return;
	std::optional<ResolvedType> opt_type;
	for (const CapturedOperand *co_operand = captured; co_operand; co_operand = co_operand->previous)
		if (co_operand->node == &node && co_operand->value.types.size() == 1) opt_type = co_operand->value.types.front();
	if (!opt_type) {
		if (const BinaryExpression *be_op = dynamic_cast<const BinaryExpression *>(&node)) {
			if (!arithmetic(be_op->op.get_operator())) return;
			const OperandProbe *op_cached = nullptr;
			if (operatorProbes && !captured && !b_inferenceDefaults) {
				const OperatorProbes::const_iterator it_probe = operatorProbes->find(&node);
				if (it_probe != operatorProbes->end() && it_probe->second.before == fs_flow) op_cached = &it_probe->second;
			}
			if (op_cached) {
				if (op_cached->overloaded) opt_type = op_cached->type;
			} else if (const std::optional<ExpressionInfo> opt_overload = binaryOperator(*be_op, nullptr, true, {});
			           opt_overload && opt_overload->types.size() == 1) {
				opt_type = opt_overload->types.front();
			}
			if (!opt_type) {
				numericPeers(*be_op->left, peers, contextualFloat);
				numericPeers(*be_op->right, peers, contextualFloat);
				return;
			}
		} else if (const UnaryExpression *ue_op = dynamic_cast<const UnaryExpression *>(&node)) {
			if (ue_op->op.get_operator() == oNot) return;
			if (analyzer.hasOperator(ue_op->op.get_lexeme()) && elementEvidence(*ue_op->operand) == ElementEvidence::Typed) {
				const ExpressionInfo ei_result = scalarInfo(node);
				if (ei_result.symbol && ei_result.types.size() == 1) opt_type = ei_result.types.front();
			}
			if (!opt_type) {
				numericPeers(*ue_op->operand, peers, contextualFloat);
				return;
			}
		} else if (dynamic_cast<const NameExpression *>(&node) || dynamic_cast<const CastExpression *>(&node) ||
		           dynamic_cast<const CallSite *>(&node) || dynamic_cast<const IndexExpression *>(&node) ||
		           dynamic_cast<const FieldAccessExpression *>(&node) || dynamic_cast<const ProviderAccessExpression *>(&node))
			opt_type = scalar(node);
	}
	if (!opt_type) return;
	if (!peers.integer && opt_type->integer()) peers.integer = *opt_type;
	if (contextualFloat && !peers.floatingWidth && opt_type->floating()) peers.floatingWidth = opt_type->width;
}

ExpressionInfo BodyChecker::binary (const BinaryExpression &node, const ResolvedType *expected, FloatContext floats, bool allowContexts) {
	// Candidate selection and numeric peer discovery share natural operand types for this expression.
	OperatorProbes m_probes;

	struct ProbeScope {
		OperatorProbes *&active;
		OperatorProbes  *previous;

		~ProbeScope () {
			active = previous;
		}
	} scope{ operatorProbes, operatorProbes };

	if (!operatorProbes) operatorProbes = &m_probes;
	if (const std::optional<ExpressionInfo> opt_call = binaryOperator(node, expected, allowContexts, floats)) return *opt_call;
	return builtinBinary(node, expected, floats);
}

ExpressionInfo BodyChecker::builtinBinary (const BinaryExpression &node, const ResolvedType *expected, FloatContext floats) {
	const Operator o_operation = node.op.get_operator();
	if (o_operation == oElvis) {
		const ResultTypes v_hint = expected ? ResultTypes{ *expected } : ResultTypes{};
		return elvis({ node.left, node.op, node.right }, expected ? &v_hint : nullptr, floats);
	}
	if (o_operation == oAnd || o_operation == oOr) {
		TestPaths                 tp_left      = test(*node.left);
		const bool                b_and        = o_operation == oAnd;
		std::optional<Narrowing> &opt_evaluate = b_and ? tp_left.yes : tp_left.no;
		BodyChecker               bc_right     = *this;
		bc_right.fs_flow                       = { opt_evaluate.value_or(fs_flow.narrowed), {} };
		if (!opt_evaluate) bc_right.we_capture = nullptr;
		TestPaths tp_right = bc_right.test(*node.right);
		if (opt_evaluate)
			mergeExits(fs_flow.exits, bc_right.fs_flow.exits);
		else
			tp_right = {};
		std::optional<Narrowing>       &opt_skip  = b_and ? tp_left.no : tp_left.yes;
		const std::optional<Narrowing> &opt_short = b_and ? tp_right.no : tp_right.yes;
		if (opt_short) join(opt_skip, *opt_short);
		opt_evaluate                       = b_and ? tp_right.yes : tp_right.no;
		std::optional<Narrowing> opt_after = tp_left.yes;
		if (tp_left.no) join(opt_after, *tp_left.no);
		e_test  = &node;
		tp_test = std::move(tp_left);
		if (!opt_after) return ExpressionInfo::never();
		fs_flow.narrowed = std::move(*opt_after);
		return value(ResolvedType(kBoolean));
	}
	if (o_operation == oRange || o_operation == oRangeInclusive)
		return rangeValue(node, expected ? rangeEndpoint(*expected) : nullptr, Code::INVALID_RANGE_VALUE);
	const bool equality = o_operation == oEqual || o_operation == oNotEqual || node.op.get_lexeme() == to_string(kIs);
	if (equality) {
		try {
			if (const std::optional<ExpressionInfo> opt_range = contextualRangeComparison(node)) return *opt_range;
		} catch (const cde::ClothDiagnostic &error) {
			// A range comparison owns its numeric contract, including inside an overload trial.
			throw IntrinsicDiagnostic(error);
		}
		const ArrayLiteral *al_left  = dynamic_cast<const ArrayLiteral *>(node.left.get()),
		                   *al_right = dynamic_cast<const ArrayLiteral *>(node.right.get());
		if ((al_left || al_right) && isEquality(o_operation) && !dynamic_cast<const NullLiteral *>(node.left.get()) &&
		    !dynamic_cast<const NullLiteral *>(node.right.get()))
			return arrayLiteralComparison(node, al_left, al_right);
	}
	const bool   compare = isComparison(o_operation);
	NumericPeers np_peers;
	if (takesNumericContext(*node.left) || takesNumericContext(*node.right)) {
		BodyChecker bc_probe = *this;
		bc_probe.record      = false;
		bc_probe.we_capture  = nullptr;
		for (const ExpressionPtr &ep_operand : { node.left, node.right }) {
			try {
				bc_probe.numericPeers(*ep_operand, np_peers, floats.contextual);
			} catch (const cde::ClothDiagnostic &) {
				// An operand can depend on effects of its predecessor. Check it with its actual incoming flow below.
			}
		}
	}
	if (!compare && expected && expected->integer() && !np_peers.integer) np_peers.integer = *expected;
	if (floats.contextual) {
		if (np_peers.floatingWidth)
			floats.width = np_peers.floatingWidth;
		else if (!floats.width && !compare && expected && expected->floating())
			floats.width = expected->width;
	}
	std::optional<ResolvedType> opt_numeric = np_peers.integer;
	if (!opt_numeric && floats.width) {
		opt_numeric        = ResolvedType(kFloat);
		opt_numeric->width = floats.width;
	}
	std::optional<ResolvedType> opt_hint = opt_numeric;
	if (equality && numericLiteral(*node.left) && hasEqualityHint(*node.right)) {
		if (const std::optional<ResolvedType> opt_peer = comparisonHint(*node.right); opt_peer && opt_peer->nullable()) opt_hint = opt_peer;
	}
	const std::optional<ResolvedType> opt_left = scalar(*node.left, opt_hint ? &*opt_hint : nullptr, floats);
	std::optional<BodyChecker>        opt_unreachable;
	opt_hint = opt_numeric;
	if (equality && opt_left && !analyzer.numeric(*opt_left) && !rangeEndpoint(*opt_left) && !dynamic_cast<const NullLiteral *>(node.right.get()))
		opt_hint = hasArrayShape(*opt_left) ? comparisonShape(opt_left->nullable() ? opt_left->element() : *opt_left) : *opt_left;
	const std::optional<ResolvedType> opt_right =
	        onPath(opt_left.has_value(), opt_unreachable).scalar(*node.right, opt_hint ? &*opt_hint : nullptr, floats);
	if (!opt_left || !opt_right) return ExpressionInfo::never();
	const ResolvedType &left = *opt_left, &right = *opt_right;
	if (isEquality(o_operation) || equality) return this->equality(node, left, right);
	if (o_operation == oPlus && left == ResolvedType(kString) && right == left) {
		return value(left);
	}
	if (left.nullable() || right.nullable()) fail(Code::NULLABLE_PAYLOAD_ACCESS, node.op.get_span(), "This operation requires a present value");
	if (!analyzer.numeric(left) || !analyzer.numeric(right) || ((left.parameter || right.parameter) && left != right)) {
		if (!analyzer.members(left, node.op.get_lexeme(), SymbolKind::Operator).empty() ||
		    !analyzer.members(right, node.op.get_lexeme(), SymbolKind::Operator).empty())
			fail(Code::OPERATOR_MATCH, node.op.get_span(), "No matching overload for operator '" + node.op.get_lexeme() + "'");
		fail(Code::BINARY_OPERANDS, node.op.get_span(), std::format("Operator '{}' requires numeric operands", node.op.get_lexeme()));
	}
	if (o_operation == oMulMul && left.integer() && right.integer()) {
		const UnaryExpression *exponent  = dynamic_cast<const UnaryExpression *>(node.right.get());
		const IntegerLiteral  *magnitude = exponent ? dynamic_cast<const IntegerLiteral *>(exponent->operand.get()) : nullptr;
		if (magnitude && exponent->op.get_operator() == oMinus && (magnitude->value.size() != 1 || magnitude->value.front()))
			fail(Code::NEGATIVE_EXPONENT, exponent->get_span(), "An integer exponent must be nonnegative");
	}
	if (left.integer() && right.integer() && left != right) {
		fail(Code::MIXED_INTEGERS, node.op.get_span(), "Integer operand types must match");
	}
	if (compare) {
		return value(ResolvedType(kBoolean));
	}
	if (left.floating() || right.floating()) {
		ResolvedType result(kFloat);
		result.width = std::max(left.floating() ? left.width : result.width, right.floating() ? right.width : result.width);
		return value(result);
	}
	return value(left);
}

std::vector<Candidate> BodyChecker::callCandidates (const CallExpression &node) {
	checkDefaultName(node.name);
	if (const Binding *b_local = local(node.name.get_lexeme()))
		fail(Code::NOT_CALLABLE, node.name.get_span(), "Local binding '" + node.name.get_lexeme() + "' is not callable", b_local->name);
	std::vector<Candidate> group;
	// Built-ins occupy the lexical function namespace. Explicit receivers still select their own members.
	const bool b_builtinName = builtinFunctionNamed(node.name.get_lexeme());
	if (!b_builtinName && owner && owner->kind == SymbolKind::State) {
		group = analyzer.members(analyzer.substitute(analyzer.ownerType(*owner), m_arguments), node.name.get_lexeme(), SymbolKind::Method,
		                         MemberLookup::CallContract);
		for (Candidate &c_method : group) c_method.arguments.insert(m_arguments.begin(), m_arguments.end());
	}
	if (!b_builtinName && hasInstance() && group.empty()) {
		group = analyzer.members(selfType(), node.name.get_lexeme(), SymbolKind::Method, MemberLookup::CallContract);
	}
	if (!group.empty()) checkValidatorReceiver(nullptr, node.get_span());
	if (!group.empty() && rp_receiverPhase == ReceiverPhase::FieldInitializer) {
		fail(Code::PARTIAL_CLASS_INSTANCE, node.name.get_span(), std::format("Methods on '{}' require a fully initialized instance", to_string(kSelf)));
	}
	if (!group.empty() && pd_default) {
		fail(Code::INVALID_PARAMETER_DEFAULT, node.name.get_span(), "A default cannot call a method on its receiver", &pd_default->name);
	}
	if (group.empty()) {
		const Candidates candidates = !b_builtinName && hasInstance() ? analyzer.lookup(context, node.name.get_lexeme()) :
		                                                                analyzer.lookupFile(context, node.name.get_lexeme());
		for (const Symbol *sym : candidates) {
			if (sym->kind == SymbolKind::Function) {
				group.push_back({ sym });
			}
		}
		if (group.empty() && !b_builtinName && !candidates.empty()) {
			fail(Code::NOT_CALLABLE, node.name.get_span(), "'" + node.name.get_lexeme() + "' is not callable", &candidates.front()->name);
		}
	}
	return group;
}

ExpressionInfo BodyChecker::call (const CallExpression &node) {
	const std::vector<Candidate> group = callCandidates(node);
	if (group.empty() && builtinFunctionNamed(node.name.get_lexeme()))
		return builtinCall(node, node.typeArguments ? &*node.typeArguments : nullptr).value();
	if (group.empty() && node.typeArguments) checkTypeArguments(*node.typeArguments);
	ExpressionInfo ei_call = invoke(node.name, node.get_span(), node.suppliedArguments, group, node.typeArguments ? &*node.typeArguments : nullptr);
	if (ei_call.fallsThrough && ei_call.symbol->kind == SymbolKind::Method && owner && owner->kind == SymbolKind::Type &&
	    dynamic_cast<const StructDeclaration *>(std::get<const TypeDeclaration *>(owner->declaration)))
		invalidate({ &context.name }, true);
	return ei_call;
}

void BodyChecker::unknownMember (const MemberCallExpression &call, std::string_view receiver) {
	if (call.typeArguments) checkTypeArguments(*call.typeArguments);
	checkArgumentNames(call, 0);
	fail(Code::UNKNOWN_MEMBER, call.name.get_span(), std::format("Unknown method '{}' on {}", call.name.get_lexeme(), receiver));
}

ExpressionInfo BodyChecker::memberCall (const MemberCallExpression &node) {
	const ExpressionInfo    ei_receiver = scalarInfo(*node.receiver);
	const TypeArgumentList *tal_types   = node.typeArguments ? &*node.typeArguments : nullptr;
	if (std::optional<ExpressionInfo> opt_builtin = builtinCall(node, tal_types, &ei_receiver)) return std::move(*opt_builtin);
	const std::optional<ArrayMutation> opt_mutation = arrayMutationNamed(node.name.get_lexeme());
	if (!ei_receiver.fallsThrough) {
		if (!opt_mutation) unknownMember(node, "a non-returning receiver");
		ResolvedType rt_array(kInteger);
		rt_array.suffixes.push_back(arraySuffix());
		return mutateArray(node, { .types = { rt_array }, .fallsThrough = false }, *opt_mutation);
	}
	const ResolvedType &rt_receiver = ei_receiver.types.front();
	if (rt_receiver.nullable()) {
		const ResolvedType rt_payload = rt_receiver.element();
		if (!rt_payload.declaration && !rt_payload.parameter && !rt_payload.array()) unknownMember(node, rt_receiver.spelling());
		fail(Code::NULLABLE_PAYLOAD_ACCESS, node.get_span(), std::format("Use {} to call a member of a nullable receiver", to_string(oSafeDot)));
	}
	checkValidatorReceiver(node.receiver.get(), node.get_span());
	if (rt_receiver.array() && opt_mutation) {
		if (readOnlyDefaultRoot(*node.receiver))
			fail(Code::INVALID_PARAMETER_DEFAULT, node.name.get_span(), "A default cannot mutate a read-only receiver", &pd_default->name);
		return mutateArray(node, ei_receiver, *opt_mutation);
	}
	const std::vector<Candidate> v_methods = methodCandidates(node, rt_receiver);
	if (v_methods.empty()) unknownMember(node, rt_receiver.spelling());
	if (readOnlyDefaultRoot(*node.receiver))
		fail(Code::INVALID_PARAMETER_DEFAULT, node.name.get_span(), "A default cannot call a method on a read-only receiver", &pd_default->name);
	if (rp_receiverPhase == ReceiverPhase::FieldInitializer && ei_receiver.receiverFieldPath == ReceiverFieldPath::Stored &&
	    asStruct(rt_receiver.declaration))
		fail(Code::PARTIAL_CLASS_INSTANCE, node.receiver->get_span(), "A method on a stored field requires a fully initialized instance");
	ExpressionInfo ei_call = invoke(node.name, node.get_span(), node.suppliedArguments, v_methods, tal_types, nullptr,
	                                rt_receiver.declaration && !analyzer.referenceType(rt_receiver) ? node.receiver.get() : nullptr);
	if (ei_call.fallsThrough) mutatingReceiver(*node.receiver, rt_receiver);
	return ei_call;
}

ExpressionInfo BodyChecker::create (const ClassCreationExpression &node) {
	std::optional<ResolvedType> opt_receiver = receiverType(*node.receiver, true);
	if (!opt_receiver || (!asClass(opt_receiver->declaration) && !opt_receiver->parameter) || !opt_receiver->suffixes.empty()) {
		fail(Code::INVALID_CLASS_LIFECYCLE, node.receiver->get_span(), std::format("{} requires a class type or a constructor bound", to_string(kNew)));
	}
	ResolvedType &rt = *opt_receiver;
	// The guarantee owns its call syntax, even if the concrete constructor uses different labels or defaults.
	const Symbol *sym_guarantee = nullptr;
	Substitutions m_guaranteeArguments;
	if (!m_arguments.empty()) {
		const BodyFacts                                                             &bf_open = openFacts();
		const std::unordered_map<const Expression *, ExpressionInfo>::const_iterator it_open = bf_open.expressions.find(&node);
		if (it_open != bf_open.expressions.end() && it_open->second.argumentContract &&
		    dynamic_cast<const ConstructorGuaranteeDeclaration *>(callable(*it_open->second.argumentContract))) {
			sym_guarantee        = it_open->second.argumentContract;
			m_guaranteeArguments = m_arguments;
			for (const Substitutions::value_type &entry : it_open->second.typeArguments)
				m_guaranteeArguments.insert_or_assign(entry.first, analyzer.substitute(entry.second, m_arguments));
		}
	}
	std::vector<Candidate> constructors;
	if (sym_guarantee) {
		constructors.push_back({ sym_guarantee, m_guaranteeArguments });
	} else {
		constructors = analyzer.members(rt, rt.name, SymbolKind::Constructor);
	}
	if (constructors.empty()) {
		fail(Code::INVALID_CLASS_LIFECYCLE, node.get_span(), "No constructor declared for " + rt.spelling());
	}
	const TypeParameterList      *params     = rt.declaration ? typeParameters(*rt.declaration) : nullptr;
	const TypeReceiverExpression *annotation = dynamic_cast<const TypeReceiverExpression *>(node.receiver.get());
	const TypeArgumentList       *written = annotation && annotation->annotation.get_application() ? &*annotation->annotation.get_application() : nullptr;
	if (params && !sym_guarantee) {
		for (Candidate &candidate : constructors) {
			candidate.inferOwner = params;
			for (const TypeParameterDeclaration &tp_parameter : params->parameters) candidate.arguments.erase(&tp_parameter);
		}
	}
	ExpressionInfo result = invoke(node.name, node.get_span(), node.suppliedArguments, constructors, params && !sym_guarantee ? written : nullptr);
	if (params && !sym_guarantee) rt = analyzer.substitute(analyzer.ownerType(*rt.declaration), result.typeArguments);
	analyzer.checkBounds(rt, node.get_span(), context);
	if (sym_guarantee) {
		ResultTypes v_parameters;
		for (const ResolvedType &rt_parameter : analyzer.declarations.at(sym_guarantee).parameters) {
			v_parameters.push_back(analyzer.substitute(rt_parameter, m_guaranteeArguments));
		}
		result.symbol = analyzer.constructorFor(rt, v_parameters, nullptr);
		if (!result.symbol) fail(Code::UNSATISFIED_GENERIC_BOUND, node.get_span(), "Missing guaranteed constructor", &sym_guarantee->name);
		remember(node.name, result.symbol->name);
		if (record) {
			result.instance         = analyzer.useCallable(*result.symbol, analyzer.argumentsOf(rt), context, node.get_span());
			result.contractInstance = analyzer.guaranteeOwner(*sym_guarantee, m_guaranteeArguments, context, node.get_span());
		}
	}
	if (record) {
		analyzer.useType(rt, context, node.get_span());
		bf_facts.expressions.at(node.receiver.get()).representedType = rt;
	}
	result.types = { rt };
	return result;
}

ExpressionInfo BodyChecker::cast (const CastExpression &node) {
	if (node.target.get_spelling() == to_string(kVoid)) {
		const ExpressionInfo ei_operand = expression(*node.operand, nullptr, true);
		return ei_operand.fallsThrough ? ExpressionInfo{} : ExpressionInfo::never();
	}
	const ExpressionInfo ei_operand = scalarInfo(*node.operand);
	if (const Token *id = node.target.get_bare_identifier(); ei_operand.fallsThrough && id && !analyzer.hasTypeName(context, *id)) {
		if (const std::optional<Candidate> state = stateTarget(ei_operand.types.front(), *id, *node.operand)) {
			if (readOnlyDefaultRoot(*node.operand))
				fail(Code::INVALID_PARAMETER_DEFAULT, node.get_span(), "A default cannot change the state of a read-only receiver",
				     &pd_default->name);
			return transition(*state, nullptr, node.get_span());
		}
	}
	const ResolvedType to = bodyType(node.target);
	if (!ei_operand.fallsThrough) return ExpressionInfo::never();
	const ResolvedType &from = ei_operand.types.front();
	if (const std::optional<Candidate> opt_cast = selectContext(node.op, from, to, true)) {
		ExpressionInfo ei_cast = callOperator(*opt_cast, *node.operand, from, node.op, node.get_span());
		require(ei_cast.types, { to }, node.get_span(), Code::INVALID_CAST);
		ei_cast.types = { to };
		return ei_cast;
	}
	if ((from.nullable() || from.name == to_string(kNull)) && !to.nullable()) {
		fail(Code::NULLABLE_PAYLOAD_ACCESS, node.get_span(), "A nullable value needs a null check before casting");
	}
	const ResolvedType source = from.nullable() ? from.element() : from, target = to.nullable() ? to.element() : to;
	const bool ordinal = source.declaration && dynamic_cast<const EnumDeclaration *>(std::get<const TypeDeclaration *>(source.declaration->declaration)) &&
	                     target == ResolvedType(kInteger);
	if (!(analyzer.numeric(source) && analyzer.numeric(target)) && !ordinal && !analyzer.accepts(to, from)) {
		fail(Code::INVALID_CAST, node.get_span(), "Cannot cast " + from.spelling() + " to " + to.spelling());
	}
	return { .types = { to }, .representedType = to == ResolvedType(kTypeinfo) ? ei_operand.representedType : std::nullopt };
}

ExpressionInfo BodyChecker::infer (const Expression &node, const ResultTypes *expected, bool discard, FloatContext floats) {
	const ResolvedType         *hint = expected && expected->size() == 1 ? &expected->front() : nullptr;
	std::optional<ResolvedType> payload;
	if (hint && hint->nullable()) {
		payload = hint->element();
		hint    = &*payload;
	}
	if (const auto *n = dynamic_cast<const NameExpression *>(&node)) {
		return name(n->name);
	}
	if (const auto *n = dynamic_cast<const IntegerLiteral *>(&node)) {
		return value(literalInteger(*n, hint, false, analyzer.program.layout, b_inferenceDefaults));
	}
	if (const auto *n = dynamic_cast<const FloatingPointLiteral *>(&node)) {
		ResolvedType rt(kFloat);
		if (floats.contextual) {
			if (floats.width)
				rt.width = floats.width;
			else if (hint && hint->floating())
				rt = *hint;
		}
		if (!b_inferenceDefaults && !std::isfinite(floatingLiteral(n->value, rt.width))) {
			fail(Code::FLOAT_RANGE, node.get_span(), "Floating-point literal is outside " + rt.spelling());
		}
		return value(rt);
	}
	if (dynamic_cast<const BooleanLiteral *>(&node)) {
		return value(ResolvedType(kBoolean));
	}
	if (dynamic_cast<const CharacterLiteral *>(&node)) {
		return value(ResolvedType(kCharacter));
	}
	if (dynamic_cast<const StringLiteral *>(&node)) {
		return value(ResolvedType(kString));
	}
	if (dynamic_cast<const NullLiteral *>(&node)) {
		return value(payload ? expected->front() : ResolvedType(kNull));
	}
	if (const auto *n = dynamic_cast<const InterpolatedString *>(&node)) {
		bool                       b_continues = true;
		std::optional<BodyChecker> opt_unreachable;
		for (const ExpressionPtr &part : n->parts) {
			BodyChecker                      &bc_part  = onPath(b_continues, opt_unreachable);
			const std::optional<ResolvedType> opt_part = bc_part.scalar(*part);
			if (opt_part && !dynamic_cast<const StringLiteral *>(part.get())) {
				if (!bc_part.bindText(*part, *opt_part))
					fail(Code::UNSUPPORTED_INTERPOLATION_TYPE, part->get_span(), "No text conversion for " + opt_part->spelling());
			}
			b_continues &= opt_part.has_value();
		}
		return b_continues ? value(ResolvedType(kString)) : ExpressionInfo::never();
	}
	if (const auto *n = dynamic_cast<const ResultListExpression *>(&node)) {
		ResultTypes                              v_values, shapes;
		std::vector<std::optional<ResolvedType>> v_metadata;
		std::optional<BodyChecker>               opt_unreachable;
		for (std::size_t st_slot = 0; st_slot < n->values.size(); ++st_slot) {
			BodyChecker        &bc_operand = onPath(v_values.size() == st_slot, opt_unreachable);
			const ResolvedType *rt_slot    = expected && st_slot < expected->size() ? &(*expected)[st_slot] : nullptr;
			const FloatContext  fc_slot    = !rt_slot || suppliesFloatingContext(*rt_slot) ? floats : FloatContext{};
			ExpressionInfo      ei_value   = bc_operand.scalarInfo(*n->values[st_slot], rt_slot, fc_slot);
			if (ei_value.fallsThrough) {
				shapes.push_back(shapeOf(*n->values[st_slot], ei_value.types.front()));
				v_metadata.push_back(ei_value.representedType);
				v_values.push_back(std::move(ei_value.types.front()));
			}
		}
		if (v_values.size() != n->values.size()) return ExpressionInfo::never();
		if (!std::ranges::any_of(v_values, hasArrayShape)) shapes.clear();
		if (std::ranges::none_of(v_metadata, &std::optional<ResolvedType>::has_value)) v_metadata.clear();
		return { .types = std::move(v_values), .arrayShapes = std::move(shapes), .representedResults = std::move(v_metadata) };
	}
	if (const auto *n = dynamic_cast<const ArrayLiteral *>(&node)) {
		std::optional<ResolvedType> elem;
		if (hint && hint->array()) {
			const std::string &extent = hint->suffixes.back();
			if (extent != arraySuffix() && extent != arraySuffix(std::to_string(n->elements.size())))
				fail(Code::INVALID_ARRAY, node.get_span(), "Array literal length does not match " + hint->spelling());
			elem = hint->element();
		}
		if (!elem) {
			BodyChecker bc_probe = *this;
			bc_probe.record      = false;
			bc_probe.we_capture  = nullptr;
			for (const ExpressionPtr &ep_part : n->elements)
				mergeElementHint(elem, bc_probe.elementHint(*ep_part, floats), ep_part->get_span(), Code::INVALID_ARRAY);
			if (elem) checkInferredBinding(*elem, node.get_span());
		}
		if ((n->elements.empty() && !elem) || (elem && elem->name == to_string(kNull))) {
			fail(Code::INVALID_ARRAY, node.get_span(), "Array literal needs an element type");
		}
		bool                        b_continues = true;
		std::optional<ResolvedType> shape       = elem;
		std::optional<BodyChecker>  opt_unreachable;
		for (const ExpressionPtr &part : n->elements) {
			BodyChecker                      &bc_part = onPath(b_continues, opt_unreachable);
			const std::optional<ResolvedType> actual  = bc_part.scalar(*part, elem ? &*elem : nullptr, floats);
			b_continues &= actual.has_value();
			if (!actual) continue;
			if (!elem) {
				elem  = *actual;
				shape = bc_part.shapeOf(*part, *actual);
			}
			const TokenSpan span = part->get_span();
			if (!analyzer.accepts(*elem, *actual))
				fail(Code::INVALID_ARRAY, span, "Array element requires " + elem->spelling() + ", found " + actual->spelling());
			*elem  = refineShape(*elem, *actual, &span);
			*shape = refineShape(*shape, bc_part.shapeOf(*part, *actual), &span);
		}
		if (!b_continues || !elem) return ExpressionInfo::never();
		const std::string outer = arraySuffix(std::to_string(n->elements.size()));
		elem->suffixes.push_back(outer);
		shape->suffixes.push_back(outer);
		return { .types = { *elem }, .arrayShapes = { *shape } };
	}
	if (const auto *n = dynamic_cast<const StructLiteral *>(&node)) {
		const StructDeclaration *td =
		        hint && hint->declaration && hint->declaration->kind == SymbolKind::Type && hint->suffixes.empty() ?
		                dynamic_cast<const StructDeclaration *>(std::get<const TypeDeclaration *>(hint->declaration->declaration)) :
		                nullptr;
		if (!td || td->fields.size() != n->fields.size()) {
			fail(Code::INVALID_STRUCT_LITERAL, node.get_span(), "Struct literal needs a matching struct type and field count");
		}
		bool                       b_continues = true;
		std::optional<BodyChecker> opt_unreachable;
		for (std::size_t i = 0; i < n->fields.size(); ++i) {
			const Candidate member = analyzer.members(*hint, td->fields[i].name.get_lexeme(), SymbolKind::Field).front();
			analyzer.requireAccess(*member.symbol, context, n->fields[i]->get_span());
			const ResolvedType                required = analyzer.substitute(*analyzer.declarations.at(member.symbol).value, member.arguments);
			const std::optional<ResolvedType> actual   = onPath(b_continues, opt_unreachable).scalar(*n->fields[i], &required);
			if (actual) require({ *actual }, { required }, n->fields[i]->get_span(), Code::INVALID_STRUCT_FIELD);
			b_continues &= actual.has_value();
		}
		return b_continues ? value(*hint) : ExpressionInfo::never();
	}
	if (const auto *n = dynamic_cast<const IndexExpression *>(&node)) {
		const ExpressionInfo ei_array = scalarInfo(*n->array);
		if (ei_array.types.size() != 1) {
			std::optional<BodyChecker> opt_unreachable;
			onPath(false, opt_unreachable).scalar(*n->index);
			return ExpressionInfo::never();
		}
		const ResolvedType &array = ei_array.types.front();
		if (array.declaration && !array.array()) {
			const std::vector<Candidate> v_getters = analyzer.members(array, to_string(oIndexRead), SymbolKind::Operator);
			if (v_getters.empty()) fail(Code::INVALID_INDEX, node.get_span(), "This type has no index getter");
			std::optional<BodyChecker> opt_unreachable;
			BodyChecker               &bc_index   = onPath(ei_array.fallsThrough, opt_unreachable);
			ExpressionInfo             ei_element = bc_index.invoke(v_getters.front().symbol->name, node.get_span(), { { n->index } }, v_getters);
			ei_element.indexed                    = bc_index.indexAccess(ei_element, node.get_span());
			if (ei_element.fallsThrough) bc_index.mutatingReceiver(*n->array, array);
			if (ei_array.receiverFieldPath != ReceiverFieldPath::None) {
				ei_element.receiverFieldPath = ReceiverFieldPath::Indirect;
			}
			ei_element.fallsThrough &= ei_array.fallsThrough;
			return ei_element;
		}
		std::optional<BodyChecker>        opt_unreachable;
		const std::optional<ResolvedType> opt_index = onPath(ei_array.fallsThrough, opt_unreachable).scalar(*n->index);
		if ((opt_index && !opt_index->integer()) || !array.array()) {
			fail(Code::INVALID_INDEX, node.get_span(), "Indexing requires an array and an integer index");
		}
		ExpressionInfo ei_element = value(array.element(), nullptr, ei_array.receiverFieldPath);
		if (!ei_array.arrayShapes.empty() && hasArrayShape(array.element())) ei_element.arrayShapes = { ei_array.arrayShapes.front().element() };
		ei_element.fallsThrough = ei_array.fallsThrough && opt_index.has_value();
		return ei_element;
	}
	if (const auto *n = dynamic_cast<const UnaryExpression *>(&node)) {
		const Operator o_operation = n->op.get_operator();
		if (const IntegerLiteral *literal = dynamic_cast<const IntegerLiteral *>(n->operand.get()); literal && o_operation == oMinus) {
			const ExpressionInfo result = value(literalInteger(*literal, hint, true, analyzer.program.layout, b_inferenceDefaults));
			if (ExpressionFacts *ef_numeric = record ? &bf_facts.expressions : ef_probeConstants) {
				ExpressionInfo &ei_literal = ef_numeric->insert_or_assign(literal, result).first->second;
				ei_literal.numericConstant = IntegerConstant{ literal->value };
			}
			return result;
		}
		const ExpressionInfo ei_operand = o_operation == oNot ? scalarInfo(*n->operand) : scalarInfo(*n->operand, hint, floats);
		if (!ei_operand.fallsThrough) return ExpressionInfo::never();
		const ResolvedType    &operand = ei_operand.types.front();
		std::vector<Candidate> group   = analyzer.members(operand, n->op.get_lexeme(), SymbolKind::Operator);
		std::erase_if(group, [&] (const Candidate &candidate) {
			const MethodDeclaration &md_method = dynamic_cast<const MethodDeclaration &>(*callable(*candidate.symbol));
			return md_method.operatorMember->selector || md_method.operatorMember->provider || !md_method.parameters.empty();
		});
		if (!group.empty()) {
			return callOperator(group.front(), *n->operand, operand, n->op, node.get_span());
		}
		if ((o_operation == oNot && operand != ResolvedType(kBoolean)) || (o_operation != oNot && !analyzer.numeric(operand))) {
			fail(Code::UNARY_OPERAND, node.get_span(), "Invalid unary operand");
		}
		if (o_operation == oNot) {
			TestPaths tp_paths = splitTest(*n->operand, ei_operand);
			tp_test            = { std::move(tp_paths.no), std::move(tp_paths.yes) };
			e_test             = n;
		}
		return value(operand);
	}
	if (const auto *n = dynamic_cast<const FieldAccessExpression *>(&node)) {
		return field(*n);
	}
	if (const auto *n = dynamic_cast<const CallExpression *>(&node)) {
		return call(*n);
	}
	if (const MemberCallExpression *mc_method = dynamic_cast<const MemberCallExpression *>(&node)) {
		if (!record && !ef_probeConstants && checksNumericMethod(mc_method->name.get_lexeme())) {
			// A numeric method can reject its own operands before an enclosing overload is selected.
			ExpressionFacts ef_constants;
			BodyChecker     bc_probe   = *this;
			bc_probe.ef_probeConstants = &ef_constants;
			ExpressionInfo ei_call     = bc_probe.memberCall(*mc_method);
			fs_flow                    = std::move(bc_probe.fs_flow);
			return ei_call;
		}
		return memberCall(*mc_method);
	}
	if (const auto *n = dynamic_cast<const ClassCreationExpression *>(&node)) {
		return create(*n);
	}
	if (const auto *n = dynamic_cast<const ClassDeletionExpression *>(&node)) {
		checkValidatorReceiver(n->receiver.get(), node.get_span());
		const std::optional<ResolvedType> opt_receiver = scalar(*n->receiver);
		if (!opt_receiver) return ExpressionInfo::never();
		if (readOnlyDefaultRoot(*n->receiver))
			fail(Code::INVALID_PARAMETER_DEFAULT, n->get_span(), "A default cannot delete a read-only receiver", &pd_default->name);
		const ResolvedType &rt = *opt_receiver;
		if (!analyzer.referenceType(rt.nullable() ? rt.element() : rt)) {
			fail(Code::INVALID_CLASS_LIFECYCLE, node.get_span(), std::format("{} requires a class instance", to_string(kDelete)));
		}
		invalidateShared();
		const auto group = analyzer.members(rt.nullable() ? rt.element() : rt, std::format("{}{}", to_string(oTilde), rt.name), SymbolKind::Destructor);
		return group.empty() ? ExpressionInfo{} : invoke(n->keyword, node.get_span(), {}, group);
	}
	if (const auto *n = dynamic_cast<const CastExpression *>(&node)) {
		return cast(*n);
	}
	if (const auto *n = dynamic_cast<const TypeCheckExpression *>(&node)) {
		return typeCheck(*n);
	}
	if (const ElvisExpression *ee_fallback = dynamic_cast<const ElvisExpression *>(&node)) {
		if (const std::optional<ExpressionInfo> opt_call =
		            binaryOperator({ ee_fallback->left, ee_fallback->op, ee_fallback->fallback }, hint, true, floats))
			return *opt_call;
		return elvis(*ee_fallback, expected, floats);
	}
	if (const auto *n = dynamic_cast<const GuardedReceiver *>(&node)) {
		if (!gp_payload || &gp_payload->node != n) fail(Code::INTERNAL, node.get_span(), "Guarded payload outside its access expression");
		return gp_payload->value;
	}
	if (const auto *n = dynamic_cast<const GuardedAccessExpression *>(&node)) {
		const ExpressionInfo ei_source = scalarInfo(*n->get_receiver());
		GuardedPayload       gp_present{ *n->payload, ExpressionInfo::never() };
		if (ei_source.types.size() == 1) {
			ResolvedType rt_source = ei_source.types.front();
			if (!rt_source.nullable()) {
				const std::optional<TypedPlace> tp_place = locate(*n->get_receiver());
				if (tp_place && tp_place->type.nullable()) rt_source = tp_place->type;
			}
			if (!rt_source.nullable()) fail(Code::INVALID_GUARDED_RECEIVER, n->op.get_span(), "Guarded access requires a nullable receiver");
			gp_present.value = value(ei_source.types.front().nullable() ? rt_source.element() : ei_source.types.front());
			gp_present.value.representedType = ei_source.representedType;
		}
		const bool  b_present    = ei_source.fallsThrough && !ei_source.types.front().nullable();
		BodyChecker bc_present   = *this;
		bc_present.fs_flow.exits = {};
		bc_present.assumePresent(*n->get_receiver());
		if (!ei_source.fallsThrough) bc_present.we_capture = nullptr;
		bc_present.gp_payload = &gp_present;
		ExpressionInfo result = bc_present.expression(*n->continuation, expected, discard);
		if (result.types.size() > 1) fail(Code::MULTIPLE_RESULTS_IN_SCALAR, n->continuation->get_span(), "Guarded access requires at most one result");
		if (ei_source.fallsThrough) {
			mergeExits(fs_flow.exits, bc_present.fs_flow.exits);
			if (b_present)
				fs_flow.narrowed = std::move(bc_present.fs_flow.narrowed);
			else if (result.fallsThrough)
				intersect(fs_flow.narrowed, bc_present.fs_flow.narrowed);
		}
		result.fallsThrough = ei_source.fallsThrough && (!b_present || result.fallsThrough);
		result.representedType.reset();
		for (ResolvedType &rt : result.types) {
			if (!rt.nullable()) {
				rt.suffixes.emplace_back(to_string(oQuestion));
			}
		}
		return result;
	}
	if (const auto *n = dynamic_cast<const FunctionBlockExpression *>(&node)) {
		if (n->declaration.returnTypes.empty())
			fail(Code::BLOCK_RETURN_TYPE_REQUIRED, n->declaration.name.get_span(), "A deferred binding requires a result type");
		BodyChecker inner       = *this;
		inner.t_deferredBinding = &n->declaration.name;
		inner.returns.clear();
		for (const TypeReference &ref : n->declaration.returnTypes) {
			ResolvedType rt_type = bodyType(ref);
			if (rt_type != ResolvedType(kVoid)) inner.returns.push_back(std::move(rt_type));
		}
		inner.scopes.emplace_back();
		inner.lt_loop       = nullptr;
		inner.fs_flow.exits = {};
		std::erase_if(inner.fs_flow.narrowed, [] (const Narrowing::value_type &entry) { return entry.first.fact != PlaceFact::RepresentedType; });
		inner.forgetMutableMetadata(inner.fs_flow.narrowed);
		std::shared_ptr<WriteEffects> we_writes = std::make_shared<WriteEffects>();
		inner.we_capture                        = we_writes.get();
		inner.checkBody(n->declaration.body);
		we_deferred = std::move(we_writes);
		return { inner.returns };
	}
	if (const auto *n = dynamic_cast<const ProviderAccessExpression *>(&node)) return providerAccess(*n);
	if (const auto *n = dynamic_cast<const StateCallExpression *>(&node)) {
		const std::optional<ResolvedType> opt_receiver = scalar(*n->receiver);
		if (!opt_receiver) {
			std::optional<BodyChecker> opt_unreachable;
			for (const CallArgument &ca_argument : n->suppliedArguments) onPath(false, opt_unreachable).scalar(*ca_argument.expression);
			return ExpressionInfo::never();
		}
		if (readOnlyDefaultRoot(*n->receiver))
			fail(Code::INVALID_PARAMETER_DEFAULT, n->name.get_span(), "A default cannot call a state method on a read-only receiver",
			     &pd_default->name);
		if (opt_receiver->nullable()) fail(Code::NULLABLE_PAYLOAD_ACCESS, n->name.get_span(), "A state call requires a non-null receiver");
		const ResolvedType          &rt_host   = rt_attachment && isSelfReference(*n->receiver) ? *rt_attachment : *opt_receiver;
		const std::vector<Candidate> v_methods = analyzer.members(rt_host, n->name.get_lexeme(), SymbolKind::StateMethodContract);
		if (v_methods.empty()) fail(Code::INVALID_STATE_TARGET, n->name.get_span(), "Unknown state method '" + n->name.get_lexeme() + "'");
		return invoke(n->name, node.get_span(), n->suppliedArguments, v_methods, n->typeArguments ? &*n->typeArguments : nullptr);
	}
	if (const auto *n = dynamic_cast<const StateTransitionExpression *>(&node)) {
		const std::optional<ResolvedType> opt_receiver = scalar(*n->receiver);
		if (!opt_receiver) {
			std::optional<BodyChecker> opt_unreachable;
			if (n->payload) onPath(false, opt_unreachable).scalar(*n->payload);
			return ExpressionInfo::never();
		}
		if (readOnlyDefaultRoot(*n->receiver))
			fail(Code::INVALID_PARAMETER_DEFAULT, n->get_span(), "A default cannot change the state of a read-only receiver", &pd_default->name);
		const std::optional<Candidate> state = stateTarget(*opt_receiver, n->target, *n->receiver);
		if (!state) {
			fail(Code::INVALID_STATE_TARGET, n->target.get_span(), "Unknown state '" + n->target.get_lexeme() + "'");
		}
		return transition(*state, n->payload, node.get_span());
	}
	fail(Code::TYPE_RECEIVER_VALUE, node.get_span(), "Expected a value expression");
}
}
