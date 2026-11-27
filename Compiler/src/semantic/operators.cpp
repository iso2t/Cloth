// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/language/expressions/Operators.rst

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <functional>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <ast/decl/declarations.hpp>
#include <ast/decl/struct_declaration.hpp>
#include <ast/expr/expression.hpp>
#include <ast/expr/expressions.hpp>
#include <ast/type/type_reference.hpp>
#include <diagnostics.hpp>
#include <parser/subs/expression_parser.hpp>
#include <semantic/checked_program.hpp>
#include <semantic/checker.hpp>
#include <semantic/symbol_table.hpp>
#include <token/token.hpp>

namespace cloth::semantic {
namespace {
struct OperatorMatch {
	Candidate                   candidate;
	std::array<ResolvedType, 2> operands;
	unsigned                    rank;
};

struct OperatorMatches {
	std::vector<OperatorMatch> candidates;
	const Symbol              *inaccessible = nullptr;
};

std::optional<Candidate> bestOperator (const Analyzer &analyzer, const Symbol &context, const Token &op, const TokenSpan &span, const OperatorMatches &ordinary,
                                       const OperatorMatches &booleanConversions) {
	const OperatorMatches            &group   = ordinary.candidates.empty() && !ordinary.inaccessible ? booleanConversions : ordinary;
	const std::vector<OperatorMatch> &matches = group.candidates;
	if (matches.empty()) {
		if (group.inaccessible) analyzer.requireAccess(*group.inaccessible, context, span);
		return {};
	}
	const unsigned      u_best       = std::ranges::min_element(matches, {}, &OperatorMatch::rank)->rank;
	const std::function moreSpecific = [&] (const OperatorMatch &candidate, const OperatorMatch &other) {
		const std::array<ResolvedType, 2> &a = candidate.operands, &b = other.operands;
		return analyzer.moreSpecificOperator(a[0], a[1], b[0], b[1]);
	};
	const OperatorMatch *om_selected = nullptr;
	for (const OperatorMatch &om_match : matches) {
		if (om_match.rank != u_best ||
		    std::ranges::any_of(matches, [&] (const OperatorMatch &other) { return other.rank == u_best && moreSpecific(other, om_match); }))
			continue;
		if (om_selected) fail(Code::OperatorMatch, span, "Ambiguous operator '" + op.getLexeme() + "'", &om_match.candidate.symbol->name);
		om_selected = &om_match;
	}
	if (!om_selected) fail(Code::OperatorMatch, span, "Ambiguous operator '" + op.getLexeme() + "'");
	return om_selected->candidate;
}

const OperatorMember &form (const Candidate &candidate) {
	return *dynamic_cast<const MethodDeclaration &>(*callable(*candidate.symbol)).operatorMember;
}

ResolvedType payload (const ResolvedType &type) {
	return type.nullable() ? type.element() : type;
}

}

bool Analyzer::moreSpecificOperator (const ResolvedType &left, const ResolvedType &right, const ResolvedType &otherLeft, const ResolvedType &otherRight) const {
	return accepts(otherLeft, left) && accepts(otherRight, right) && (!accepts(left, otherLeft) || !accepts(right, otherRight));
}

bool Analyzer::hasOperator (std::string_view name) const {
	return s_operatorNames.contains(name);
}

const TypeReference *widthSelector (const Symbol &symbol) {
	if (symbol.kind != SymbolKind::Operator) return nullptr;
	const OperatorMember &om_form = *dynamic_cast<const MethodDeclaration &>(*callable(symbol)).operatorMember;
	return om_form.selector && om_form.selector->capturesWidth() ? &*om_form.selector : nullptr;
}

DeclarationInfo Analyzer::widthSignature (const Symbol &symbol, const Substitutions &arguments, unsigned width) {
	const CallableSignature &cs_method    = *callable(symbol);
	DeclarationInfo          di_signature = specialize(declarations.at(&symbol), arguments);
	di_signature.value                    = checkedType(*widthSelector(symbol), symbol, arguments, false, width);
	if (!cs_method.returnTypes.empty()) {
		di_signature.returns = { checkedType(cs_method.returnTypes.front(), symbol, arguments, false, width) };
	} else if (symbol.name.getOperator() == OPointerRight) {
		di_signature.returns = { *di_signature.value };
	} else if (di_signature.inferResult) {
		const InstanceKey ik_key{
			.declaration = &symbol, .receiver = substitute(ownerType(*di_signature.owner), arguments), .arguments = {}, .operatorWidth = width
		};
		const auto it_cached = m_widthResults.find(ik_key);
		if (it_cached != m_widthResults.end()) {
			di_signature.returns     = it_cached->second;
			di_signature.inferResult = false;
		} else {
			if (!s_inferringWidths.insert(ik_key).second)
				fail(Code::InvalidOperatorDeclaration, symbol.name.getSpan(), "A recursive operand context needs an explicit result type");
			try {
				BodyFacts bf_probe;
				BodyChecker(*this, symbol, bf_probe, arguments, di_signature, nullptr, width).inferOperatorResult();
				m_widthResults.emplace(ik_key, di_signature.returns);
			} catch (...) {
				s_inferringWidths.erase(ik_key);
				throw;
			}
			s_inferringWidths.erase(ik_key);
		}
	}
	return di_signature;
}

const BodyFacts &Analyzer::widthTemplate (const Symbol &symbol, unsigned width) {
	const std::pair  it_template = m_widthTemplates.try_emplace({ &symbol, width });
	GenericInstance &gi_template = it_template.first->second;
	if (it_template.second) {
		// Open receivers record generic dependencies against their declaration, including when requested by a closed call.
		const InstanceWork *iw_previous = iw_active;
		if (isTemplate(symbol)) iw_active = nullptr;
		try {
			gi_template.signature = widthSignature(symbol, {}, width);
			checkPublicSurface(symbol, nullptr, &gi_template.signature);
			BodyChecker(*this, symbol, gi_template.body, {}, gi_template.signature, nullptr, width).check();
		} catch (...) {
			iw_active = iw_previous;
			m_widthTemplates.erase(it_template.first);
			throw;
		}
		iw_active = iw_previous;
	}
	return gi_template.body;
}

void Analyzer::resolveOperator (const Symbol &operation, const MethodDeclaration &method) {
	DeclarationInfo      &di_signature = declarations.at(&operation);
	const OperatorMember &om_form      = *method.operatorMember;
	const Operator        o_op         = method.name.getOperator();
	const bool            b_is         = method.name.getLexeme() == toString(KIs);
	const bool            b_unary      = isNumericPrefix(o_op) || o_op == ONot;
	const bool            b_cast       = o_op == OPointerRight;
	const bool            b_binary     = ExpressionParser::BINARY_OPERATORS.contains(o_op) || o_op == ORange || o_op == ORangeInclusive || b_is;
	const bool            b_index      = o_op == OIndexRead || o_op == OIndexWrite || o_op == OIndexValidate;
	const bool            b_update     = isUpdate(o_op) || o_op == OIndexWrite;
	const std::size_t     st_arity     = method.parameters.size();
	const std::function   invalid      = [&] (std::string_view message) {
                fail(Code::InvalidOperatorDeclaration, method.span, std::format("Operator '{}' {}", method.name.getLexeme(), message));
	};
	s_operatorNames.insert(method.name.getNameKey());
	if (std::ranges::any_of(method.parameters, &ParameterDeclaration::isOptional)) invalid("requires explicit operands");
	if (om_form.provider || om_form.selector) {
		if (om_form.rightReceiver || st_arity || (om_form.provider && om_form.selector))
			invalid("requires an empty parameter list and an implicit receiver");
		if (om_form.selector) {
			if (!b_binary && !b_cast) invalid("does not take a type selector");
			const TypeReference &tr_selector = *om_form.selector;
			if (tr_selector.capturesWidth() &&
			    (tr_selector.isUnsigned() || tr_selector.hasNullability() || !tr_selector.getArraySuffixes().empty() ||
			     (tr_selector.getName().getLexeme() != toString(KInteger) && tr_selector.getName().getLexeme() != toString(KFloat))))
				invalid("requires a scalar signed integer or floating-point type for a captured width");
			di_signature.value = type(tr_selector, operation);
			if (*di_signature.value == ResolvedType(KVoid)) invalid("requires a value type as its selector");
		}
	} else if (b_update || b_index || o_op == OForeach) {
		const std::size_t st_required = o_op == OIndexWrite ? 2 : (o_op == OPlusPlus || o_op == OMinusMinus || o_op == OForeach) ? 0 : 1;
		if (om_form.rightReceiver || st_arity != st_required) invalid("has the wrong operand count or receiver position");
	} else if ((!st_arity && (!b_unary || om_form.rightReceiver)) || (st_arity == 1 && !b_binary) || st_arity > 1) {
		invalid("has no expression form with these operands");
	}
	std::optional<ResolvedType> opt_fixed;
	if (b_cast && !om_form.provider)
		opt_fixed = di_signature.value;
	else if ((!om_form.selector && (o_op == ONot || isComparison(o_op) || o_op == OColon || o_op == OIndexValidate)) || b_is)
		opt_fixed = ResolvedType(KBoolean);
	else if (om_form.provider && method.name.getNameKey() == provider::S_STRING)
		opt_fixed = ResolvedType(KString);
	if (opt_fixed) {
		if (!method.returnTypes.empty() && di_signature.returns != ResultTypes{ *opt_fixed }) invalid("must return " + opt_fixed->spelling());
		di_signature.returns = { *opt_fixed };
	} else if (b_update) {
		if (!di_signature.returns.empty()) invalid("must return " + std::string(toString(KVoid)));
	} else if (di_signature.returns.empty()) {
		if (!method.returnTypes.empty()) invalid("must produce a value");
		if (o_op == OForeach || !std::holds_alternative<ExpressionBody>(method.executableBody()->value)) invalid("requires an explicit result type");
		di_signature.inferResult = true;
	}
}

std::optional<Candidate> BodyChecker::selectOperator (const Token &op, const TokenSpan &span, const ResolvedType &left, const ResolvedType &right,
                                                      bool &rightReceiver, const BinaryExpression *operands, const Symbol **unmatched) const {
	OperatorMatches om_ordinary, om_booleanConversions;
	for (bool const b_right : { false, true }) {
		const ResolvedType &rt_receiver = b_right ? right : left, &rt_peer = b_right ? left : right;
		for (const Candidate &c_candidate : analyzer.members(rt_receiver, op.getNameKey(), SymbolKind::Operator)) {
			const OperatorMember  &om_form      = form(c_candidate);
			const DeclarationInfo &di_signature = analyzer.declarations.at(c_candidate.symbol);
			if (om_form.provider || om_form.selector || om_form.rightReceiver != b_right || di_signature.parameters.size() != 1) continue;
			if (unmatched && rt_peer.name.empty()) *unmatched = c_candidate.symbol;
			const ResolvedType rt_parameter = analyzer.substitute(di_signature.parameters.front(), c_candidate.arguments);
			const ResolvedType rt_owner     = analyzer.substitute(analyzer.ownerType(*di_signature.owner), c_candidate.arguments);
			unsigned           u_rank       = rt_owner == rt_receiver && rt_peer == rt_parameter ? 0 : 1;
			if (!analyzer.accepts(rt_parameter, rt_peer)) {
				if (!operands) continue;
				ExpressionFacts ef_trial;
				BodyChecker     bc_trial   = *this;
				bc_trial.record            = false;
				bc_trial.we_capture        = nullptr;
				bc_trial.ef_probeConstants = &ef_trial;
				try {
					const std::optional<ResolvedType> opt_peer =
					        bc_trial.scalar(b_right ? *operands->left : *operands->right, &rt_parameter);
					if (opt_peer && !analyzer.accepts(rt_parameter, *opt_peer)) continue;
				} catch (const cde::ClothDiagnostic &error) {
					if (contextualFailure(error) ||
					    (error.getCode() == Code::NullablePayloadAccess && !dynamic_cast<const IntrinsicDiagnostic *>(&error)))
						continue;
					throw;
				}
				u_rank = 2;
			}
			OperatorMatches &om_group = usesBooleanArgumentConversion(rt_parameter, rt_peer) ? om_booleanConversions : om_ordinary;
			if (!analyzer.accessible(c_candidate, context)) {
				om_group.inaccessible = c_candidate.symbol;
				continue;
			}
			om_group.candidates.push_back({ .candidate = c_candidate,
			                                .operands  = { b_right ? rt_parameter : rt_owner, b_right ? rt_owner : rt_parameter },
			                                .rank      = u_rank });
		}
	}
	std::optional<Candidate> opt_match = bestOperator(analyzer, context, op, span, om_ordinary, om_booleanConversions);
	if (opt_match) rightReceiver = form(*opt_match).rightReceiver;
	return opt_match;
}

std::optional<Candidate> BodyChecker::selectContext (const Token &op, const ResolvedType &receiver, const ResolvedType &peer, bool typeTarget) const {
	OperatorMatches om_ordinary, om_booleanConversions;
	for (const Candidate &c_context : analyzer.members(receiver, op.getNameKey(), SymbolKind::Operator)) {
		const OperatorMember &om_form = form(c_context);
		if (!om_form.selector) continue;
		const DeclarationInfo &di_signature = analyzer.declarations.at(c_context.symbol);
		ResolvedType           rt_selector  = analyzer.substitute(*di_signature.value, c_context.arguments);
		const bool             b_width      = om_form.selector->capturesWidth();
		if (b_width) rt_selector.width = peer.width;
		if (typeTarget ? rt_selector != peer : !analyzer.accepts(rt_selector, peer)) continue;
		OperatorMatches &om_group = rt_selector.convertsIntegerToBool(peer) ? om_booleanConversions : om_ordinary;
		if (!analyzer.accessible(c_context, context)) {
			om_group.inaccessible = c_context.symbol;
			continue;
		}
		const unsigned u_rank = rt_selector == peer ? (b_width ? 1 : 0) : 2;
		om_group.candidates.push_back(
		        { .candidate = c_context,
		          .operands  = { analyzer.substitute(analyzer.ownerType(*di_signature.owner), c_context.arguments), std::move(rt_selector) },
		          .rank      = u_rank });
	}
	std::optional<Candidate> opt_selected = bestOperator(analyzer, context, op, op.getSpan(), om_ordinary, om_booleanConversions);
	if (opt_selected && widthSelector(*opt_selected->symbol)) opt_selected->operatorWidth = peer.width;
	return opt_selected;
}

ExpressionInfo BodyChecker::callOperator (const Candidate &candidate, const Expression &receiver, const ResolvedType &type, const Token &op,
                                          const TokenSpan &span, const ExpressionPtr &peer, const ResolvedType *peerType) {
	const ResolvedType rt_owner = analyzer.substitute(analyzer.ownerType(*analyzer.declarations.at(candidate.symbol).owner), candidate.arguments);
	if (type.nullable()) fail(Code::NullablePayloadAccess, receiver.getSpan(), "An operator requires a present receiver", &candidate.symbol->name);
	if (!analyzer.accepts(rt_owner, type)) fail(Code::OperatorMatch, span, "The operand no longer has the selected receiver type", &candidate.symbol->name);
	if (readOnlyDefaultRoot(receiver))
		fail(Code::InvalidParameterDefault, op.getSpan(), "A default cannot invoke an operator on a read-only receiver", &pd_default->name);
	checkValidatorReceiver(&receiver, op.getSpan());
	ResultTypes               v_types;
	std::vector<CallArgument> v_arguments;
	if (peer) {
		v_arguments.push_back({ .expression = peer });
		v_types.push_back(*peerType);
	}
	ExpressionInfo ei_call = invoke(op, span, v_arguments, { candidate }, nullptr, &v_types);
	mutatingReceiver(receiver, type);
	return ei_call;
}

std::optional<ExpressionInfo> BodyChecker::binaryOperator (const BinaryExpression &node, const ResolvedType *expected, bool allowContexts,
                                                           const FloatContext &floats) {
	const bool b_inequality = node.op.getOperator() == ONotEqual;
	if (!analyzer.hasOperator(node.op.getNameKey()) && !(b_inequality && analyzer.hasOperator(toString(OEqual)))) return {};
	ExpressionFacts ef_natural;
	BodyChecker     bc_probe   = *this;
	bc_probe.record            = false;
	bc_probe.we_capture        = nullptr;
	bc_probe.ef_probeConstants = &ef_natural;
	const std::function probe  = [&] (const Expression &operand) {
                const bool b_cache = operatorProbes && !captured && !b_inferenceDefaults &&
                                     (dynamic_cast<const BinaryExpression *>(&operand) || dynamic_cast<const ElvisExpression *>(&operand));
                if (b_cache) {
                        const auto it_probe = operatorProbes->find(&operand);
                        if (it_probe != operatorProbes->end() && it_probe->second.before == bc_probe.fs_flow) {
                                bc_probe.fs_flow = it_probe->second.after;
                                return it_probe->second.type;
                        }
                }
                try {
                        const FlowState      fs_before  = bc_probe.fs_flow;
                        const ExpressionInfo ei_operand = bc_probe.scalarInfo(operand);
                        ResolvedType         rt_type    = ei_operand.fallsThrough ? ei_operand.types.front() : ResolvedType{};
                        if (b_cache)
                                operatorProbes->insert_or_assign(&operand, OperandProbe{ .before     = fs_before,
				                                                          .after      = bc_probe.fs_flow,
				                                                          .type       = rt_type,
				                                                          .overloaded = ei_operand.argumentContract || ei_operand.leftContext ||
                                                                                                       ei_operand.rightContext });
                        return rt_type;
                } catch (const cde::ClothDiagnostic &) {
                        // The committed path supplies context and the short-circuit presence proofs.
                        return ResolvedType{};
                }
	};
	const ResolvedType rt_left = probe(*node.left), rt_right = probe(*node.right);
	if (rt_left.declaration || rt_left.parameter || rt_right.declaration || rt_right.parameter) {
		if (std::optional<ExpressionInfo> result = applyBinaryOperator(node, rt_left, rt_right, expected, allowContexts, floats)) return result;
	}
	const Operator op = node.op.getOperator();
	if (allowContexts && (op == OAnd || op == OOr) && rt_left == ResolvedType(KBoolean) && rt_right != rt_left) {
		// A presence test on the left can expose the right operand's operator only on the path that evaluates it.
		bc_probe.fs_flow          = fs_flow;
		const TestPaths paths     = bc_probe.test(*node.left);
		const TestPath &rightPath = op == OAnd ? paths.yes : paths.no;
		if (rightPath.facts != bc_probe.fs_flow.narrowed) {
			bc_probe.fs_flow.narrowed   = rightPath.facts;
			const ResolvedType narrowed = probe(*node.right);
			if (narrowed != rt_right && (narrowed.declaration || narrowed.parameter))
				return applyBinaryOperator(node, rt_left, narrowed, expected, allowContexts, floats);
		}
	}
	return {};
}

std::optional<ExpressionInfo> BodyChecker::applyBinaryOperator (const BinaryExpression &node, const ResolvedType &rtLeft, const ResolvedType &rtRight,
                                                                const ResolvedType *expected, bool allowContexts, const FloatContext &floats) {
	if (isEquality(node.op.getOperator())) {
		if (!rtLeft.name.empty()) receiverClassBound(*node.left, rtLeft);
		if (!rtRight.name.empty()) receiverClassBound(*node.right, rtRight);
	}
	const bool               b_inequality = node.op.getOperator() == ONotEqual;
	const bool               b_lift       = isEquality(node.op.getOperator()) && rtLeft != ResolvedType(KNull) && rtRight != ResolvedType(KNull);
	const ResolvedType       rt_a = b_lift ? payload(rtLeft) : rtLeft, rt_b = b_lift ? payload(rtRight) : rtRight;
	const Token              t_equal(std::string(toString(OEqual)), OEqual, node.op.getSpan());
	bool                     b_right = false, b_negate = false, b_lifted = false;
	const Symbol            *sym_unmatched = nullptr;
	std::optional<Candidate> opt_full      = selectOperator(node.op, node.op.getSpan(), rtLeft, rtRight, b_right, &node, &sym_unmatched);
	if (!opt_full && b_lift && (rtLeft.nullable() || rtRight.nullable())) {
		opt_full = selectOperator(node.op, node.op.getSpan(), rt_a, rt_b, b_right);
		b_lifted = opt_full.has_value();
	}
	if (!opt_full && b_inequality) {
		opt_full = selectOperator(t_equal, node.op.getSpan(), rtLeft, rtRight, b_right, &node, &sym_unmatched);
		if (!opt_full && b_lift && (rtLeft.nullable() || rtRight.nullable())) {
			opt_full = selectOperator(t_equal, node.op.getSpan(), rt_a, rt_b, b_right);
			b_lifted = opt_full.has_value();
		}
		b_negate = opt_full.has_value();
	}
	if (opt_full) {
		const ResolvedType rt_param = analyzer.substitute(analyzer.declarations.at(opt_full->symbol).parameters.front(), opt_full->arguments);
		const std::optional<ResolvedType> opt_left = scalar(*node.left, b_right ? &rt_param : nullptr);
		std::optional<BodyChecker>        bc_unreachable;
		const std::optional<ResolvedType> opt_right = onPath(opt_left.has_value(), bc_unreachable).scalar(*node.right, b_right ? nullptr : &rt_param);
		if (!opt_left || !opt_right) return ExpressionInfo::never();
		const ResolvedType &rt_peer     = b_right ? *opt_left : *opt_right;
		const ResolvedType  rt_argument = b_lifted && !analyzer.accepts(rt_param, rt_peer) ? payload(rt_peer) : rt_peer;
		const ResolvedType &rt_receiver = b_right ? *opt_right : *opt_left;
		ExpressionInfo ei_call = callOperator(*opt_full, b_right ? *node.right : *node.left, b_lifted ? payload(rt_receiver) : rt_receiver, node.op,
		                                      node.getSpan(), b_right ? node.left : node.right, &rt_argument);
		ei_call.negateEquality = b_negate;
		ei_call.liftedEquality = b_lifted;
		ei_call.rightReceiver           = b_right;
		return ei_call;
	}
	if (sym_unmatched)
		fail(Code::OperatorMatch, node.op.getSpan(), "No matching overload for operator '" + node.op.getLexeme() + "'", &sym_unmatched->name);
	const bool logicalOp = node.op.getOperator() == OAnd || node.op.getOperator() == OOr;
	if (!allowContexts || node.op.getLexeme() == toString(KIs) || (!logicalOp && (rtLeft.name.empty() || rtRight.name.empty())) || rtLeft.nullable() ||
	    rtRight.nullable() || rtLeft == ResolvedType(KNull) || rtRight == ResolvedType(KNull) ||
	    (isEquality(node.op.getOperator()) && (rtLeft == rtRight || (analyzer.referenceType(rtLeft) && analyzer.referenceType(rtRight)))))
		return {};
	const ResolvedType  boolean(KBoolean);
	const std::function contextFor = [&] (const ResolvedType &receiver, const ResolvedType &peer) -> std::optional<Candidate> {
		if (receiver.name.empty()) return {};
		// A control transfer has no value type. A logical operand can still supply the boolean used to reach it.
		std::optional<Candidate> opt_context = selectContext(node.op, receiver, peer.name.empty() ? boolean : peer);
		if (!opt_context && b_inequality) opt_context = selectContext(t_equal, receiver, peer);
		return opt_context;
	};
	const std::optional<Candidate> opt_left = contextFor(rtLeft, rtRight), opt_right = contextFor(rtRight, rtLeft);
	if (!opt_left && !opt_right) return {};
	if (logicalOp) {
		const auto suppliesBool = [&] (const ResolvedType &type, const std::optional<Candidate> &context) {
			if (!context) return type == boolean || type.name.empty();
			const ResultTypes results = analyzer.callResults(*context->symbol, context->arguments, context->operatorWidth);
			return results.size() == 1 && results.front() == boolean;
		};
		if (suppliesBool(rtLeft, opt_left) && suppliesBool(rtRight, opt_right)) return logical(node, opt_left, opt_right);
		if (rtLeft.name.empty() || rtRight.name.empty()) return {};
	}
	const std::function project = [&] (const Expression &operand, const std::optional<Candidate> &candidate) {
		ExpressionInfo ei_operand = scalarInfo(operand);
		if (!candidate || !ei_operand.fallsThrough) return ei_operand;
		return callOperator(*candidate, operand, ei_operand.types.front(), node.op, node.getSpan());
	};
	const ExpressionInfo  ei_left = project(*node.left, opt_left), ei_right = project(*node.right, opt_right);
	const CapturedOperand co_left{ .node = node.left.get(), .value = ei_left, .previous = captured },
	        co_right{ .node = node.right.get(), .value = ei_right, .previous = &co_left };
	BodyChecker bc_application = *this;
	bc_application.captured    = &co_right;
	ExpressionInfo ei_result   = bc_application.binary(node, expected, floats, false);
	fs_flow                    = std::move(bc_application.fs_flow);
	if (opt_left && ei_left.fallsThrough) ei_result.leftContext = OperatorSelection{ .symbol = ei_left.symbol, .instance = ei_left.instance };
	if (opt_right && ei_right.fallsThrough) ei_result.rightContext = OperatorSelection{ .symbol = ei_right.symbol, .instance = ei_right.instance };
	return ei_result;
}

void requireRangeMember (const Expression &member, const ResolvedType &actual, const ResolvedType &endpoint) {
	if (actual != endpoint)
		fail(Code::InvalidRangeMembership, member.getSpan(),
		     std::format("Range membership requires {}, got {}", endpoint.spelling(), actual.spelling()));
}

ExpressionInfo BodyChecker::equality (const BinaryExpression &node, const ResolvedType &left, const ResolvedType &right) {
	const Operator      o_op = node.op.getOperator();
	ExpressionInfo      ei_result{ .types = { ResolvedType(KBoolean) } };
	const std::function incomparable = [&] {
		fail(Code::IncomparableTypes, node.op.getSpan(), "Cannot compare " + left.spelling() + " with " + right.spelling());
	};
	const ResolvedType *rt_leftEndpoint = rangeEndpoint(left), *rt_rightEndpoint = rangeEndpoint(right);
	if (o_op == OColon && (rt_leftEndpoint || !rt_rightEndpoint || left == ResolvedType(KNull)))
		fail(Code::InvalidRangeMembership, node.op.getSpan(),
		     std::format("Operator '{}' requires a numeric value on the left and a range on the right", toString(o_op)));
	if (o_op != OIdentity && (rt_leftEndpoint || rt_rightEndpoint) && left != ResolvedType(KNull) && right != ResolvedType(KNull)) {
		if (rt_leftEndpoint && rt_rightEndpoint) {
			if (*rt_leftEndpoint != *rt_rightEndpoint) incomparable();
			ei_result.range = RangeOperation::Equal;
		} else {
			const Expression   &e_range = rt_leftEndpoint ? *node.left : *node.right, &e_member = rt_leftEndpoint ? *node.right : *node.left;
			const ResolvedType &rt_endpoint = rt_leftEndpoint ? *rt_leftEndpoint : *rt_rightEndpoint, &rt_member = rt_leftEndpoint ? right : left;
			if ((rt_leftEndpoint ? left : right).nullable())
				fail(Code::NullablePayloadAccess, e_range.getSpan(), "Range membership requires a present range");
			requireRangeMember(e_member, rt_member, rt_endpoint);
			ei_result.range = RangeOperation::Membership;
		}
		return ei_result;
	}
	if (node.op.getLexeme() == toString(KIs)) fail(Code::InvalidRangeMembership, node.getSpan(), "Membership requires a range with compatible endpoints");
	receiverClassBound(*node.left, left);
	receiverClassBound(*node.right, right);
	if (left == ResolvedType(KNull) || right == ResolvedType(KNull)) {
		const bool                      b_leftNull  = left == ResolvedType(KNull);
		const ResolvedType             &rt_peer     = b_leftNull ? right : left;
		const std::optional<TypedPlace> tp_place    = locate(b_leftNull ? *node.right : *node.left);
		const ResolvedType             &rt_declared = tp_place ? tp_place->type : rt_peer;
		if (!rt_declared.nullable())
			fail(Code::InvalidNullComparison, node.op.getSpan(),
			     std::format("Comparison with {} requires a typed nullable operand", toString(KNull)));
		if (o_op == OIdentity && !analyzer.referenceType(payload(rt_declared))) incomparable();
		if (o_op != OIdentity) ei_result.comparisonType = rt_declared;
		return ei_result;
	}
	const ResolvedType rt_left = payload(left), rt_right = payload(right);
	const bool         b_references = analyzer.referenceType(rt_left) && analyzer.referenceType(rt_right);
	if (o_op == OIdentity) {
		if (!b_references) incomparable();
		ei_result.checkedIdentity = analyzer.hasMultipleTypeBounds(rt_left) || analyzer.hasMultipleTypeBounds(rt_right);
		if (!m_arguments.empty()) {
			const BodyFacts &bf_open = openFacts();
			const auto       it_open = bf_open.expressions.find(&node);
			if (it_open != bf_open.expressions.end()) ei_result.checkedIdentity |= it_open->second.checkedIdentity;
		}
		return ei_result;
	}
	if ((rt_left == ResolvedType(KBoolean) && rt_left == rt_right) || rt_left.convertsIntegerToBool(rt_right) || rt_right.convertsIntegerToBool(rt_left)) {
		ei_result.booleanEquality = true;
		if (left.nullable() || right.nullable()) {
			ei_result.comparisonType.emplace(KBoolean);
			ei_result.comparisonType->suffixes.emplace_back(toString(OQuestion));
		}
		return ei_result;
	}
	if (rt_left.array() || rt_right.array()) {
		if (comparisonShape(rt_left) != comparisonShape(rt_right)) incomparable();
		ResolvedType rt_leaf = rt_left;
		rt_leaf.suffixes.clear();
		const Token                    t_equal(std::string(toString(OEqual)), OEqual, node.op.getSpan());
		bool                           b_right   = false;
		const std::optional<Candidate> opt_equal = selectOperator(t_equal, node.op.getSpan(), rt_leaf, rt_leaf, b_right);
		if (!opt_equal && (asStruct(rt_leaf.declaration) || !analyzer.supportsEquality(rt_leaf))) incomparable();
		if (opt_equal) {
			const GenericInstance *gi_instance = record ? analyzer.useCallable(*opt_equal->symbol, opt_equal->arguments, context, node.getSpan()) :
			                                              nullptr;
			ei_result.elementEquality = OperatorSelection{ .symbol = opt_equal->symbol, .instance = gi_instance, .rightReceiver = b_right };
		}
		ei_result.callsElementEquality = opt_equal.has_value() || (analyzer.referenceType(rt_leaf) && analyzer.classEqualityMayCall(rt_leaf));
		if (ei_result.callsElementEquality) {
			if (readOnlyDefaultRoot(b_right ? *node.right : *node.left))
				fail(Code::InvalidParameterDefault, node.op.getSpan(), "A default cannot invoke element equality on a read-only operand",
				     &pd_default->name);
			invalidateShared();
		}
		return ei_result;
	}
	if (b_references) {
		ei_result.callsClassEquality = analyzer.classEqualityMayCall(rt_left, o_op);
		if (ei_result.callsClassEquality) {
			if (readOnlyDefaultRoot(*node.left))
				fail(Code::InvalidParameterDefault, node.op.getSpan(), "A default cannot invoke equality on a read-only receiver",
				     &pd_default->name);
			invalidateShared();
		}
		return ei_result;
	}
	if (!analyzer.supportsEquality(rt_left) || !analyzer.supportsEquality(rt_right) || asStruct(rt_left.declaration) ||
	    (!analyzer.accepts(left, right) && !analyzer.accepts(right, left) &&
	     !(!left.nullable() && !right.nullable() && (left.integer() || left.floating()) && (right.integer() || right.floating()) &&
	       (left.floating() || right.floating()))))
		incomparable();
	if ((rt_left.integer() || rt_left.floating()) && (rt_right.integer() || rt_right.floating())) {
		if (left.nullable() || right.nullable())
			ei_result.comparisonType = left.nullable() ? left : right;
		else
			ei_result.comparisonType = numericOperationType(left, right);
	} else if (rt_left == rt_right && (rt_left == ResolvedType(KCharacter) || rt_left.enumeration()))
		ei_result.comparisonType = left.nullable() ? left : right;
	return ei_result;
}
}
