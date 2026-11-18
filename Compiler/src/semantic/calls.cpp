// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <ranges>

#include <semantic/checker.hpp>
#include <token/token.hpp>

namespace cloth::semantic {
namespace {

std::optional<std::vector<std::size_t>> placeArguments (const std::vector<CallArgument> &args, const std::vector<ParameterDeclaration> &params) {
	std::vector destinations(args.size(), params.size());
	std::vector supplied(params.size(), false);
	std::size_t positional = 0;
	for (std::size_t i = 0; i < args.size(); ++i) {
		if (!args[i].label) {
			++positional;
			continue;
		}
		const auto it =
		        std::ranges::find_if(params, [&] (const ParameterDeclaration &p) { return p.name.get_lexeme() == args[i].label->get_lexeme(); });
		if (it == params.end()) {
			return std::nullopt;
		}
		destinations[i]           = static_cast<std::size_t>(it - params.begin());
		supplied[destinations[i]] = true;
	}
	std::size_t required = 0;
	for (std::size_t i = 0; i < params.size(); ++i) {
		if (!supplied[i] && !params[i].isOptional()) {
			++required;
		}
	}
	std::size_t next = 0;
	for (std::size_t i = 0; i < params.size(); ++i) {
		if (!params[i].isOptional() && !supplied[i]) {
			--required;
		}
		if (next < positional && (!params[i].isOptional() || positional - next > required)) {
			if (supplied[i]) {
				return std::nullopt;
			}
			destinations[next++] = i;
			supplied[i]          = true;
		}
		if (!supplied[i] && !params[i].isOptional()) {
			return std::nullopt;
		}
	}
	if (next != positional) {
		return std::nullopt;
	}
	return destinations;
}

void inferArguments (const ResolvedType &pattern, ResolvedType actual, const TypeParameterList &generic, Substitutions &args) {
	if (actual.name == to_string(kNull)) {
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
	} else if (pattern.declaration == actual.declaration && pattern.arguments.size() == actual.arguments.size()) {
		for (std::size_t i = 0; i < pattern.arguments.size(); ++i) {
			inferArguments(pattern.arguments[i], actual.arguments[i], generic, args);
		}
	}
}

bool contextualFailure (const Code code) {
	return code == Code::TYPE_MISMATCH || code == Code::INTEGER_RANGE || code == Code::FLOAT_RANGE || code == Code::INVALID_ARRAY ||
	       code == Code::INVALID_STRUCT_LITERAL || code == Code::BRANCH_TYPE_MISMATCH || code == Code::INVALID_NULLABLE_TYPE ||
	       code == Code::MIXED_INTEGERS;
}

struct Match {
	const Symbol            *symbol;
	std::vector<std::size_t> destinations;
	ResultTypes              parameters;
	Substitutions            substitutions;
	int                      rank;
	std::size_t              omitted;
};

}

ExpressionInfo BodyChecker::invoke (const Token &name, const TokenSpan &span, const std::vector<CallArgument> &arguments, const std::vector<Candidate> &group,
                                    const TypeArgumentList *typeArguments, const ResultTypes *knownArguments) {
	const std::string &spelling = name.get_lexeme();
	if (group.empty()) {
		fail(Code::UNKNOWN_NAME, name.get_span(), "Unknown callable '" + spelling + "'");
	}
	std::set<std::string> labels;
	for (const CallArgument &arg : arguments) {
		if (!arg.label && !labels.empty()) {
			fail(Code::INVALID_ARGUMENT_BINDING, arg.get_span(), "Positional arguments must precede named arguments");
		}
		if (arg.label && !labels.insert(arg.label->get_lexeme()).second) {
			fail(Code::INVALID_ARGUMENT_BINDING, arg.label->get_span(), "Duplicate argument label");
		}
	}
	std::vector<std::optional<ResolvedType>> explicitTypes;
	if (typeArguments) {
		for (const TypeReference &ref : typeArguments->arguments) {
			explicitTypes.push_back(ref.is_placeholder() ? std::nullopt : std::optional{ analyzer.checkedType(ref, context) });
		}
	}
	std::vector<std::optional<ResolvedType>> ordinary;
	BodyChecker                              natural = *this;
	for (const CallArgument &arg : arguments) {
		if (knownArguments) {
			ordinary.emplace_back(knownArguments->at(ordinary.size()));
			continue;
		}
		try {
			ordinary.emplace_back(natural.scalar(*arg.expression));
		} catch (const cde::ClothDiagnostic &error) {
			if (!contextualFailure(error.get_code())) {
				throw;
			}
			ordinary.emplace_back();
		}
	}
	std::vector<Match> matches;
	bool               placed       = false;
	const Symbol      *inaccessible = nullptr;
	for (const Candidate &candidate : group) {
		const Symbol            &symbol       = *candidate.symbol;
		const CallableSignature &signature    = *callable(symbol);
		const DeclarationInfo   &info         = analyzer.declarations.at(&symbol);
		const auto               destinations = placeArguments(arguments, signature.parameters);
		if (!destinations) {
			continue;
		}
		placed                                 = true;
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
			for (std::size_t i = 0; i < ordinary.size(); ++i) {
				if (ordinary[i]) {
					inferArguments(info.parameters[(*destinations)[i]], *ordinary[i], *generic, substitutions);
				}
			}
		}
		bool viable = true;
		for (std::size_t i = 0; i < count; ++i) {
			const TypeParameterDeclaration &param = generic->parameters[i];
			if (!substitutions.contains(&param) && param.defaultType) {
				substitutions[&param] = substitute(analyzer.type(*param.defaultType, symbol), substitutions);
			}
			if (!substitutions.contains(&param)) {
				viable = false;
				break;
			}
		}
		if (viable && generic) {
			for (const TypeParameterDeclaration &param : generic->parameters) {
				viable &= analyzer.satisfies(param, substitutions, symbol, context);
			}
		}
		if (!viable) {
			continue;
		}
		Match match{ .symbol        = &symbol,
			     .destinations  = *destinations,
			     .parameters    = {},
			     .substitutions = {},
			     .rank          = 0,
			     .omitted       = signature.parameters.size() - arguments.size() };
		for (std::size_t i = 0; i < arguments.size(); ++i) {
			const ResolvedType parameter = substitute(info.parameters[(*destinations)[i]], substitutions);
			match.parameters.push_back(parameter);
			if (ordinary[i] && *ordinary[i] == parameter) {
				continue;
			}
			if (ordinary[i] && analyzer.accepts(parameter, *ordinary[i])) {
				match.rank = std::max(match.rank, 1);
				continue;
			}
			BodyChecker trial = *this;
			trial.record      = false;
			try {
				viable = analyzer.accepts(parameter, trial.scalar(*arguments[i].expression, &parameter));
			} catch (const cde::ClothDiagnostic &error) {
				if (error.get_code() == Code::PRIVATE_CLASS_MEMBER || error.get_code() == Code::PRIVATE_NESTED_TYPE) {
					if (group.size() == 1) {
						throw;
					}
				} else if (!contextualFailure(error.get_code())) {
					throw;
				}
				viable = false;
			}
			if (!viable) {
				break;
			}
			match.rank = 2;
		}
		if (!viable) {
			continue;
		}
		if (!analyzer.accessible(symbol, context)) {
			inaccessible = &symbol;
			continue;
		}
		match.substitutions = std::move(substitutions);
		matches.push_back(std::move(match));
	}
	if (matches.empty()) {
		if (inaccessible) {
			analyzer.requireAccess(*inaccessible, context, span);
		}
		const Code code = placed ? Code::NO_MATCHING_CALL : labels.empty() ? Code::CALL_ARITY : Code::INVALID_ARGUMENT_BINDING;
		fail(code, span, "No overload of '" + spelling + "' accepts these arguments", &group.front().symbol->name);
	}
	const int rank = std::ranges::min_element(matches, {}, &Match::rank)->rank;
	std::erase_if(matches, [rank] (const Match &m) { return m.rank != rank; });
	if (std::ranges::all_of(matches, [&] (const Match &m) { return m.parameters == matches.front().parameters; })) {
		const std::size_t omitted = std::ranges::min_element(matches, {}, &Match::omitted)->omitted;
		std::erase_if(matches, [omitted] (const Match &m) { return m.omitted != omitted; });
		if (std::ranges::any_of(matches, [] (const Match &m) { return !typeParameters(*m.symbol); })) {
			std::erase_if(matches, [] (const Match &m) { return typeParameters(*m.symbol) != nullptr; });
		}
	}
	if (matches.size() != 1) {
		fail(Code::AMBIGUOUS_CALL, span, "Ambiguous call to '" + spelling + "'", &matches.front().symbol->name);
	}
	const Match &best = matches.front();
	for (std::size_t i = 0; i < arguments.size(); ++i) {
		if (!ordinary[i] || !analyzer.accepts(best.parameters[i], *ordinary[i])) {
			expect(*arguments[i].expression, best.parameters[i]);
		}
	}
	ResultTypes results;
	for (const ResolvedType &rt : analyzer.resultsOf(*best.symbol)) {
		results.push_back(substitute(rt, best.substitutions));
	}
	remember(name, best.symbol->name);
	return { .types                = std::move(results),
		 .symbol               = best.symbol,
		 .binding              = nullptr,
		 .parameterForArgument = best.destinations,
		 .fallsThrough         = true,
		 .typeArguments        = best.substitutions };
}

}
