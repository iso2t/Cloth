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

ExpressionInfo value (ResolvedType type, const Symbol *symbol = nullptr) {
	return { .types = { std::move(type) }, .symbol = symbol };
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
		analyzer.program.expressions.insert_or_assign(&node, info);
	}
	return info;
}

ResolvedType BodyChecker::scalar (const Expression &node, const ResolvedType *expected) {
	ResultTypes types;
	if (expected) {
		types.push_back(*expected);
	}
	const ExpressionInfo result = expression(node, expected ? &types : nullptr);
	if (result.types.size() != 1) {
		fail(result.types.empty() ? Code::VOID_VALUE : Code::MULTIPLE_RESULTS_IN_SCALAR, node.get_span(), "Expected one value");
	}
	return result.types.front();
}

ExpressionInfo BodyChecker::name (const Token &token) {
	const std::string &spelling = token.get_lexeme();
	if (const Binding *binding = local(spelling)) {
		remember(token, *binding->name);
		const auto found = narrowed.find(binding->name);
		return { { found == narrowed.end() ? binding->type : found->second }, nullptr, binding->name };
	}
	if (spelling == to_string(kSelf)) {
		if (!owner) {
			fail(Code::RECEIVER_OUTSIDE_METHOD, token.get_span(), std::format("{} requires a member body", to_string(kSelf)));
		}
		return value(selfType());
	}
	if (spelling == to_string(kState) && owner && owner->kind == SymbolKind::State) {
		return value(*analyzer.declarations.at(owner).value);
	}
	if (owner) {
		const std::vector<Candidate> fields = analyzer.members(selfType(), spelling, SymbolKind::Field);
		if (!fields.empty()) {
			const Candidate &field = fields.front();
			analyzer.requireAccess(*field.symbol, context, token.get_span());
			remember(token, field.symbol->name);
			return value(substitute(*analyzer.declarations.at(field.symbol).value, field.arguments), field.symbol);
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

ResolvedType BodyChecker::receiver (const Expression &node, bool &isType, bool allowOpen) {
	std::optional<ResolvedType> type;
	if (const TypeReceiverExpression *r = dynamic_cast<const TypeReceiverExpression *>(&node)) {
		type = analyzer.type(r->annotation, context, 0, allowOpen);
	} else if (const std::optional<Token> path = typeNameCandidate(node, [&] (const Token &t) {
		           if (local(t.get_lexeme()) || (owner && !analyzer.members(selfType(), t.get_lexeme(), SymbolKind::Field).empty())) {
			           return false;
		           }
		           return analyzer.genericParameter(context, t.get_lexeme()) ||
		                  std::ranges::any_of(analyzer.lookup(context, t.get_lexeme()), [] (const Symbol *s) { return s->kind == SymbolKind::Type; });
	           })) {
		try {
			type = analyzer.type(TypeReference(*path), context, 0, allowOpen);
		} catch (const cde::ClothDiagnostic &error) {
			if (error.get_code() != Code::UNSUPPORTED_TYPE) {
				throw;
			}
		}
	}
	isType = type.has_value();
	if (!type) {
		return scalar(node);
	}
	if (!allowOpen) {
		analyzer.checkBounds(*type, node.get_span(), context);
	}
	if (record) {
		analyzer.program.expressions[&node] = value(ResolvedType(kTypeinfo), type->declaration);
	}
	return *type;
}

ExpressionInfo BodyChecker::field (const FieldAccessExpression &node) {
	bool               isType = false;
	const ResolvedType rt     = receiver(*node.receiver, isType);
	if (rt.nullable()) {
		fail(Code::NULLABLE_PAYLOAD_ACCESS, node.get_span(), std::format("Use {} to access a nullable receiver", to_string(oSafeDot)));
	}
	const std::vector<Candidate> fields = analyzer.members(rt, node.name.get_lexeme(), isType ? SymbolKind::EnumCase : SymbolKind::Field);
	if (fields.empty()) {
		fail(Code::UNKNOWN_MEMBER, node.name.get_span(), "Unknown member '" + node.name.get_lexeme() + "' on " + rt.spelling());
	}
	const Candidate &member = fields.front();
	analyzer.requireAccess(*member.symbol, context, node.name.get_span());
	remember(node.name, member.symbol->name);
	return value(substitute(*analyzer.declarations.at(member.symbol).value, member.arguments), member.symbol);
}

std::optional<Candidate> BodyChecker::stateTarget (const ResolvedType &receiver, const Token &name) {
	const ResolvedType host = receiver.nullable() ? receiver.element() : receiver;
	for (SymbolKind kind : { SymbolKind::State, SymbolKind::StateAttachment }) {
		const std::vector<Candidate> candidates = analyzer.members(host, name.get_lexeme(), kind);
		if (candidates.empty()) {
			continue;
		}
		if (receiver.nullable()) {
			fail(Code::NULLABLE_PAYLOAD_ACCESS, name.get_span(), "A state operation requires a non-null receiver");
		}
		analyzer.requireAccess(*candidates.front().symbol, context, name.get_span());
		remember(name, candidates.front().symbol->name);
		return candidates.front();
	}
	return std::nullopt;
}

ExpressionInfo BodyChecker::transition (const Candidate &state, const ExpressionPtr &payload, const TokenSpan &span) {
	const ResolvedType required = substitute(*analyzer.declarations.at(state.symbol).value, state.arguments);
	if (payload && required.name == to_string(kVoid)) {
		fail(Code::INVALID_STATE_PAYLOAD, payload->get_span(), "This state takes no payload");
	}
	if (payload) {
		expect(*payload, required);
	} else if (required.name != to_string(kVoid)) {
		fail(Code::INVALID_STATE_PAYLOAD, span, "Missing state payload");
	}
	return { {}, state.symbol };
}

ExpressionInfo BodyChecker::typeCheck (const TypeCheckExpression &node) {
	const Token  *identifier = node.target.get_bare_identifier();
	ExpressionPtr valueTarget;
	if (node.valueCandidate) {
		bool isType = false;
		receiver(*node.valueCandidate, isType);
		if (!isType) {
			valueTarget = node.valueCandidate;
		}
	} else if (identifier &&
	           (local(identifier->get_lexeme()) || (owner && !analyzer.members(selfType(), identifier->get_lexeme(), SymbolKind::Field).empty()))) {
		valueTarget = std::make_shared<NameExpression>(*identifier);
	}
	if (valueTarget) {
		std::shared_ptr<const BinaryExpression> test = std::make_shared<BinaryExpression>(node.operand, node.op, valueTarget);
		if (record) {
			test = analyzer.program.valueTests.try_emplace(&node, test).first->second;
		}
		return expression(*test);
	}
	const ResolvedType source = scalar(*node.operand);
	if (identifier && !analyzer.genericParameter(context, identifier->get_lexeme()) &&
	    !std::ranges::any_of(analyzer.lookup(context, identifier->get_lexeme()), [] (const Symbol *symbol) { return symbol->kind == SymbolKind::Type; })) {
		if (const std::optional<Candidate> state = stateTarget(source, *identifier)) {
			return value(ResolvedType(kBoolean), state->symbol);
		}
	}
	if (source.name == to_string(kNull)) {
		fail(Code::EXPLICIT_NULLABLE_TYPE, node.operand->get_span(), "A type test needs a typed operand");
	}
	const ResolvedType target = analyzer.checkedType(node.target, context);
	return value(ResolvedType(kBoolean), target.declaration);
}

ExpressionInfo BodyChecker::binary (const BinaryExpression &node, const ResolvedType *expected) {
	const Operator o_operation = node.op.get_operator();
	if (o_operation == oAnd || o_operation == oOr) {
		expect(*node.left, ResolvedType(kBoolean));
		BodyChecker right = *this;
		right.refine(*node.left, o_operation == oAnd);
		right.expect(*node.right, ResolvedType(kBoolean));
		return value(ResolvedType(kBoolean));
	}
	const bool   compare = isComparison(o_operation);
	ResolvedType left, right;
	if (numericLiteral(*node.left) && !numericLiteral(*node.right)) {
		right = scalar(*node.right);
		left  = scalar(*node.left, &right);
	} else {
		left  = scalar(*node.left, !compare && expected && numeric(*expected) ? expected : nullptr);
		right = scalar(*node.right, &left);
	}
	std::vector<Candidate> sides[2];
	for (unsigned side = 0; side < 2; ++side) {
		const ResolvedType &peer = side ? left : right;
		for (const Candidate &c : analyzer.members(side ? right : left, node.op.get_lexeme(), SymbolKind::Operator)) {
			const OperatorMember &m = *dynamic_cast<const MethodDeclaration &>(*callable(*c.symbol)).operatorMember;
			if (m.provider || (!m.selector && m.rightReceiver != static_cast<bool>(side))) {
				continue;
			}
			if (m.selector) {
				ResolvedType selector = substitute(*analyzer.declarations.at(c.symbol).value, c.arguments);
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
		if (right.name != "range" || right.declaration || right.arguments.size() != 1 || !analyzer.accepts(right.arguments.front(), left)) {
			fail(Code::INVALID_RANGE_MEMBERSHIP, node.get_span(), "Membership requires a range with compatible endpoints");
		}
		return value(ResolvedType(kBoolean));
	}
	if (isEquality(o_operation)) {
		if (!analyzer.accepts(left, right) && !analyzer.accepts(right, left) &&
		    !(numeric(left) && numeric(right) && (left.floating() || right.floating()))) {
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
		range.arguments.push_back(left);
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
	if (const Binding *b = local(node.name.get_lexeme())) {
		if (!b->deferred || !node.suppliedArguments.empty() || node.typeArguments) {
			fail(Code::NOT_CALLABLE, node.name.get_span(), "Local binding is not callable with these arguments", b->name);
		}
		return { { b->type }, nullptr, b->name };
	}
	std::vector<Candidate> group;
	if (owner && owner->kind == SymbolKind::State) {
		group = analyzer.members(analyzer.ownerType(*owner), node.name.get_lexeme(), SymbolKind::Method);
	}
	if (owner && group.empty()) {
		group = analyzer.members(selfType(), node.name.get_lexeme(), SymbolKind::Method);
	}
	if (group.empty()) {
		const Candidates candidates = analyzer.lookup(context, node.name.get_lexeme());
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
	bool         isType = false;
	ResolvedType rt     = receiver(*node.receiver, isType, true);
	if (!isType || (!rt.declaration && !rt.parameter) || !rt.suffixes.empty()) {
		fail(Code::INVALID_CLASS_LIFECYCLE, node.get_span(), std::format("{} requires a class type or a constructor bound", to_string(kNew)));
	}
	auto constructors = analyzer.members(rt, rt.name, SymbolKind::Constructor);
	if (constructors.empty()) {
		fail(Code::INVALID_CLASS_LIFECYCLE, node.get_span(), "No constructor declared for " + rt.spelling());
	}
	const TypeParameterList *params     = rt.declaration ? typeParameters(*rt.declaration) : nullptr;
	const auto              *annotation = dynamic_cast<const TypeReceiverExpression *>(node.receiver.get());
	if (params) {
		for (Candidate &candidate : constructors) {
			for (std::size_t i = 0; i < params->parameters.size(); ++i) {
				const TypeParameterDeclaration &param = params->parameters[i];
				if (!annotation || i >= annotation->annotation.get_type_arguments().size() ||
				    annotation->annotation.get_type_arguments()[i].is_placeholder()) {
					if (hint && hint->declaration == rt.declaration) {
						candidate.arguments[&param] = hint->arguments[i];
					} else {
						candidate.arguments.erase(&param);
					}
					candidate.inferOwner = params;
				}
			}
		}
	}
	ExpressionInfo result = invoke(node.name, node.get_span(), node.suppliedArguments, constructors);
	rt                    = substitute(rt, result.typeArguments);
	analyzer.checkBounds(rt, node.get_span(), context);
	result.types = { rt };
	return result;
}

ExpressionInfo BodyChecker::cast (const CastExpression &node) {
	if (node.target.get_spelling() == to_string(kVoid)) {
		expression(*node.operand, nullptr, true);
		return {};
	}
	const ResolvedType from = scalar(*node.operand);
	if (const Token *id = node.target.get_bare_identifier();
	    id && !analyzer.genericParameter(context, id->get_lexeme()) &&
	    !std::ranges::any_of(analyzer.lookup(context, id->get_lexeme()), [] (const Symbol *symbol) { return symbol->kind == SymbolKind::Type; })) {
		if (const std::optional<Candidate> state = stateTarget(from, *id)) {
			return transition(*state, nullptr, node.get_span());
		}
	}
	const ResolvedType to = analyzer.checkedType(node.target, context);
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
		for (const ExpressionPtr &part : n->parts) {
			scalar(*part);
		}
		return value(ResolvedType(kString));
	}
	if (const auto *n = dynamic_cast<const ResultListExpression *>(&node)) {
		ResultTypes values;
		for (const ExpressionPtr &part : n->values) {
			values.push_back(scalar(*part, expected && values.size() < expected->size() ? &(*expected)[values.size()] : nullptr));
		}
		return { values };
	}
	if (const auto *n = dynamic_cast<const ArrayLiteral *>(&node)) {
		std::optional<ResolvedType> elem;
		if (hint && hint->array()) {
			elem = hint->element();
		}
		if (!elem && !n->elements.empty()) {
			const auto it = std::ranges::find_if(n->elements, [] (const ExpressionPtr &e) {
				return !numericLiteral(*e) && !dynamic_cast<const NullLiteral *>(e.get());
			});
			elem          = scalar(**(it == n->elements.end() ? n->elements.begin() : it));
		}
		if (!elem || elem->name == to_string(kNull)) {
			fail(Code::INVALID_ARRAY, node.get_span(), "Array literal needs an element type");
		}
		for (const ExpressionPtr &part : n->elements) {
			expect(*part, *elem);
		}
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
		for (std::size_t i = 0; i < n->fields.size(); ++i) {
			const Candidate member = analyzer.members(*hint, td->fields[i].name.get_lexeme(), SymbolKind::Field).front();
			analyzer.requireAccess(*member.symbol, context, n->fields[i]->get_span());
			expect(*n->fields[i], substitute(*analyzer.declarations.at(member.symbol).value, member.arguments));
		}
		return value(*hint);
	}
	if (const auto *n = dynamic_cast<const IndexExpression *>(&node)) {
		const ResolvedType array = scalar(*n->array);
		if (array.declaration && !array.array()) {
			const auto group = analyzer.members(array, to_string(oIndexRead), SymbolKind::Operator);
			return invoke(group.empty() ? array.declaration->name : group.front().symbol->name, node.get_span(), { { n->index } }, group);
		}
		if (!scalar(*n->index).integer() || !array.array()) {
			fail(Code::INVALID_INDEX, node.get_span(), "Indexing requires an array and an integer index");
		}
		return value(array.element());
	}
	if (const auto *n = dynamic_cast<const UnaryExpression *>(&node)) {
		const Operator o_operation = n->op.get_operator();
		if (const IntegerLiteral *literal = dynamic_cast<const IntegerLiteral *>(n->operand.get()); literal && o_operation == oMinus) {
			const ExpressionInfo result = value(literalInteger(*literal, hint, true));
			if (record) {
				analyzer.program.expressions[literal] = result;
			}
			return result;
		}
		const ResolvedType operand = scalar(*n->operand, hint);
		const auto         group   = analyzer.members(operand, n->op.get_lexeme(), SymbolKind::Operator);
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
		const ResolvedType rt = scalar(*n->receiver);
		if (rt.nullable()) {
			fail(Code::NULLABLE_PAYLOAD_ACCESS, node.get_span(),
			     std::format("Use {} to call a member of a nullable receiver", to_string(oSafeDot)));
		}
		const auto group = analyzer.members(rt, n->name.get_lexeme(), SymbolKind::Method);
		if (group.empty()) {
			fail(Code::UNKNOWN_MEMBER, n->name.get_span(), "Unknown method '" + n->name.get_lexeme() + "' on " + rt.spelling());
		}
		return invoke(n->name, node.get_span(), n->suppliedArguments, group, n->typeArguments ? &*n->typeArguments : nullptr);
	}
	if (const auto *n = dynamic_cast<const ClassCreationExpression *>(&node)) {
		return create(*n, hint);
	}
	if (const auto *n = dynamic_cast<const ClassDeletionExpression *>(&node)) {
		const ResolvedType rt = scalar(*n->receiver);
		if (!rt.declaration || !rt.suffixes.empty() ||
		    !dynamic_cast<const ClassDeclaration *>(std::get<const TypeDeclaration *>(rt.declaration->declaration))) {
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
		const ResolvedType rt = scalar(*n->left);
		if (rt.name == to_string(kNull)) {
			return expression(*n->fallback, expected);
		}
		if (!rt.nullable()) {
			fail(Code::INVALID_ELVIS, n->op.get_span(), std::format("Left operand of {} must be nullable", to_string(oElvis)));
		}
		const ResolvedType elem = rt.element(), fallback = scalar(*n->fallback, &elem);
		if (analyzer.accepts(elem, fallback)) {
			return value(elem);
		}
		if (analyzer.accepts(rt, fallback)) {
			return value(rt);
		}
		fail(Code::TYPE_MISMATCH, n->fallback->get_span(), "Incompatible fallback type");
	}
	if (const auto *n = dynamic_cast<const GuardedReceiver *>(&node)) {
		const ResolvedType rt = scalar(*n->source);
		if (!rt.nullable()) {
			fail(Code::INVALID_GUARDED_RECEIVER, node.get_span(), "Guarded access requires a nullable receiver");
		}
		return value(rt.element());
	}
	if (const auto *n = dynamic_cast<const GuardedAccessExpression *>(&node)) {
		ExpressionInfo result = expression(*n->continuation, expected, discard);
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
		BodyChecker inner = *this;
		inner.returns.clear();
		for (const TypeReference &ref : n->declaration.returnTypes) {
			inner.returns.push_back(analyzer.type(ref, context));
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
		bool               isType = false;
		const ResolvedType rt     = receiver(*n->receiver, isType);
		const std::string &id     = n->name.get_lexeme();
		auto               group  = analyzer.members(rt, id, SymbolKind::Operator);
		std::erase_if(group,
		              [] (const Candidate &c) { return !dynamic_cast<const MethodDeclaration &>(*callable(*c.symbol)).operatorMember->provider; });
		if (!group.empty()) {
			return invoke(n->name, node.get_span(), {}, group);
		}
		if (id == provider::S_SIZE) {
			return value(ResolvedType(kUsize));
		}
		if (id == provider::S_TYPE) {
			return value(ResolvedType(kTypeinfo));
		}
		if (id == provider::S_STRING && !isType) {
			return value(ResolvedType(kString));
		}
		if ((id == provider::S_MIN || id == provider::S_MAX) && numeric(rt)) {
			return value(rt);
		}
		fail(Code::UNKNOWN_PROVIDER, n->name.get_span(), "Unknown provider '" + id + "' on " + rt.spelling());
	}
	if (const auto *n = dynamic_cast<const StateCallExpression *>(&node)) {
		const ResolvedType rt = scalar(*n->receiver);
		return invoke(n->name, node.get_span(), n->suppliedArguments, analyzer.members(rt, n->name.get_lexeme(), SymbolKind::StateMethodContract),
		              n->typeArguments ? &*n->typeArguments : nullptr);
	}
	if (const auto *n = dynamic_cast<const StateTransitionExpression *>(&node)) {
		const ResolvedType             rt    = scalar(*n->receiver);
		const std::optional<Candidate> state = stateTarget(rt, n->target);
		if (!state) {
			fail(Code::INVALID_STATE_TARGET, n->target.get_span(), "Unknown state '" + n->target.get_lexeme() + "'");
		}
		return transition(*state, n->payload, node.get_span());
	}
	fail(Code::TYPE_RECEIVER_VALUE, node.get_span(), "Expected a value expression");
}

}
