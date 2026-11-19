// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <ranges>
#include <variant>

#include <semantic/checker.hpp>
#include <token/token.hpp>

namespace cloth::semantic {
namespace {

struct ArgumentPlacement {
	std::vector<std::size_t> destinations;
	std::vector<std::size_t> omitted;
};

struct BindingProblem {
	TokenSpan    span;
	std::string  message;
	const Token *declaration;
};

using PlacementResult = std::variant<ArgumentPlacement, BindingProblem>;

PlacementResult placeArguments (const std::vector<CallArgument> &arguments, const Symbol &symbol, const TokenSpan &span) {
	const std::vector<ParameterDeclaration> &v_parameters = callable(symbol)->parameters;
	ArgumentPlacement                        ap_placement{ std::vector<std::size_t>(arguments.size()), {} };
	std::vector<const Token *>               v_labels(v_parameters.size());
	std::size_t                              st_positional = 0;
	for (std::size_t st_source = 0; st_source < arguments.size(); ++st_source) {
		const std::optional<Token> &opt_label = arguments[st_source].label;
		if (!opt_label) {
			++st_positional;
			continue;
		}
		const std::vector<ParameterDeclaration>::const_iterator it_parameter = std::ranges::find_if(
		        v_parameters, [&] (const ParameterDeclaration &parameter) { return parameter.name.get_lexeme() == opt_label->get_lexeme(); });
		if (it_parameter == v_parameters.end()) {
			return BindingProblem{ opt_label->get_span(), "Unknown argument label '" + opt_label->get_lexeme() + "'", &symbol.name };
		}
		const std::size_t st_slot            = static_cast<std::size_t>(it_parameter - v_parameters.begin());
		ap_placement.destinations[st_source] = st_slot;
		v_labels[st_slot]                    = &*opt_label;
	}
	std::size_t st_required = 0;
	for (std::size_t st_slot = 0; st_slot < v_parameters.size(); ++st_slot) {
		if (!v_labels[st_slot] && !v_parameters[st_slot].isOptional()) {
			++st_required;
		}
	}
	std::size_t st_next = 0;
	for (std::size_t st_slot = 0; st_slot < v_parameters.size(); ++st_slot) {
		const ParameterDeclaration &pd_parameter = v_parameters[st_slot];
		const Token                *t_label      = v_labels[st_slot];
		if (!pd_parameter.isOptional() && !t_label) {
			--st_required;
		}
		if (st_next < st_positional && (!pd_parameter.isOptional() || st_positional - st_next > st_required)) {
			if (t_label) {
				return BindingProblem{ t_label->get_span(), "Parameter '" + t_label->get_lexeme() + "' is supplied twice", &pd_parameter.name };
			}
			ap_placement.destinations[st_next++] = st_slot;
		} else if (!t_label) {
			if (!pd_parameter.isOptional()) {
				return BindingProblem{ span, "Missing required argument '" + pd_parameter.name.get_lexeme() + "'", &pd_parameter.name };
			}
			ap_placement.omitted.push_back(st_slot);
		}
	}
	if (st_next < st_positional) {
		return BindingProblem{ arguments[st_next].get_span(), "Too many positional arguments", &symbol.name };
	}
	return ap_placement;
}

void inferArguments (const ResolvedType &pattern, ResolvedType actual, const TypeParameterList &generic, Substitutions &args) {
	if (!pattern.dependent() || actual.name == to_string(kNull)) {
		return;
	}
	for (const auto &suffix : std::views::reverse(pattern.suffixes)) {
		if (suffix == to_string(oQuestion) && !actual.nullable()) {
			continue;
		}
		if (actual.suffixes.empty() || (actual.suffixes.back() != suffix && !(suffix == arraySuffix() && actual.array()))) {
			return;
		}
		actual.suffixes.pop_back();
	}
	if (pattern.parameter) {
		for (const TypeParameterDeclaration &p : generic.parameters) {
			if (&p == pattern.parameter) {
				args.try_emplace(&p, std::move(actual));
			}
		}
	} else if (pattern.declaration == actual.declaration && pattern.arguments().size() == actual.arguments().size()) {
		for (std::size_t i = 0; i < pattern.arguments().size(); ++i) {
			inferArguments(pattern.arguments()[i], actual.arguments()[i], generic, args);
		}
	}
}

bool contextualFailure (const cde::ClothDiagnostic &error) {
	if (dynamic_cast<const ReturnDiagnostic *>(&error)) return false;
	const Code code = error.get_code();
	return code == Code::TYPE_MISMATCH || code == Code::INTEGER_RANGE || code == Code::FLOAT_RANGE || code == Code::INVALID_ARRAY ||
	       code == Code::INVALID_STRUCT_LITERAL || code == Code::INVALID_STRUCT_FIELD || code == Code::INVALID_RANGE_VALUE ||
	       code == Code::BRANCH_TYPE_MISMATCH || code == Code::INVALID_NULLABLE_TYPE || code == Code::MIXED_INTEGERS;
}

struct PlacedCandidate {
	const Candidate  &candidate;
	ArgumentPlacement placement;
};

std::vector<PlacedCandidate> bindCandidates (const Token &name, const TokenSpan &span, const std::vector<CallArgument> &arguments,
                                             const std::vector<Candidate> &group, bool named) {
	std::vector<PlacedCandidate>  v_placed;
	std::optional<BindingProblem> opt_firstProblem;
	bool                          b_optional = false;
	for (const Candidate &c_candidate : group) {
		PlacementResult pr_result = placeArguments(arguments, *c_candidate.symbol, span);
		if (ArgumentPlacement *ap_placement = std::get_if<ArgumentPlacement>(&pr_result)) {
			v_placed.push_back({ c_candidate, std::move(*ap_placement) });
		} else {
			if (!opt_firstProblem) opt_firstProblem = std::get<BindingProblem>(std::move(pr_result));
			b_optional |= std::ranges::any_of(callable(*c_candidate.symbol)->parameters, &ParameterDeclaration::isOptional);
		}
	}
	if (v_placed.empty()) {
		if (named || b_optional) fail(Code::INVALID_ARGUMENT_BINDING, opt_firstProblem->span, opt_firstProblem->message, opt_firstProblem->declaration);
		fail(Code::CALL_ARITY, span, "No overload of '" + name.get_lexeme() + "' accepts these arguments", &group.front().symbol->name);
	}
	return v_placed;
}

enum class MatchRank { Exact, Assignable, Contextual };

struct Match {
	const Symbol               *symbol;
	const Symbol               *contract;
	ArgumentPlacement           placement;
	ResultTypes                 parameters;
	Substitutions               substitutions;
	const Substitutions        *overridingArguments;
	std::optional<CallDispatch> dispatch;
	MatchRank                   rank;

