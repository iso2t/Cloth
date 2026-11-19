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

bool isSelfReference (const Expression &node) {
	const NameExpression *ne_name = dynamic_cast<const NameExpression *>(&node);
	return ne_name && ne_name->name.get_lexeme() == to_string(kSelf);
}

ResolvedType literalInteger (const IntegerLiteral &literal, const ResolvedType *expected, bool negative) {
	const ResolvedType  rt         = expected && expected->integer() ? *expected : ResolvedType(kInteger);
	const bool          isUnsigned = rt.name == unsignedIntegerSpelling() || rt.name == to_string(kUsize);
	const unsigned      width      = rt.width ? rt.width : sizeof(void *) * 8;
	const IntegerValue &words      = literal.value;
	const std::size_t   bits       = (words.size() - 1) * 32 + std::bit_width(words.back());
	const bool          minimum    = negative && !isUnsigned && bits == width && std::has_single_bit(words.back()) &&
	                     std::all_of(words.begin(), words.end() - 1, [] (std::uint32_t word) { return word == 0; });
	if ((negative && isUnsigned && bits) || (bits > width - (isUnsigned ? 0 : 1) && !minimum)) {
		fail(Code::INTEGER_RANGE, literal.get_span(), "Integer literal is outside " + rt.spelling());
	}
	return rt;
}

bool numeric (const ResolvedType &t) {
	return t.integer() || t.floating();
}

bool numericLiteral (const Expression &expr) {
	if (dynamic_cast<const NumberLiteral *>(&expr)) {
		return true;
	}
	const auto *u = dynamic_cast<const UnaryExpression *>(&expr);
	return u && (u->op.get_operator() == oPlus || u->op.get_operator() == oMinus) && numericLiteral(*u->operand);
}

}

ExpressionInfo BodyChecker::expression (const Expression &node, const ResultTypes *expected, bool discard) {
	ExpressionInfo info = infer(node, expected, discard);
	if (record) {
		ExpressionInfo &ei_fact = bf_facts.expressions.insert_or_assign(&node, info).first->second;
		// A guarded chain can still need the declared type on its absent path. The exiting expression itself produces no values.
		if (!ei_fact.fallsThrough) ei_fact.types.clear();
	}
	return info;
}

std::optional<ResolvedType> BodyChecker::scalar (const Expression &node, const ResolvedType *expected) {
	ExpressionInfo ei_value = scalarInfo(node, expected);
	if (!ei_value.fallsThrough) return std::nullopt;
	return std::move(ei_value.types.front());
}

