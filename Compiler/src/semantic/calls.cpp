// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <ranges>
#include <variant>

#include <semantic/checker.hpp>
#include <token/token.hpp>

namespace cloth::semantic {
bool contextualFailure (const cde::ClothDiagnostic &error) {
	if (dynamic_cast<const IntrinsicDiagnostic *>(&error)) return false;
	switch (error.get_code()) {
	case Code::TYPE_MISMATCH:
	case Code::INTEGER_RANGE:
	case Code::FLOAT_RANGE:
	case Code::INTEGER_DIVISION_OVERFLOW:
	case Code::NEGATIVE_EXPONENT:
	case Code::INVALID_CAST:
	case Code::UNARY_OPERAND:
	case Code::BINARY_OPERANDS:
	case Code::MIXED_INTEGERS:
	case Code::INCOMPARABLE_TYPES:
	case Code::INVALID_ARRAY:
	case Code::INVALID_INDEX:
	case Code::INVALID_STRUCT_LITERAL:
	case Code::INVALID_STRUCT_FIELD:
	case Code::INVALID_RANGE_VALUE:
	case Code::INVALID_RANGE_MEMBERSHIP:
	case Code::INVALID_NULLABLE_TYPE:
	case Code::EXPLICIT_NULLABLE_TYPE:
	case Code::MISSING_ELSE:
	case Code::NON_BOOLEAN_CONDITION:
	case Code::MISSING_BRANCH_VALUE:
	case Code::BRANCH_TYPE_MISMATCH:
	case Code::BRANCH_RESULT_ARITY:
	case Code::VOID_VALUE:
		return true;
	default:
		return false;
	}
}

bool acceptsTypeArgumentCount (const TypeParameterList *parameters, std::size_t count) {
	return parameters && count <= parameters->parameters.size() &&
	       std::ranges::all_of(parameters->parameters | std::views::drop(count),
	                           [] (const TypeParameterDeclaration &parameter) { return parameter.defaultType.has_value(); });
}

void rejectTypeArgumentCount (const TypeArgumentList &arguments, const Symbol &declaration, bool generic) {
	if (arguments.arguments.empty() && !generic)
		fail(Code::INVALID_GENERIC_ARGUMENT, arguments.get_span(), "This declaration has no type parameters", &declaration.name);
	fail(Code::GENERIC_ARITY, arguments.get_span(), "No overload accepts this number of type arguments", &declaration.name);
}

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
		        v_parameters, [&] (const ParameterDeclaration &parameter) { return parameter.name.get_name_key() == opt_label->get_name_key(); });
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

using InferenceSlots = std::set<const TypeParameterDeclaration *>;
using EvidenceSpans  = std::map<const TypeParameterDeclaration *, TokenSpan>;

struct Inference {
	const Analyzer &analyzer;
	Substitutions  &arguments;
	EvidenceSpans   spans;
	bool            report;

	bool collect (const ResolvedType &pattern, ResolvedType actual, const InferenceSlots &slots, const TokenSpan &use,
	              std::set<std::pair<ResolvedType, ResolvedType>> &seen) {
		if (!pattern.dependent() || actual.name == to_string(kNull) || !seen.emplace(pattern, actual).second) return true;
		for (const std::string &s_suffix : std::views::reverse(pattern.suffixes)) {
			if (s_suffix == to_string(oQuestion)) {
				if (actual.nullable()) actual = actual.element();
			} else {
				if (!actual.array()) return true;
				actual = actual.element();
			}
		}
		if (pattern.parameter) {
			if (!slots.contains(pattern.parameter)) return true;
			const std::pair<Substitutions::iterator, bool> it_argument = arguments.emplace(pattern.parameter, actual);
			if (it_argument.second) spans.emplace(pattern.parameter, use);
			if (it_argument.second || it_argument.first->second == actual) return true;
			if (report) {
				const Token &t_parameter = pattern.parameter->name;
				throw cde::ClothDiagnostic(Code::GENERIC_INFERENCE_FAILED, "Conflicting types for '" + t_parameter.get_lexeme() + "'",
				                           { { spans.at(pattern.parameter), "infers " + it_argument.first->second.spelling() },
				                             { use, "infers " + actual.spelling() },
				                             { t_parameter.get_span(), "type parameter declared here" } });
			}
			return false;
		}
		if (pattern.arguments().empty() || !actual.suffixes.empty()) return true;
		const std::function<bool(const ResolvedType &)> match = [&] (const ResolvedType &view) {
			for (std::size_t st_slot = 0; st_slot < pattern.arguments().size(); ++st_slot)
				if (!collect(pattern.arguments()[st_slot], view.arguments()[st_slot], slots, use, seen)) return false;
			return true;
		};
		if (pattern.declaration) return std::ranges::all_of(analyzer.applicationsOf(actual, *pattern.declaration), match);
		return actual.name != pattern.name || actual.arguments().size() != pattern.arguments().size() || match(actual);
	}
};