	[[nodiscard]] std::size_t omittedCount () const {
		return placement.omitted.size();
	}
};
}

bool BodyChecker::checkArguments (const std::vector<CallArgument> &arguments, const ResultTypes &parameters, const std::vector<ArgumentProbe> &probes,
                                  bool contextual) {
	bool                       b_continues = true;
	std::optional<BodyChecker> opt_unreachable;
	for (std::size_t st_source = 0; st_source < arguments.size(); ++st_source) {
		const ResolvedType  &rt_parameter = parameters[st_source];
		const Expression    &e_argument   = *arguments[st_source].expression;
		BodyChecker         &bc_operand   = onPath(b_continues, opt_unreachable);
		const ArgumentProbe &ap_probe     = probes[st_source];
		const bool           b_sameInput  = ap_probe.result && ap_probe.before == bc_operand.fs_flow;
		const bool           b_fits = b_sameInput && (!ap_probe.result->fallsThrough || analyzer.accepts(rt_parameter, ap_probe.result->types.front()));
		// A nested function or method call has a fixed return contract. Only construction can infer class arguments from the expected type.
		if (b_sameInput && !b_fits && dynamic_cast<const CallSite *>(&e_argument) && !dynamic_cast<const ClassCreationExpression *>(&e_argument)) {
			require(ap_probe.result->types, { rt_parameter }, e_argument.get_span());
		}
		if (b_fits && !record && !we_capture) {
			// Reuse an already typed operand only with the same incoming facts, then carry its effects to the next written argument.
			bc_operand.fs_flow = ap_probe.after;
			b_continues &= ap_probe.result->fallsThrough;
			continue;
		}
		const std::optional<ResolvedType> opt_actual = bc_operand.scalar(e_argument, contextual && !b_fits ? &rt_parameter : nullptr);
		if (opt_actual) {
			require({ *opt_actual }, { rt_parameter }, e_argument.get_span());
			if (record && hasArrayShape(rt_parameter)) {
				const TokenSpan span = e_argument.get_span();
				refineShape(rt_parameter, bc_operand.shapeOf(e_argument, *opt_actual), &span);
			}
		}
		b_continues &= opt_actual.has_value();
	}
	return b_continues;
}

std::vector<Candidate> BodyChecker::methodCandidates (const MemberCallExpression &site, const ResolvedType &receiver) const {
	std::vector<Candidate> v_candidates = analyzer.members(receiver, site.name.get_lexeme(), SymbolKind::Method, MemberLookup::CallContract);
	if (!m_arguments.empty()) {
		using Facts                             = std::unordered_map<const Expression *, ExpressionInfo>;
		const Facts                &m_open      = analyzer.openBody(rt_attachment).expressions;
		const Facts::const_iterator it_receiver = m_open.find(site.receiver.get()), it_call = m_open.find(&site);
		if (it_receiver != m_open.end() && it_call != m_open.end() && it_call->second.argumentContract && it_receiver->second.types.size() == 1 &&
		    it_receiver->second.types.front().parameter) {
			return { analyzer.boundMethod(it_call->second, receiver, v_candidates, m_arguments) };
		}
	}
	return v_candidates;
}

ExpressionInfo BodyChecker::invoke (const Token &name, const TokenSpan &span, const std::vector<CallArgument> &arguments, const std::vector<Candidate> &group,
                                    const TypeArgumentList *typeArguments, const ResultTypes *knownArguments, const Expression *storedReceiver) {
	const ReceiverProofs v_proofs = receiverProofs(arguments.empty() ? nullptr : storedReceiver);
	ExpressionInfo       ei_call  = bindCall(name, span, arguments, group, typeArguments, knownArguments, v_proofs);
	if (ei_call.fallsThrough) invalidateShared();
	return ei_call;
}

ExpressionInfo BodyChecker::bindCall (const Token &name, const TokenSpan &span, const std::vector<CallArgument> &arguments, const std::vector<Candidate> &group,
                                      const TypeArgumentList *typeArguments, const ResultTypes *knownArguments, const ReceiverProofs &proofs) {
	const std::string &spelling = name.get_lexeme();
	if (group.empty()) {
		fail(Code::UNKNOWN_NAME, name.get_span(), "Unknown callable '" + spelling + "'");
	}
	std::map<std::string, const Token *> labels;
	for (const CallArgument &arg : arguments) {
		if (!arg.label && !labels.empty()) {
			fail(Code::INVALID_ARGUMENT_BINDING, arg.get_span(), "Positional arguments must precede named arguments");
		}
		if (arg.label) {
			const std::pair<decltype(labels)::iterator, bool> it_inserted = labels.emplace(arg.label->get_lexeme(), &*arg.label);
			if (!it_inserted.second) {
				fail(Code::INVALID_ARGUMENT_BINDING, arg.label->get_span(), "Duplicate argument label", it_inserted.first->second);
			}
		}
	}
	std::vector<PlacedCandidate>             v_placed = bindCandidates(name, span, arguments, group, !labels.empty());
	std::vector<std::optional<ResolvedType>> explicitTypes;
	if (typeArguments) {
		for (const TypeReference &ref : typeArguments->arguments) {
			explicitTypes.push_back(ref.is_placeholder() ? std::nullopt : std::optional{ bodyType(ref) });
		}
	}
	std::vector<ArgumentProbe> v_probes;
	BodyChecker                natural            = *this;
	natural.record                                = false;
	natural.we_capture                            = nullptr;
	bool                       b_naturalContinues = true;
	std::optional<BodyChecker> opt_unreachable;
	for (const CallArgument &arg : arguments) {
		BodyChecker  &bc_input = natural.onPath(b_naturalContinues, opt_unreachable);
		ArgumentProbe ap_probe{ .result = {}, .before = bc_input.fs_flow, .after = bc_input.fs_flow };
		if (knownArguments) {
			ap_probe.result = ExpressionInfo{ { knownArguments->at(v_probes.size()) } };
		} else {
			BodyChecker bc_operand = bc_input;
			try {
				ap_probe.result = bc_operand.scalarInfo(*arg.expression);
				ap_probe.after = bc_input.fs_flow = std::move(bc_operand.fs_flow);
				b_naturalContinues &= ap_probe.result->fallsThrough;
			} catch (const cde::ClothDiagnostic &error) {
				if (!contextualFailure(error)) throw;
			}
		}
		v_probes.push_back(std::move(ap_probe));
	}
	std::vector<Match>              matches;
	const Symbol                   *inaccessible = nullptr;
	const TypeParameterDeclaration *tp_unproven  = nullptr;
	for (PlacedCandidate &pc_placed : v_placed) {
		const Candidate         &candidate     = pc_placed.candidate;
		ArgumentPlacement       &ap_placement  = pc_placed.placement;
		const Symbol            &symbol        = *candidate.symbol;
		const DeclarationInfo   &info          = analyzer.declarations.at(&symbol);
		Substitutions            substitutions = candidate.arguments;
		const TypeParameterList *generic       = candidate.inferOwner ? candidate.inferOwner : typeParameters(symbol);
		const std::size_t        count         = generic ? generic->parameters.size() : 0;
		if ((typeArguments && !generic) || explicitTypes.size() > count) {
			continue;
		}
		for (std::size_t i = 0; i < explicitTypes.size(); ++i) {
			if (explicitTypes[i]) {
				substitutions[&generic->parameters[i]] = *explicitTypes[i];
			}
		}
		if (generic) {
			for (std::size_t i = 0; i < v_probes.size(); ++i) {
				const std::optional<ExpressionInfo> &opt_result = v_probes[i].result;
				if (opt_result && opt_result->fallsThrough) {
					inferArguments(info.parameters[ap_placement.destinations[i]], opt_result->types.front(), *generic, substitutions);
				}
			}
		}
		bool viable = true;
		for (std::size_t i = 0; i < count; ++i) {
			const TypeParameterDeclaration &param = generic->parameters[i];
			if (!substitutions.contains(&param) && param.defaultType) {
				substitutions[&param] = analyzer.substitute(analyzer.type(*param.defaultType, symbol), substitutions);
			}
			if (!substitutions.contains(&param)) {
				viable = false;
				break;
			}
		}
		if (viable && generic) {
			for (const TypeParameterDeclaration &param : generic->parameters) {
				if (!analyzer.satisfies(param, substitutions, symbol, context)) {
					if (substitutions.at(&param).dependent() && !tp_unproven) tp_unproven = &param;
					viable = false;
				}
			}
		}
		if (!viable) {
			continue;
		}
		Match match{ .symbol              = candidate.overridingMethod ? candidate.overridingMethod : &symbol,
			     .contract            = &symbol,
			     .placement           = std::move(ap_placement),
			     .parameters          = {},
			     .substitutions       = {},
			     .overridingArguments = &candidate.overridingArguments,
			     .dispatch            = candidate.dispatch,
			     .rank                = MatchRank::Exact };
		for (std::size_t i = 0; i < arguments.size(); ++i) {
			const ResolvedType parameter = analyzer.substitute(info.parameters[match.placement.destinations[i]], substitutions);
			match.parameters.push_back(parameter);
			const std::optional<ExpressionInfo> &opt_result = v_probes[i].result;
			if (opt_result && opt_result->fallsThrough && opt_result->types.front() == parameter) {
				continue;
			}
			if (opt_result && (!opt_result->fallsThrough || analyzer.accepts(parameter, opt_result->types.front()))) {
				match.rank = std::max(match.rank, MatchRank::Assignable);
				continue;
			}
			match.rank = MatchRank::Contextual;
		}
		if (match.rank == MatchRank::Contextual) {
			BodyChecker trial = *this;
			trial.record      = false;
			trial.we_capture  = nullptr;
			try {
				trial.checkArguments(arguments, match.parameters, v_probes, true);
			} catch (const cde::ClothDiagnostic &error) {
				if (error.get_code() == Code::PRIVATE_CLASS_MEMBER || error.get_code() == Code::PRIVATE_NESTED_TYPE) {
					if (group.size() == 1 || dynamic_cast<const ReturnDiagnostic *>(&error)) {
						throw;
					}
				} else if (!contextualFailure(error)) {
					throw;
				}
				viable = false;
			}
		}
		if (!viable) {
			continue;
		}
		if (!analyzer.accessible(candidate, context)) {
			inaccessible = &symbol;
			continue;
		}
		if (!candidate.dispatch && candidate.overridingMethod && !analyzer.accessible(*candidate.overridingMethod, context)) {
			inaccessible = candidate.overridingMethod;
			continue;
		}
		match.substitutions = std::move(substitutions);
		matches.push_back(std::move(match));
	}
	if (matches.empty()) {
		if (inaccessible) {
			analyzer.requireAccess(*inaccessible, context, span);
		}
		if (tp_unproven && group.size() == 1) {
			fail(Code::UNSATISFIED_GENERIC_BOUND, span, "Cannot prove the bounds of '" + tp_unproven->name.get_lexeme() + "'", &tp_unproven->name);
		}
		fail(Code::NO_MATCHING_CALL, span, "No overload of '" + spelling + "' accepts these arguments", &group.front().symbol->name);
	}
	const MatchRank mr_best = std::ranges::min_element(matches, {}, &Match::rank)->rank;
	std::erase_if(matches, [mr_best] (const Match &match) { return match.rank != mr_best; });
	if (std::ranges::all_of(matches, [&] (const Match &m) { return m.parameters == matches.front().parameters; })) {
		const std::size_t st_omitted = std::ranges::min_element(matches, {}, &Match::omittedCount)->omittedCount();
		std::erase_if(matches, [st_omitted] (const Match &match) { return match.omittedCount() != st_omitted; });
		if (std::ranges::any_of(matches, [] (const Match &m) { return !typeParameters(*m.contract); })) {
			std::erase_if(matches, [] (const Match &m) { return typeParameters(*m.contract) != nullptr; });
		}
	}
	if (matches.size() != 1) {
		fail(Code::AMBIGUOUS_CALL, span, "Ambiguous call to '" + spelling + "'", &matches.front().symbol->name);
	}
	Match &best = matches.front();
	if (best.contract->visibility == Visibility::Private && asInterface(analyzer.declarations.at(best.contract).owner) &&
	    !callable(*best.contract)->executableBody()) {
		fail(Code::MISSING_INTERFACE_IMPLEMENTATION, span, "Private interface method '" + spelling + "' has no body", &best.contract->name);
	}
	bool b_continues = true;
	if (!knownArguments || best.rank == MatchRank::Contextual) {
		// Only the selected contract commits facts. Replay written arguments in source order.
		if (!record && !we_capture && best.rank != MatchRank::Contextual) {
			fs_flow     = std::move(natural.fs_flow);
			b_continues = b_naturalContinues;
		} else {
			b_continues = checkArguments(arguments, best.parameters, v_probes, best.rank == MatchRank::Contextual);
		}
	}
	if (b_continues) requireReceiverProofs(proofs);
	ResultTypes results;
	for (const ResolvedType &rt : analyzer.resultsOf(*best.contract)) {
		results.push_back(analyzer.substitute(rt, best.substitutions));
	}
	remember(name, best.symbol->name);
	const GenericInstance *gi_contract = nullptr, *gi_target = nullptr;
	if (record) {
		const ResolvedType *rt_host = best.dispatch && best.dispatch->kind == DispatchKind::State ? &best.dispatch->contract.owner : nullptr;
		gi_target = gi_contract = analyzer.useCallable(*best.contract, best.substitutions, context, span, rt_host);
		if (best.dispatch) analyzer.useDispatch(*best.dispatch, best.substitutions, context, span);
		if (best.symbol != best.contract) {
			Substitutions m_target = *best.overridingArguments;
			if (const TypeParameterList *tpl_method = typeParameters(*best.symbol)) {
				const TypeParameterList &tpl_contract = *typeParameters(*best.contract);
				for (std::size_t st_index = 0; st_index < tpl_method->parameters.size(); ++st_index) {
					m_target.emplace(&tpl_method->parameters[st_index], best.substitutions.at(&tpl_contract.parameters[st_index]));
				}
			}
			gi_target = analyzer.useCallable(*best.symbol, m_target, context, span);
		}
	}
	return { .types                = std::move(results),
		 .symbol               = best.symbol,
		 .binding              = nullptr,
		 .argumentContract     = best.contract,
		 .parameterForArgument = std::move(best.placement.destinations),
		 .omittedParameters    = std::move(best.placement.omitted),
		 .fallsThrough         = b_continues,
		 .typeArguments        = std::move(best.substitutions),
		 .instance             = gi_target,
		 .contractInstance     = gi_contract,
		 .dispatch             = best.dispatch };
}
}