ExpressionInfo BodyChecker::scalarInfo (const Expression &node, const ResolvedType *expected) {
	ResultTypes types;
	if (expected) {
		types.push_back(*expected);
	}
	const ExpressionInfo result = expression(node, expected ? &types : nullptr);
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

ExpressionInfo BodyChecker::name (const Token &token) {
	const std::string &spelling = token.get_lexeme();
	if (const Binding *binding = local(spelling)) {
		remember(token, *binding->name);
		if (!binding->type) return { .types = {}, .binding = binding->name, .fallsThrough = false };
		const auto found = fs_flow.narrowed.find(binding->name);
		return { { found == fs_flow.narrowed.end() ? *binding->type : found->second }, nullptr, binding->name };
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
		return value(rt_payload, owner);
	}
	if (hasInstance()) {
		const std::vector<Candidate> fields = analyzer.members(selfType(), spelling, SymbolKind::Field);
		if (!fields.empty()) {
			const Candidate &field = fields.front();
			analyzer.requireAccess(*field.symbol, context, token.get_span());
			checkFieldRead(*field.symbol, token);
			remember(token, field.symbol->name);
			return value(analyzer.substitute(*analyzer.declarations.at(field.symbol).value, field.arguments), field.symbol,
			             ReceiverFieldPath::Stored);
		}
	}
	for (const Symbol *sym : analyzer.lookup(context, spelling)) {
		if (sym->kind == SymbolKind::Function) {
			fail(Code::FUNCTION_VALUE, token.get_span(), "'" + spelling + "' is a function", &sym->name);
		}
		if (sym->kind == SymbolKind::Type) {
			fail(Code::TYPE_RECEIVER_VALUE, token.get_span(), "'" + spelling + "' is a type", &sym->name);
		}
	}
	fail(Code::UNKNOWN_NAME, token.get_span(), "Unknown name '" + spelling + "'");
}

std::optional<BodyChecker::Receiver> BodyChecker::receiver (const Expression &node, bool allowOpen) {
	std::optional<ResolvedType> type;
	if (const TypeReceiverExpression *r = dynamic_cast<const TypeReceiverExpression *>(&node)) {
		type = bodyType(r->annotation, allowOpen);
	} else if (const std::optional<Token> path = typeNameCandidate(node, [&] (const Token &t) {
		           checkDefaultName(t);
		           if (local(t.get_lexeme()) || (hasInstance() && !analyzer.members(selfType(), t.get_lexeme(), SymbolKind::Field).empty())) {
			           return false;
		           }
		           return analyzer.genericParameter(context, t.get_lexeme()) ||
		                  std::ranges::any_of(analyzer.lookup(context, t.get_lexeme()), [] (const Symbol *s) { return s->kind == SymbolKind::Type; });
	           })) {
		try {
			type = bodyType(TypeReference(*path), allowOpen);
		} catch (const cde::ClothDiagnostic &error) {
			if (error.get_code() != Code::UNSUPPORTED_TYPE) {
				throw;
			}
		}
	}
	if (!type) {
		ExpressionInfo ei_value = scalarInfo(node);
		if (ei_value.types.size() != 1) return std::nullopt;
		return Receiver{ .type = std::move(ei_value.types.front()), .fieldPath = ei_value.receiverFieldPath, .fallsThrough = ei_value.fallsThrough };
	}
	if (record) {
		ExpressionInfo ei_type  = value(ResolvedType(kTypeinfo), type->declaration);
		ei_type.representedType = *type;
		bf_facts.expressions.insert_or_assign(&node, std::move(ei_type));
	}
	return Receiver{ .type = std::move(*type), .isType = true };
}

ExpressionInfo BodyChecker::field (const FieldAccessExpression &node) {
	const bool                    b_self       = isSelfReference(*node.receiver);
	const std::optional<Receiver> opt_receiver = b_self && rp_receiverPhase == ReceiverPhase::FieldInitializer ? Receiver{ recordSelf(*node.receiver) } :
	                                                                                                             receiver(*node.receiver);
	if (!opt_receiver) return ExpressionInfo::never();
	const Receiver     &r_receiver = *opt_receiver;
	const ResolvedType &rt         = r_receiver.type;
	if (rt.nullable()) {
		fail(Code::NULLABLE_PAYLOAD_ACCESS, node.get_span(), std::format("Use {} to access a nullable receiver", to_string(oSafeDot)));
	}
	const std::vector<Candidate> fields = analyzer.members(rt, node.name.get_lexeme(), r_receiver.isType ? SymbolKind::EnumCase : SymbolKind::Field);
	if (fields.empty()) {
		fail(Code::UNKNOWN_MEMBER, node.name.get_span(), "Unknown member '" + node.name.get_lexeme() + "' on " + rt.spelling());
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
	return { .types = {}, .symbol = state.symbol, .fallsThrough = statePayload(payload, required, span, state.symbol->name) };
}

ExpressionInfo BodyChecker::typeCheck (const TypeCheckExpression &node) {
	const Token  *identifier = node.target.get_bare_identifier();
	ExpressionPtr valueTarget;
	if (node.valueCandidate) {
		const std::optional<Receiver> opt_target = receiver(*node.valueCandidate);
		if (!opt_target || !opt_target->isType) {
			valueTarget = node.valueCandidate;
		}
	} else if (identifier &&
	           (local(identifier->get_lexeme()) || (hasInstance() && !analyzer.members(selfType(), identifier->get_lexeme(), SymbolKind::Field).empty()))) {
		valueTarget = std::make_shared<NameExpression>(*identifier);
	}
	if (valueTarget) {
		std::shared_ptr<const BinaryExpression> test = std::make_shared<BinaryExpression>(node.operand, node.op, valueTarget);
		if (record) {
			test = bf_facts.valueTests.try_emplace(&node, test).first->second;
		}
		return expression(*test);
	}
	const bool b_stateName = identifier && !analyzer.genericParameter(context, identifier->get_lexeme()) &&
	                         !std::ranges::any_of(analyzer.lookup(context, identifier->get_lexeme()),
	                                              [] (const Symbol *symbol) { return symbol->kind == SymbolKind::Type; });
	if (b_stateName && rp_receiverPhase == ReceiverPhase::FieldInitializer && isSelfReference(*node.operand)) {
		if (const std::optional<Candidate> state = stateTarget(selfType(), *identifier, *node.operand)) {
			recordSelf(*node.operand);
			return value(ResolvedType(kBoolean), state->symbol);
		}
	}
	const std::optional<ResolvedType> opt_source = scalar(*node.operand);
	if (b_stateName && opt_source) {
		if (const std::optional<Candidate> state = stateTarget(*opt_source, *identifier, *node.operand)) {
			return value(ResolvedType(kBoolean), state->symbol);
		}
	}
	if (opt_source && opt_source->name == to_string(kNull)) {
		fail(Code::EXPLICIT_NULLABLE_TYPE, node.operand->get_span(), "A type test needs a typed operand");
	}
	const ResolvedType target = bodyType(node.target);
	return opt_source ? value(ResolvedType(kBoolean), target.declaration) : ExpressionInfo::never();
}

ExpressionInfo BodyChecker::binary (const BinaryExpression &node, const ResolvedType *expected) {
	const Operator o_operation = node.op.get_operator();
	if (o_operation == oAnd || o_operation == oOr) {
		const bool  b_left = expect(*node.left, ResolvedType(kBoolean));
		BodyChecker right  = *this;
		right.refine(*node.left, o_operation == oAnd);
		const bool            b_right         = right.expect(*node.right, ResolvedType(kBoolean));
		const BooleanLiteral *bl_left         = dynamic_cast<const BooleanLiteral *>(node.left.get());
		const bool            b_evaluateRight = !bl_left || bl_left->value == (o_operation == oAnd);
		if (b_left && b_evaluateRight) {
			fs_flow.loopMayBreak |= right.fs_flow.loopMayBreak;
			fs_flow.loopMayContinue |= right.fs_flow.loopMayContinue;
		}
		if (!b_left || (bl_left && b_evaluateRight && !b_right)) return ExpressionInfo::never();
		return value(ResolvedType(kBoolean));
	}
	const bool                  compare = isComparison(o_operation);
	std::optional<ResolvedType> opt_left, opt_right;
	if (numericLiteral(*node.left) && !numericLiteral(*node.right)) {
		opt_right = scalar(*node.right);
		opt_left  = scalar(*node.left, opt_right ? &*opt_right : nullptr);
	} else {
		opt_left  = scalar(*node.left, !compare && expected && numeric(*expected) ? expected : nullptr);
		opt_right = scalar(*node.right, opt_left ? &*opt_left : nullptr);
	}
	if (!opt_left || !opt_right) return ExpressionInfo::never();
	const ResolvedType    &left = *opt_left, &right = *opt_right;
	std::vector<Candidate> sides[2];
	for (unsigned side = 0; side < 2; ++side) {
		const ResolvedType &peer = side ? left : right;
		for (const Candidate &c : analyzer.members(side ? right : left, node.op.get_lexeme(), SymbolKind::Operator)) {
			const OperatorMember &m = *dynamic_cast<const MethodDeclaration &>(*callable(*c.symbol)).operatorMember;
			if (m.provider || (!m.selector && m.rightReceiver != static_cast<bool>(side))) {
				continue;
			}
			if (m.selector) {
				ResolvedType selector = analyzer.substitute(*analyzer.declarations.at(c.symbol).value, c.arguments);
				if (m.selector->capturesWidth()) {
					selector.width = peer.width;
				}
				if (selector != peer) {
					continue;
				}
			} else if (callable(*c.symbol)->parameters.size() != 1) {
				continue;
			}
			sides[side].push_back(c);
		}
	}
	if (!sides[0].empty() || !sides[1].empty()) {
		// Probe each receiver direction independently so a nonmatching left overload cannot hide a right overload.
		struct OperatorMatch {
			std::vector<Candidate>    candidates;
			std::vector<CallArgument> args;
			ResultTypes               supplied;
			bool                      projection;
			unsigned                  side;
		};

		std::vector<OperatorMatch> matches;
		const Symbol              *privateMatch = nullptr;
		for (unsigned side = 0; side < 2; ++side) {
			for (bool projection : { false, true }) {
				std::vector<Candidate> group = sides[side];
				std::erase_if(group, [&] (const Candidate &c) { return analyzer.declarations.at(c.symbol).value.has_value() != projection; });
				if (group.empty()) {
					continue;
				}
				std::vector<CallArgument> args;
				ResultTypes               supplied;
				if (!projection) {
					args.push_back({ side ? node.left : node.right });
					supplied.push_back(side ? left : right);
				}
				BodyChecker trial = *this;
				trial.record      = false;
				try {
					trial.invoke(node.op, node.get_span(), args, group, nullptr, &supplied);
				} catch (const cde::ClothDiagnostic &error) {
					if (error.get_code() == Code::PRIVATE_CLASS_MEMBER) {
						privateMatch = group.front().symbol;
						continue;
					}
					if (error.get_code() == Code::NO_MATCHING_CALL || error.get_code() == Code::CALL_ARITY) {
						continue;
					}
					throw;
				}
				matches.push_back({ std::move(group), std::move(args), std::move(supplied), projection, side });
			}
		}
		if (matches.empty() && privateMatch) {
			fail(Code::PRIVATE_CLASS_MEMBER, node.op.get_span(), "Operator is private", &privateMatch->name);
		}
		if (matches.size() != 1) {
			fail(Code::OPERATOR_MATCH, node.op.get_span(), matches.empty() ? "No matching operator" : "Ambiguous operator");
		}
		const OperatorMatch &m        = matches.front();
		ExpressionInfo       selected = invoke(node.op, node.get_span(), m.args, m.candidates, nullptr, &m.supplied);
		if (m.projection && selected.types.size() == 1) {
			const ResolvedType &peer = m.side ? left : right;
			if (!selected.types[0].width && numeric(selected.types[0])) {
				selected.types[0].width = peer.width;
			}
			if (!numeric(selected.types[0]) || !numeric(peer)) {
				fail(Code::BINARY_OPERANDS, node.op.get_span(), "Operator projection requires numeric operands");
			}
			if (compare) {
				selected.types = { ResolvedType(kBoolean) };
			}
		}
		return selected;
	}
	if (node.op.get_lexeme() == to_string(kIs)) {
		if (right.name != "range" || right.declaration || right.arguments().size() != 1 || !analyzer.accepts(right.arguments().front(), left)) {
			fail(Code::INVALID_RANGE_MEMBERSHIP, node.get_span(), "Membership requires a range with compatible endpoints");
		}
		return value(ResolvedType(kBoolean));
	}
	if (isEquality(o_operation)) {
		const bool b_unproven = left.name != to_string(kNull) && right.name != to_string(kNull) &&
		                        (!analyzer.supportsEquality(left) || !analyzer.supportsEquality(right));
		if (b_unproven || (!analyzer.accepts(left, right) && !analyzer.accepts(right, left) &&
		                   !(numeric(left) && numeric(right) && (left.floating() || right.floating())))) {
			fail(Code::INCOMPARABLE_TYPES, node.op.get_span(), "Cannot compare " + left.spelling() + " with " + right.spelling());
		}
		return value(ResolvedType(kBoolean));
	}
	if (o_operation == oPlus && left == ResolvedType(kString) && right == left) {
		return value(left);
	}
	if (!numeric(left) || !numeric(right)) {
		fail(Code::BINARY_OPERANDS, node.op.get_span(), std::format("Operator '{}' requires numeric operands", node.op.get_lexeme()));
	}
	if (left.integer() && right.integer() && left != right) {
		fail(Code::MIXED_INTEGERS, node.op.get_span(), "Integer operand types must match");
	}
	if (compare) {
		return value(ResolvedType(kBoolean));
	}
	if (o_operation == oRange || o_operation == oRangeInclusive) {
		if (!left.integer() || left != right) {
			fail(Code::INVALID_RANGE_VALUE, node.get_span(), "Range endpoints must have matching integer types");
		}
		ResolvedType range("range");
		analyzer.applyArguments(range, { left });
		return value(range);
	}
	if (left.floating() || right.floating()) {
		ResolvedType result(kFloat);
		result.width = std::max(left.floating() ? left.width : 32u, right.floating() ? right.width : 32u);
		return value(result);
	}
	return value(left);
}

ExpressionInfo BodyChecker::call (const CallExpression &node) {
	checkDefaultName(node.name);
	if (const Binding *b = local(node.name.get_lexeme())) {
		if (!b->deferred || !node.suppliedArguments.empty() || node.typeArguments) {
			fail(Code::NOT_CALLABLE, node.name.get_span(), "Local binding is not callable with these arguments", b->name);
		}
		return { { *b->type }, nullptr, b->name };
	}
	std::vector<Candidate> group;
	if (owner && owner->kind == SymbolKind::State) {
		group = analyzer.members(analyzer.substitute(analyzer.ownerType(*owner), m_arguments), node.name.get_lexeme(), SymbolKind::Method,
		                         MemberLookup::CallContract);
		for (Candidate &c_method : group) c_method.arguments.insert(m_arguments.begin(), m_arguments.end());
	}
	if (hasInstance() && group.empty()) {
		group = analyzer.members(selfType(), node.name.get_lexeme(), SymbolKind::Method, MemberLookup::CallContract);
	}
	if (!group.empty() && rp_receiverPhase == ReceiverPhase::FieldInitializer) {
		fail(Code::PARTIAL_CLASS_INSTANCE, node.name.get_span(), std::format("Methods on '{}' require a fully initialized instance", to_string(kSelf)));
	}
	if (!group.empty() && pd_default) {
		fail(Code::INVALID_PARAMETER_DEFAULT, node.name.get_span(), "A default cannot call a method on its receiver", &pd_default->name);
	}
	if (group.empty()) {
		const Candidates candidates = hasInstance() ? analyzer.lookup(context, node.name.get_lexeme()) :
		                                              analyzer.lookupFile(context, node.name.get_lexeme());
		for (const Symbol *sym : candidates) {
			if (sym->kind == SymbolKind::Function) {
				group.push_back({ sym });
			}
		}
		if (group.empty() && !candidates.empty()) {
			fail(Code::NOT_CALLABLE, node.name.get_span(), "'" + node.name.get_lexeme() + "' is not callable", &candidates.front()->name);
		}
	}
	return invoke(node.name, node.get_span(), node.suppliedArguments, group, node.typeArguments ? &*node.typeArguments : nullptr);
}

ExpressionInfo BodyChecker::create (const ClassCreationExpression &node, const ResolvedType *hint) {
	std::optional<Receiver> opt_receiver = receiver(*node.receiver, true);
	if (!opt_receiver || !opt_receiver->isType) {
		fail(Code::INVALID_CLASS_LIFECYCLE, node.get_span(), std::format("{} requires a class type or a constructor bound", to_string(kNew)));
	}
	ResolvedType &rt = opt_receiver->type;
	if ((!rt.declaration && !rt.parameter) || !rt.suffixes.empty()) {
		fail(Code::INVALID_CLASS_LIFECYCLE, node.get_span(), std::format("{} requires a class type or a constructor bound", to_string(kNew)));
	}
	// The guarantee owns its call syntax, even if the concrete constructor uses different labels or defaults.
	const Symbol *sym_guarantee = nullptr;
	Substitutions m_guaranteeArguments;
	if (!m_arguments.empty()) {
		const BodyFacts                                                             &bf_open = analyzer.openBody(rt_attachment);
		const std::unordered_map<const Expression *, ExpressionInfo>::const_iterator it_open = bf_open.expressions.find(&node);
		if (it_open != bf_open.expressions.end() && it_open->second.argumentContract &&
		    dynamic_cast<const ConstructorGuaranteeDeclaration *>(callable(*it_open->second.argumentContract))) {
			sym_guarantee = it_open->second.argumentContract;
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
	if (params && !sym_guarantee) {
		for (Candidate &candidate : constructors) {
			for (std::size_t i = 0; i < params->parameters.size(); ++i) {
				const TypeParameterDeclaration &param = params->parameters[i];
				if (!annotation || i >= annotation->annotation.get_type_arguments().size() ||
				    annotation->annotation.get_type_arguments()[i].is_placeholder()) {
					if (hint && hint->declaration == rt.declaration) {
						candidate.arguments[&param] = hint->arguments()[i];
					} else {
						candidate.arguments.erase(&param);
					}
					candidate.inferOwner = params;
				}
			}
		}
	}
	ExpressionInfo result = invoke(node.name, node.get_span(), node.suppliedArguments, constructors);
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
	const std::optional<ResolvedType> opt_from = scalar(*node.operand);
	if (const Token *id = node.target.get_bare_identifier();
	    opt_from && id && !analyzer.genericParameter(context, id->get_lexeme()) &&
	    !std::ranges::any_of(analyzer.lookup(context, id->get_lexeme()), [] (const Symbol *symbol) { return symbol->kind == SymbolKind::Type; })) {
		if (const std::optional<Candidate> state = stateTarget(*opt_from, *id, *node.operand)) {
			if (readOnlyDefaultRoot(*node.operand))
				fail(Code::INVALID_PARAMETER_DEFAULT, node.get_span(), "A default cannot change the state of a read-only receiver",
				     &pd_default->name);
			return transition(*state, nullptr, node.get_span());
		}
	}
	const ResolvedType to = bodyType(node.target);
	if (!opt_from) return ExpressionInfo::never();
	const ResolvedType &from = *opt_from;
	if ((from.nullable() || from.name == to_string(kNull)) && !to.nullable()) {
		fail(Code::NULLABLE_PAYLOAD_ACCESS, node.get_span(), "A nullable value needs a null check before casting");
	}
	const ResolvedType source = from.nullable() ? from.element() : from, target = to.nullable() ? to.element() : to;
	const bool ordinal = source.declaration && dynamic_cast<const EnumDeclaration *>(std::get<const TypeDeclaration *>(source.declaration->declaration)) &&
	                     target == ResolvedType(kInteger);
	if (!(numeric(source) && numeric(target)) && !ordinal && !analyzer.accepts(to, from)) {
		fail(Code::INVALID_CAST, node.get_span(), "Cannot cast " + from.spelling() + " to " + to.spelling());
	}
	return value(to);
}

ExpressionInfo BodyChecker::infer (const Expression &node, const ResultTypes *expected, bool discard) {
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
		return value(literalInteger(*n, hint, false));
	}
	if (const auto *n = dynamic_cast<const FloatingPointLiteral *>(&node)) {
		ResolvedType      rt      = hint && hint->floating() ? *hint : ResolvedType(kFloat);
		const long double literal = std::strtold(n->value.c_str(), nullptr);
		const long double limit   = rt.width == 16 ? 65520.L :
		                            rt.width == 32 ? std::ldexp(1.L, 128) - std::ldexp(1.L, 103) :
		                                             std::ldexp(1.L, 1024) - std::ldexp(1.L, 970);
		if (!std::isfinite(literal) || std::fabs(literal) >= limit) {
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
		return value(ResolvedType(kNull));
	}
	if (const auto *n = dynamic_cast<const InterpolatedString *>(&node)) {
		bool b_continues = true;
		for (const ExpressionPtr &part : n->parts) {
			b_continues &= scalar(*part).has_value();
		}
		return b_continues ? value(ResolvedType(kString)) : ExpressionInfo::never();
	}
	if (const auto *n = dynamic_cast<const ResultListExpression *>(&node)) {
		ResultTypes                v_values;
		std::optional<BodyChecker> opt_unreachable;
		for (std::size_t st_slot = 0; st_slot < n->values.size(); ++st_slot) {
			BodyChecker &bc_operand = onPath(v_values.size() == st_slot, opt_unreachable);
			if (std::optional<ResolvedType> opt_value =
			            bc_operand.scalar(*n->values[st_slot], expected && st_slot < expected->size() ? &(*expected)[st_slot] : nullptr)) {
				v_values.push_back(std::move(*opt_value));
			}
		}
		return v_values.size() == n->values.size() ? ExpressionInfo{ std::move(v_values) } : ExpressionInfo::never();
	}
	if (const auto *n = dynamic_cast<const ArrayLiteral *>(&node)) {
		std::optional<ResolvedType> elem;
		if (hint && hint->array()) {
			elem = hint->element();
		}
		if (!elem) {
			BodyChecker bc_probe = *this;
			bc_probe.record      = false;
			for (const ExpressionPtr &ep_part : n->elements) {
				if (!numericLiteral(*ep_part) && !dynamic_cast<const NullLiteral *>(ep_part.get())) elem = bc_probe.scalar(*ep_part);
				if (elem) break;
			}
			if (!elem) {
				for (const ExpressionPtr &ep_part : n->elements) {
					elem = bc_probe.scalar(*ep_part);
					if (elem) break;
				}
			}
		}
		if ((n->elements.empty() && !elem) || (elem && elem->name == to_string(kNull))) {
			fail(Code::INVALID_ARRAY, node.get_span(), "Array literal needs an element type");
		}
		bool b_continues = true;
		for (const ExpressionPtr &part : n->elements) {
			b_continues &= elem ? expect(*part, *elem) : scalar(*part).has_value();
		}
		if (!b_continues || !elem) return ExpressionInfo::never();
		elem->suffixes.push_back(arraySuffix(std::to_string(n->elements.size())));
		return value(*elem);
	}
	if (const auto *n = dynamic_cast<const StructLiteral *>(&node)) {
		const StructDeclaration *td =
		        hint && hint->declaration && hint->declaration->kind == SymbolKind::Type && hint->suffixes.empty() ?
		                dynamic_cast<const StructDeclaration *>(std::get<const TypeDeclaration *>(hint->declaration->declaration)) :
		                nullptr;
		if (!td || td->fields.size() != n->fields.size()) {
			fail(Code::INVALID_STRUCT_LITERAL, node.get_span(), "Struct literal needs a matching struct type and field count");
		}
		bool b_continues = true;
		for (std::size_t i = 0; i < n->fields.size(); ++i) {
			const Candidate member = analyzer.members(*hint, td->fields[i].name.get_lexeme(), SymbolKind::Field).front();
			analyzer.requireAccess(*member.symbol, context, n->fields[i]->get_span());
			b_continues &= expect(*n->fields[i], analyzer.substitute(*analyzer.declarations.at(member.symbol).value, member.arguments));
		}
		return b_continues ? value(*hint) : ExpressionInfo::never();
	}
	if (const auto *n = dynamic_cast<const IndexExpression *>(&node)) {
		const ExpressionInfo ei_array = scalarInfo(*n->array);
		if (ei_array.types.size() != 1) {
			scalar(*n->index);
			return ExpressionInfo::never();
		}
		const ResolvedType &array = ei_array.types.front();
		if (array.declaration && !array.array()) {
			const std::vector<Candidate> v_getters = analyzer.members(array, to_string(oIndexRead), SymbolKind::Operator);
			ExpressionInfo ei_element = invoke(v_getters.empty() ? array.declaration->name : v_getters.front().symbol->name, node.get_span(),
			                                   { { n->index } }, v_getters);
			if (ei_array.receiverFieldPath != ReceiverFieldPath::None) {
				ei_element.receiverFieldPath = ReceiverFieldPath::Indirect;
			}
			ei_element.fallsThrough &= ei_array.fallsThrough;
			return ei_element;
		}
		const std::optional<ResolvedType> opt_index = scalar(*n->index);
		if ((opt_index && !opt_index->integer()) || !array.array()) {
			fail(Code::INVALID_INDEX, node.get_span(), "Indexing requires an array and an integer index");
		}
		ExpressionInfo ei_element = value(array.element(), nullptr, ei_array.receiverFieldPath);
		ei_element.fallsThrough   = ei_array.fallsThrough && opt_index.has_value();
		return ei_element;
	}
	if (const auto *n = dynamic_cast<const UnaryExpression *>(&node)) {
		const Operator o_operation = n->op.get_operator();
		if (const IntegerLiteral *literal = dynamic_cast<const IntegerLiteral *>(n->operand.get()); literal && o_operation == oMinus) {
			const ExpressionInfo result = value(literalInteger(*literal, hint, true));
			if (record) {
				bf_facts.expressions[literal] = result;
			}
			return result;
		}
		const std::optional<ResolvedType> opt_operand = scalar(*n->operand, hint);
		if (!opt_operand) return ExpressionInfo::never();
		const ResolvedType &operand = *opt_operand;
		const auto          group   = analyzer.members(operand, n->op.get_lexeme(), SymbolKind::Operator);
		if (!group.empty()) {
			return invoke(n->op, node.get_span(), {}, group);
		}
		if ((o_operation == oNot && operand != ResolvedType(kBoolean)) || (o_operation != oNot && !numeric(operand))) {
			fail(Code::UNARY_OPERAND, node.get_span(), "Invalid unary operand");
		}
		return value(operand);
	}
	if (const auto *n = dynamic_cast<const BinaryExpression *>(&node)) {
		return binary(*n, hint);
	}
	if (const auto *n = dynamic_cast<const FieldAccessExpression *>(&node)) {
		return field(*n);
	}
	if (const auto *n = dynamic_cast<const CallExpression *>(&node)) {
		return call(*n);
	}
	if (const auto *n = dynamic_cast<const MemberCallExpression *>(&node)) {
		const ExpressionInfo ei_receiver = scalarInfo(*n->receiver);
		if (ei_receiver.types.size() != 1) {
			for (const CallArgument &ca_argument : n->suppliedArguments) scalar(*ca_argument.expression);
			return ExpressionInfo::never();
		}
		if (readOnlyDefaultRoot(*n->receiver)) {
			fail(Code::INVALID_PARAMETER_DEFAULT, n->name.get_span(), "A default cannot call a method on a read-only receiver", &pd_default->name);
		}
		const ResolvedType &rt = ei_receiver.types.front();
		if (rt.nullable()) {
			fail(Code::NULLABLE_PAYLOAD_ACCESS, node.get_span(),
			     std::format("Use {} to call a member of a nullable receiver", to_string(oSafeDot)));
		}
		const std::vector<Candidate> v_methods = methodCandidates(*n, rt);
		if (v_methods.empty()) {
			fail(Code::UNKNOWN_MEMBER, n->name.get_span(), "Unknown method '" + n->name.get_lexeme() + "' on " + rt.spelling());
		}
		if (rp_receiverPhase == ReceiverPhase::FieldInitializer && ei_receiver.receiverFieldPath == ReceiverFieldPath::Stored && rt.declaration &&
		    dynamic_cast<const StructDeclaration *>(std::get<const TypeDeclaration *>(rt.declaration->declaration))) {
			fail(Code::PARTIAL_CLASS_INSTANCE, n->receiver->get_span(), "A method on a stored field requires a fully initialized instance");
		}
		ExpressionInfo ei_call = invoke(n->name, node.get_span(), n->suppliedArguments, v_methods, n->typeArguments ? &*n->typeArguments : nullptr);
		ei_call.fallsThrough &= ei_receiver.fallsThrough;
		return ei_call;
	}
	if (const auto *n = dynamic_cast<const ClassCreationExpression *>(&node)) {
		return create(*n, hint);
	}
	if (const auto *n = dynamic_cast<const ClassDeletionExpression *>(&node)) {
		const std::optional<ResolvedType> opt_receiver = scalar(*n->receiver);
		if (!opt_receiver) return ExpressionInfo::never();
		if (readOnlyDefaultRoot(*n->receiver))
			fail(Code::INVALID_PARAMETER_DEFAULT, n->get_span(), "A default cannot delete a read-only receiver", &pd_default->name);
		const ResolvedType &rt = *opt_receiver;
		if (!analyzer.referenceType(rt)) {
			fail(Code::INVALID_CLASS_LIFECYCLE, node.get_span(), std::format("{} requires a class instance", to_string(kDelete)));
		}
		const auto group = analyzer.members(rt, std::format("{}{}", to_string(oTilde), rt.name), SymbolKind::Destructor);
		return group.empty() ? ExpressionInfo{} : invoke(n->keyword, node.get_span(), {}, group);
	}
	if (const auto *n = dynamic_cast<const CastExpression *>(&node)) {
		return cast(*n);
	}
	if (const auto *n = dynamic_cast<const TypeCheckExpression *>(&node)) {
		return typeCheck(*n);
	}
	if (const auto *n = dynamic_cast<const ElvisExpression *>(&node)) {
		const std::optional<ResolvedType> opt_left = scalar(*n->left);
		if (!opt_left) {
			scalar(*n->fallback, hint);
			return ExpressionInfo::never();
		}
		const ResolvedType &rt = *opt_left;
		if (rt.name == to_string(kNull)) {
			return expression(*n->fallback, expected);
		}
		if (!rt.nullable()) {
			fail(Code::INVALID_ELVIS, n->op.get_span(), std::format("Left operand of {} must be nullable", to_string(oElvis)));
		}
		const ResolvedType                rt_element   = rt.element();
		const std::optional<ResolvedType> opt_fallback = scalar(*n->fallback, &rt_element);
		if (!opt_fallback || analyzer.accepts(rt_element, *opt_fallback)) {
			return value(rt_element);
		}
		if (analyzer.accepts(rt, *opt_fallback)) {
			return value(rt);
		}
		fail(Code::TYPE_MISMATCH, n->fallback->get_span(), "Incompatible fallback type");
	}
	if (const auto *n = dynamic_cast<const GuardedReceiver *>(&node)) {
		if (!gp_payload || &gp_payload->node != n) fail(Code::INTERNAL, node.get_span(), "Guarded payload outside its access expression");
		return gp_payload->value;
	}
	if (const auto *n = dynamic_cast<const GuardedAccessExpression *>(&node)) {
		const ExpressionInfo ei_source = scalarInfo(*n->get_receiver());
		GuardedPayload       gp_present{ *n->payload, ExpressionInfo::never() };
		if (ei_source.types.size() == 1) {
			const ResolvedType &rt_source = ei_source.types.front();
			if (!rt_source.nullable()) fail(Code::INVALID_GUARDED_RECEIVER, n->op.get_span(), "Guarded access requires a nullable receiver");
			gp_present.value = value(rt_source.element());
		}
		BodyChecker bc_present = *this;
		bc_present.gp_payload  = &gp_present;
		ExpressionInfo result  = bc_present.expression(*n->continuation, expected, discard);
		if (result.types.size() > 1) fail(Code::MULTIPLE_RESULTS_IN_SCALAR, n->continuation->get_span(), "Guarded access requires at most one result");
		if (ei_source.fallsThrough) {
			fs_flow.loopMayBreak |= bc_present.fs_flow.loopMayBreak;
			fs_flow.loopMayContinue |= bc_present.fs_flow.loopMayContinue;
			if (result.fallsThrough) {
				std::erase_if(fs_flow.narrowed, [&bc_present] (const Narrowing::value_type &fact) {
					const Narrowing::const_iterator it_fact = bc_present.fs_flow.narrowed.find(fact.first);
					return it_fact == bc_present.fs_flow.narrowed.end() || it_fact->second != fact.second;
				});
			}
		}
		result.fallsThrough = ei_source.fallsThrough;
		for (ResolvedType &rt : result.types) {
			if (!rt.nullable()) {
				rt.suffixes.emplace_back(to_string(oQuestion));
			}
		}
		return result;
	}
	if (const auto *n = dynamic_cast<const IfExpression *>(&node)) {
		return condition(*n, expected, discard);
	}
	if (const auto *n = dynamic_cast<const FunctionBlockExpression *>(&node)) {
		BodyChecker inner      = *this;
		inner.b_nestedFunction = true;
		inner.returns.clear();
		for (const TypeReference &ref : n->declaration.returnTypes) {
			inner.returns.push_back(bodyType(ref));
		}
		if (inner.returns.empty()) {
			fail(Code::BLOCK_RETURN_TYPE_REQUIRED, node.get_span(), "A deferred binding requires a result type");
		}
		inner.scopes.emplace_back();
		inner.loopDepth = 0;
		inner.checkBody(n->declaration.body);
		return { inner.returns };
	}
	if (const auto *n = dynamic_cast<const ProviderAccessExpression *>(&node)) {
		const std::optional<Receiver> opt_receiver = receiver(*n->receiver);
		if (!opt_receiver) return ExpressionInfo::never();
		const Receiver        &r_receiver  = *opt_receiver;
		const ResolvedType    &rt          = r_receiver.type;
		const std::string     &id          = n->name.get_lexeme();
		std::vector<Candidate> v_providers = analyzer.members(rt, id, SymbolKind::Operator);
		std::erase_if(v_providers,
		              [] (const Candidate &c) { return !dynamic_cast<const MethodDeclaration &>(*callable(*c.symbol)).operatorMember->provider; });
		if (!v_providers.empty()) {
			return invoke(n->name, node.get_span(), {}, v_providers);
		}
		if (id == provider::S_SIZE) {
			if (rt.nullable() && rt.element().array()) {
				fail(Code::NULLABLE_PAYLOAD_ACCESS, node.get_span(), "Array size requires a present array");
			}
			if (!rt.parameter || rt.array()) return value(ResolvedType(kUsize));
		}
		if (id == provider::S_TYPE) {
			ExpressionInfo ei_type  = value(ResolvedType(kTypeinfo));
			ei_type.representedType = rt;
			return ei_type;
		}
		if (id == provider::S_STRING && !r_receiver.isType && (!rt.parameter || analyzer.referenceType(rt))) {
			return value(ResolvedType(kString));
		}
		if ((id == provider::S_MIN || id == provider::S_MAX) && numeric(rt)) {
			return value(rt);
		}
		fail(Code::UNKNOWN_PROVIDER, n->name.get_span(), "Unknown provider '" + id + "' on " + rt.spelling());
	}
	if (const auto *n = dynamic_cast<const StateCallExpression *>(&node)) {
		const std::optional<ResolvedType> opt_receiver = scalar(*n->receiver);
		if (!opt_receiver) {
			for (const CallArgument &ca_argument : n->suppliedArguments) scalar(*ca_argument.expression);
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
			if (n->payload) scalar(*n->payload);
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