struct PlacedCandidate {
	const Candidate         *candidate;
	ArgumentPlacement        placement;
	const TypeParameterList *parameters;
};

enum class InferenceTier { Typed, TypedArray, Literal };

struct ArgumentEvidence {
	TokenSpan                   use;
	std::optional<ResolvedType> type;
	InferenceTier               tier = InferenceTier::Typed;
};

std::vector<PlacedCandidate> bindCandidates (const Token &name, const TokenSpan &span, const std::vector<CallArgument> &arguments,
                                             const std::vector<Candidate> &group, bool named, const TypeArgumentList *written) {
	std::vector<PlacedCandidate>  v_placed;
	std::optional<BindingProblem> opt_firstProblem;
	bool                          b_optional = false;
	bool                          b_generic  = false;
	for (const Candidate &c_candidate : group) {
		const TypeParameterList *tpl_parameters = c_candidate.inferOwner ? c_candidate.inferOwner : typeParameters(*c_candidate.symbol);
		b_generic |= tpl_parameters != nullptr;
		if (written && !acceptsTypeArgumentCount(tpl_parameters, written->arguments.size())) continue;
		PlacementResult pr_result = placeArguments(arguments, *c_candidate.symbol, span);
		if (ArgumentPlacement *ap_placement = std::get_if<ArgumentPlacement>(&pr_result)) {
			v_placed.push_back({ &c_candidate, std::move(*ap_placement), tpl_parameters });
		} else {
			if (!opt_firstProblem) opt_firstProblem = std::get<BindingProblem>(std::move(pr_result));
			b_optional |= std::ranges::any_of(callable(*c_candidate.symbol)->parameters, &ParameterDeclaration::isOptional);
		}
	}
	if (v_placed.empty()) {
		if (!opt_firstProblem) rejectTypeArgumentCount(*written, *group.front().symbol, b_generic);
		if (named || b_optional) fail(Code::INVALID_ARGUMENT_BINDING, opt_firstProblem->span, opt_firstProblem->message, opt_firstProblem->declaration);
		fail(Code::CALL_ARITY, span, "No overload of '" + name.get_lexeme() + "' accepts these arguments", &group.front().symbol->name);
	}
	return v_placed;
}

bool inferCall (const Analyzer &analyzer, const PlacedCandidate &placed, const Symbol &context, const TokenSpan &span, const TypeArgumentList *written,
                const std::vector<std::optional<ResolvedType>> &explicitTypes, const std::vector<ArgumentEvidence> &evidence, bool report,
                Substitutions &arguments, std::optional<cde::ClothDiagnostic> &firstBoundFailure) {
	const std::vector<TypeParameterDeclaration> &v_parameters    = placed.parameters->parameters;
	const Symbol                                &sym_declaration = *placed.candidate->symbol;
	const ResultTypes                           &v_patterns      = analyzer.declarations.at(&sym_declaration).parameters;
	Inference                                    in_match{ analyzer, arguments, {}, report };
	InferenceSlots                               s_slots;
	for (std::size_t st_slot = 0; st_slot < v_parameters.size(); ++st_slot) {
		const TypeParameterDeclaration *tp_parameter = &v_parameters[st_slot];
		if (st_slot < explicitTypes.size() && explicitTypes[st_slot]) {
			arguments[tp_parameter] = *explicitTypes[st_slot];
			in_match.spans.emplace(tp_parameter, written->arguments[st_slot].get_span());
		} else if ((!written || st_slot < explicitTypes.size()) && !arguments.contains(tp_parameter)) {
			s_slots.insert(tp_parameter);
		}
	}
	// Evidence within a tier must agree. Later tiers can only fill slots the earlier arguments left open.
	for (InferenceTier it_tier : { InferenceTier::Typed, InferenceTier::TypedArray, InferenceTier::Literal }) {
		if (s_slots.empty()) break;
		for (std::size_t st_index = 0; st_index < evidence.size(); ++st_index) {
			const ArgumentEvidence &ae_value = evidence[st_index];
			if (!ae_value.type || ae_value.tier != it_tier) continue;
			std::set<std::pair<ResolvedType, ResolvedType>> s_seen;
			if (!in_match.collect(v_patterns[placed.placement.destinations[st_index]], *ae_value.type, s_slots, ae_value.use, s_seen)) return false;
		}
		std::erase_if(s_slots, [&] (const TypeParameterDeclaration *parameter) { return arguments.contains(parameter); });
	}
	for (const TypeParameterDeclaration &tp_parameter : v_parameters) {
		if (arguments.contains(&tp_parameter)) continue;
		if (tp_parameter.defaultType) {
			arguments.emplace(&tp_parameter, analyzer.substitute(analyzer.type(*tp_parameter.defaultType, sym_declaration), arguments));
			continue;
		}
		if (report)
			fail(Code::GENERIC_INFERENCE_FAILED, written ? written->get_span() : span,
			     "Cannot infer type argument '" + tp_parameter.name.get_lexeme() + "'", &tp_parameter.name);
		return false;
	}
	try {
		for (const TypeParameterDeclaration &tp_parameter : v_parameters) {
			const EvidenceSpans::const_iterator it_span = in_match.spans.find(&tp_parameter);
			analyzer.checkBounds(tp_parameter, arguments, context, it_span == in_match.spans.end() ? span : it_span->second);
		}
	} catch (const cde::ClothDiagnostic &error) {
		if (report || (error.get_code() != Code::UNSATISFIED_GENERIC_BOUND && error.get_code() != Code::CONFLICTING_INTERFACE_APPLICATION)) throw;
		if (!firstBoundFailure) firstBoundFailure = error;
		return false;
	}
	return true;
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
	unsigned                    operatorWidth;
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
		// A nested call's written arguments determine its return contract.
		if (b_sameInput && !b_fits && dynamic_cast<const CallSite *>(&e_argument)) {
			require(ap_probe.result->types, { rt_parameter }, e_argument.get_span());
		}
		if (b_fits && !record && !we_capture && (!contextual || dynamic_cast<const CallSite *>(&e_argument))) {
			// Reuse an already typed operand only with the same incoming facts, then carry its effects to the next written argument.
			bc_operand.fs_flow = ap_probe.after;
			b_continues &= ap_probe.result->fallsThrough;
			continue;
		}
		const std::optional<ResolvedType> opt_actual = bc_operand.scalar(e_argument, contextual ? &rt_parameter : nullptr, FloatContext{ contextual });
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
	receiverClassBound(*site.receiver, receiver);
	std::vector<Candidate> v_candidates = analyzer.members(receiver, site.name.get_name_key(), SymbolKind::Method, MemberLookup::CallContract);
	if (!m_arguments.empty()) {
		using Facts                             = std::unordered_map<const Expression *, ExpressionInfo>;
		const Facts                &m_open      = openFacts().expressions;
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

bool validateArgumentLabels (const std::vector<CallArgument> &arguments) {
	std::map<std::string, const Token *> labels;
	for (const CallArgument &arg : arguments) {
		if (!arg.label && !labels.empty()) {
			fail(Code::INVALID_ARGUMENT_BINDING, arg.get_span(), "Positional arguments must precede named arguments");
		}
		if (arg.label) {
			const std::pair<decltype(labels)::iterator, bool> it_inserted = labels.emplace(arg.label->get_name_key(), &*arg.label);
			if (!it_inserted.second) {
				fail(Code::INVALID_ARGUMENT_BINDING, arg.label->get_span(), "Duplicate argument label", it_inserted.first->second);
			}
		}
	}
	return !labels.empty();
}

ExpressionInfo BodyChecker::bindCall (const Token &name, const TokenSpan &span, const std::vector<CallArgument> &arguments, const std::vector<Candidate> &group,
                                      const TypeArgumentList *typeArguments, const ResultTypes *knownArguments, const ReceiverProofs &proofs) {
	const std::string &spelling = name.get_lexeme();
	if (group.empty()) unknownName(name);
	std::vector<PlacedCandidate>                 v_placed = bindCandidates(name, span, arguments, group, validateArgumentLabels(arguments), typeArguments);
	const std::vector<PlacedCandidate>::iterator it_firstGeneric =
	        std::stable_partition(v_placed.begin(), v_placed.end(), [] (const PlacedCandidate &placed) { return !placed.parameters; });
	const bool                               b_singleTemplate = v_placed.end() - it_firstGeneric == 1;
	std::vector<std::optional<ResolvedType>> explicitTypes;
	if (typeArguments) {
		for (const TypeReference &ref : typeArguments->arguments) {
			explicitTypes.push_back(ref.is_placeholder() ? std::nullopt : std::optional{ bodyType(ref) });
			if (explicitTypes.back() && explicitTypes.back()->name == to_string(kVoid))
				fail(Code::INVALID_GENERIC_ARGUMENT, ref.get_span(), "A type argument must be a value type");
		}
	}
	std::vector<ArgumentProbe>    v_probes;
	std::vector<ArgumentEvidence> v_evidence;
	ExpressionFacts               ef_natural;
	BodyChecker                   natural         = *this;
	natural.record                                = false;
	natural.we_capture                            = nullptr;
	natural.ef_probeConstants                     = &ef_natural;
	bool                       b_naturalContinues = true;
	std::optional<BodyChecker> opt_unreachable;
	for (const CallArgument &arg : arguments) {
		BodyChecker                &bc_input = natural.onPath(b_naturalContinues, opt_unreachable);
		ArgumentProbe               ap_probe{ .result = {}, .before = bc_input.fs_flow, .after = bc_input.fs_flow };
		std::optional<ResolvedType> opt_evidence;
		if (knownArguments) {
			ap_probe.result = ExpressionInfo{ { knownArguments->at(v_probes.size()) } };
		} else {
			BodyChecker bc_operand = bc_input;
			try {
				ap_probe.result              = bc_operand.expression(*arg.expression);
				const ResultKind rk_argument = ap_probe.result->resultKind();
				if (rk_argument == ResultKind::Empty)
					fail(Code::NO_MATCHING_CALL, span, "No overload of '" + spelling + "' accepts an argument that produces no value",
					     &group.front().symbol->name);
				if (rk_argument == ResultKind::Multiple)
					fail(Code::MULTIPLE_RESULTS_IN_SCALAR, arg.expression->get_span(), "A call argument requires one value");
				ap_probe.after = bc_input.fs_flow = std::move(bc_operand.fs_flow);
				b_naturalContinues &= ap_probe.result->fallsThrough;
			} catch (const cde::ClothDiagnostic &error) {
				if (!contextualFailure(error)) throw;
				if (it_firstGeneric != v_placed.end()) {
					BodyChecker bc_default = bc_input;
					try {
						if (error.get_code() == Code::INTEGER_RANGE || error.get_code() == Code::FLOAT_RANGE) {
							bc_default.b_inferenceDefaults = true;
							opt_evidence                   = bc_default.scalar(*arg.expression);
						} else if (error.get_code() == Code::EXPLICIT_NULLABLE_TYPE) {
							// The inferred parameter supplies a destination for an array of possibly absent elements.
							const std::optional<ResolvedType> opt_array = bc_default.elementHint(*arg.expression, {});
							if (opt_array && opt_array->array()) opt_evidence = bc_default.scalar(*arg.expression, &*opt_array);
						}
					} catch (const cde::ClothDiagnostic &failure) {
						if (!contextualFailure(failure)) throw;
					}
				}
			}
		}
		if (it_firstGeneric != v_placed.end()) {
			const ElementEvidence ee_kind = knownArguments ? ElementEvidence::Typed : elementEvidence(*arg.expression);
			if (ee_kind != ElementEvidence::None && ap_probe.result && ap_probe.result->fallsThrough) opt_evidence = ap_probe.result->types.front();
			const InferenceTier it_tier = ee_kind == ElementEvidence::Literal                                         ? InferenceTier::Literal :
			                              !knownArguments && dynamic_cast<const ArrayLiteral *>(arg.expression.get()) ? InferenceTier::TypedArray :
			                                                                                                            InferenceTier::Typed;
			v_evidence.push_back({ arg.get_span(), std::move(opt_evidence), it_tier });
		}
		v_probes.push_back(std::move(ap_probe));
	}
	std::vector<Match>                            matches;
	const Symbol                                 *inaccessible = nullptr;
	std::optional<cde::ClothDiagnostic>           opt_boundFailure;
	bool                                          b_inferred = false;
	std::vector<std::pair<Match, const Symbol *>> v_contextual;
	const std::function                           tryContextual = [&] {
                // An exact or assignable ordinary match ends this overload group's search.
                if (matches.empty()) {
                        for (std::pair<Match, const Symbol *> &entry : v_contextual) {
                                Match          &match = entry.first;
                                ExpressionFacts ef_trial;
                                BodyChecker     trial   = *this;
                                trial.record            = false;
                                trial.we_capture        = nullptr;
                                trial.ef_probeConstants = &ef_trial;
                                try {
                                        trial.checkArguments(arguments, match.parameters, v_probes, true);
                                } catch (const cde::ClothDiagnostic &error) {
                                        const bool b_private = error.get_code() == Code::PRIVATE_CLASS_MEMBER || error.get_code() == Code::PRIVATE_NESTED_TYPE;
                                        if (b_private && group.size() > 1 && !dynamic_cast<const IntrinsicDiagnostic *>(&error)) continue;
                                        if (contextualFailure(error)) continue;
                                        throw;
                                }
                                if (entry.second)
                                        inaccessible = entry.second;
                                else
                                        matches.push_back(std::move(match));
                        }
                }
                v_contextual.clear();
	};
	for (std::vector<PlacedCandidate>::iterator it_placed = v_placed.begin(); it_placed != v_placed.end(); ++it_placed) {
		if (it_placed == it_firstGeneric) {
			tryContextual();
			if (!matches.empty()) break;
		}
		PlacedCandidate       &pc_placed     = *it_placed;
		const Candidate       &candidate     = *pc_placed.candidate;
		ArgumentPlacement     &ap_placement  = pc_placed.placement;
		const Symbol          &symbol        = *candidate.symbol;
		const DeclarationInfo &info          = analyzer.declarations.at(&symbol);
		Substitutions          substitutions = candidate.arguments;
		if (pc_placed.parameters) {
			if (!inferCall(analyzer, pc_placed, context, span, typeArguments, explicitTypes, v_evidence, b_singleTemplate, substitutions,
			               opt_boundFailure))
				continue;
			b_inferred = true;
		}
		Match match{ .symbol              = candidate.overridingMethod ? candidate.overridingMethod : &symbol,
			     .contract            = &symbol,
			     .placement           = std::move(ap_placement),
			     .parameters          = {},
			     .substitutions       = {},
			     .overridingArguments = &candidate.overridingArguments,
			     .dispatch            = candidate.dispatch,
			     .operatorWidth       = candidate.operatorWidth,
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
		const Symbol *sym_private = nullptr;
		if (!analyzer.accessible(candidate, context))
			sym_private = &symbol;
		else if (!candidate.dispatch && candidate.overridingMethod && !analyzer.accessible(*candidate.overridingMethod, context))
			sym_private = candidate.overridingMethod;
		match.substitutions = std::move(substitutions);
		if (match.rank == MatchRank::Contextual)
			v_contextual.emplace_back(std::move(match), sym_private);
		else if (sym_private)
			inaccessible = sym_private;
		else
			matches.push_back(std::move(match));
	}
	tryContextual();
	if (matches.empty()) {
		if (inaccessible) {
			analyzer.requireAccess(*inaccessible, context, span);
		}
		if (!b_inferred && opt_boundFailure) throw *opt_boundFailure;
		fail(Code::NO_MATCHING_CALL, span, "No overload of '" + spelling + "' accepts these arguments", &group.front().symbol->name);
	}
	const MatchRank mr_best = std::ranges::min_element(matches, {}, &Match::rank)->rank;
	std::erase_if(matches, [mr_best] (const Match &match) { return match.rank != mr_best; });
	if (std::ranges::all_of(matches, [&] (const Match &m) { return m.parameters == matches.front().parameters; })) {
		const std::size_t st_omitted = std::ranges::min_element(matches, {}, &Match::omittedCount)->omittedCount();
		std::erase_if(matches, [st_omitted] (const Match &match) { return match.omittedCount() != st_omitted; });
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
	if (best.operatorWidth)
		results = analyzer.widthSignature(*best.contract, best.substitutions, best.operatorWidth).returns;
	else
		for (const ResolvedType &rt : analyzer.resultsOf(*best.contract)) results.push_back(analyzer.substitute(rt, best.substitutions));
	remember(name, best.symbol->name);
	const GenericInstance *gi_contract = nullptr, *gi_target = nullptr;
	if (record) {
		const ResolvedType *rt_host = best.dispatch && best.dispatch->kind == DispatchKind::State ? &best.dispatch->contract.owner : nullptr;
		const TokenSpan     ts_use  = typeArguments ? typeArguments->get_span() : span;
		gi_target = gi_contract = analyzer.useCallable(*best.contract, best.substitutions, context, ts_use, rt_host, best.operatorWidth);
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
