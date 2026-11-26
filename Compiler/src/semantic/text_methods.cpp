// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <array>
#include <format>
#include <utility>

#include <ast/expr/member_call_expression.hpp>
#include <diagnostics.hpp>
#include <semantic/checker.hpp>

namespace cloth::semantic {
namespace {
enum class Family { Character, String, CharacterOrBoolean, CharacterOrString };
enum class Slot { None, Self, Int, Size, NullableSize, Bool, Char, String, Chars, Strings, CharOrString };

// Match the standard library signatures that carry the positional and operand diagnostics.
struct Definition {
	std::string_view    name;
	Family              family;
	Slot                result;
	std::array<Slot, 2> parameters = {};
};

constexpr Definition METHODS[] = {
	{ .name = "toInt", .family = Family::CharacterOrBoolean, .result = Slot::Int },
	{ .name = "isWhitespace", .family = Family::Character, .result = Slot::Bool },
	{ .name = "isLetter", .family = Family::Character, .result = Slot::Bool },
	{ .name = "isDigit", .family = Family::Character, .result = Slot::Bool },
	{ .name = "isLetterOrDigit", .family = Family::Character, .result = Slot::Bool },
	{ .name = "isUpperCase", .family = Family::Character, .result = Slot::Bool },
	{ .name = "isLowerCase", .family = Family::Character, .result = Slot::Bool },
	{ .name = "toUpperCase", .family = Family::CharacterOrString, .result = Slot::Self },
	{ .name = "toLowerCase", .family = Family::CharacterOrString, .result = Slot::Self },
	{ .name = "isEmpty", .family = Family::String, .result = Slot::Bool },
	{ .name = "contains", .family = Family::String, .result = Slot::Bool, .parameters = { Slot::String } },
	{ .name = "startsWith", .family = Family::String, .result = Slot::Bool, .parameters = { Slot::String } },
	{ .name = "endsWith", .family = Family::String, .result = Slot::Bool, .parameters = { Slot::String } },
	{ .name = "charAt", .family = Family::String, .result = Slot::Char, .parameters = { Slot::Size } },
	{ .name = "substring", .family = Family::String, .result = Slot::String, .parameters = { Slot::Size, Slot::Size } },
	{ .name = "indexOf", .family = Family::String, .result = Slot::NullableSize, .parameters = { Slot::String } },
	{ .name = "lastIndexOf", .family = Family::String, .result = Slot::NullableSize, .parameters = { Slot::String } },
	{ .name = "toChars", .family = Family::String, .result = Slot::Chars },
	{ .name = "split", .family = Family::String, .result = Slot::Strings, .parameters = { Slot::CharOrString } },
	{ .name = "replace", .family = Family::String, .result = Slot::String, .parameters = { Slot::String, Slot::String } },
	{ .name = "replaceFirst", .family = Family::String, .result = Slot::String, .parameters = { Slot::String, Slot::String } },
	{ .name = "removePrefix", .family = Family::String, .result = Slot::String, .parameters = { Slot::String } },
	{ .name = "removeSuffix", .family = Family::String, .result = Slot::String, .parameters = { Slot::String } },
	{ .name = "trim", .family = Family::String, .result = Slot::String },
	{ .name = "trimStart", .family = Family::String, .result = Slot::String },
	{ .name = "trimEnd", .family = Family::String, .result = Slot::String },
	{ .name = "isBlank", .family = Family::String, .result = Slot::Bool },
};

const Definition *definition (std::string_view name) {
	const auto it = std::ranges::find(METHODS, name, &Definition::name);
	return it == std::end(METHODS) ? nullptr : it;
}

bool acceptsReceiver (const Family family, const ResolvedType &type) {
	if (type.declaration || !type.suffixes.empty()) return false;
	switch (family) {
	case Family::Character:
		return type == ResolvedType(KCharacter);
	case Family::String:
		return type == ResolvedType(KString);
	case Family::CharacterOrBoolean:
		return type == ResolvedType(KCharacter) || type == ResolvedType(KBoolean);
	case Family::CharacterOrString:
		return type == ResolvedType(KCharacter) || type == ResolvedType(KString);
	}
	return false;
}

ResolvedType representative (const Family family) {
	switch (family) {
	case Family::Character:
		return ResolvedType(KCharacter);
	case Family::CharacterOrBoolean:
		return ResolvedType(KBoolean);
	case Family::String:
	case Family::CharacterOrString:
		return ResolvedType(KString);
	default:
		return ResolvedType(KInteger);
	}
}

ResolvedType slotType (Slot slot, const ResolvedType &self) {
	switch (slot) {
	case Slot::Self:
		return self;
	case Slot::Int:
		return ResolvedType(KInteger);
	case Slot::Size:
		return ResolvedType(KUsize);
	case Slot::Bool:
		return ResolvedType(KBoolean);
	case Slot::Char:
		return ResolvedType(KCharacter);
	case Slot::String:
		return ResolvedType(KString);
	case Slot::NullableSize: {
		ResolvedType size(KUsize);
		size.suffixes.emplace_back(toString(OQuestion));
		return size;
	}
	case Slot::Chars:
	case Slot::Strings: {
		ResolvedType array(slot == Slot::Chars ? KCharacter : KString);
		array.suffixes.emplace_back(arraySuffix());
		return array;
	}
	default:
		return ResolvedType{};
	}
}

bool matchesContract (const Analyzer &analyzer, const Candidate &candidate, const Definition &contract, const ResolvedType &self, std::size_t arity) {
	const auto &decl = analyzer.declarations.at(candidate.symbol);
	if (typeParameters(*candidate.symbol) || std::ranges::any_of(callable(*candidate.symbol)->parameters, &ParameterDeclaration::isOptional) ||
	    decl.parameters.size() != arity || decl.returns.size() != 1 ||
	    analyzer.substitute(decl.returns.front(), candidate.arguments) != slotType(contract.result, self))
		return false;
	for (std::size_t i = 0; i < arity; ++i) {
		const Slot slot      = contract.parameters[i];
		const auto parameter = analyzer.substitute(decl.parameters[i], candidate.arguments);
		if (slot == Slot::CharOrString) {
			if (parameter != ResolvedType(KCharacter) && parameter != ResolvedType(KString)) return false;
		} else if (parameter != slotType(slot, self))
			return false;
	}
	return true;
}
}

bool textMethodNamed (std::string_view name, const ResolvedType &receiver) {
	const auto *method = definition(name);
	return method && acceptsReceiver(method->family, receiver);
}

std::optional<ExpressionInfo> BodyChecker::textMethodCall (const MemberCallExpression &call, const ExpressionInfo &receiver) {
	const auto *contract = definition(call.name.getNameKey());
	if (!contract) return {};
	ResolvedType self = receiver.fallsThrough ? receiver.types.front() : representative(contract->family);
	if (!acceptsReceiver(contract->family, self)) return {};
	auto methods = analyzer.members(self, contract->name, SymbolKind::Method);
	if (methods.empty() && !receiver.fallsThrough && (contract->family == Family::CharacterOrBoolean || contract->family == Family::CharacterOrString)) {
		self    = ResolvedType(KCharacter);
		methods = analyzer.members(self, contract->name, SymbolKind::Method);
	}
	if (methods.empty()) return {};
	const std::size_t          arity         = std::ranges::count_if(contract->parameters, [] (Slot slot) { return slot != Slot::None; });
	const auto                *typeArguments = call.typeArguments ? &*call.typeArguments : nullptr;
	const auto                 canonical     = [&] (const Candidate &candidate) { return matchesContract(analyzer, candidate, *contract, self, arity); };
	std::optional<BodyChecker> unreachable;
	if (!std::ranges::all_of(methods, canonical)) {
		auto result = onPath(receiver.fallsThrough, unreachable).invoke(call.name, call.getSpan(), call.suppliedArguments, methods, typeArguments);
		result.fallsThrough &= receiver.fallsThrough;
		const auto selected = std::ranges::find(methods, result.symbol, &Candidate::symbol);
		if (selected != methods.end() && canonical(*selected)) checkBuiltinArguments(call, typeArguments, arity, arity);
		return result;
	}
	checkBuiltinArguments(call, typeArguments, arity, arity);
	ResultTypes parameters;
	bool        continues = receiver.fallsThrough;
	for (std::size_t i = 0; i < arity; ++i) {
		const Expression    &argument  = *call.suppliedArguments[i].expression;
		const bool           separator = methods.size() > 1 && contract->parameters[i] == Slot::CharOrString;
		const auto          &method    = methods.front();
		ResolvedType         parameter = separator ? ResolvedType(KCharacter) :
		                                             analyzer.substitute(analyzer.declarations.at(method.symbol).parameters[i], method.arguments);
		const ExpressionInfo actual    = onPath(continues, unreachable).scalarInfo(argument, separator ? nullptr : &parameter);
		if (actual.fallsThrough) {
			const auto &type = actual.types.front();
			if (separator && type == ResolvedType(KString)) parameter = type;
			if (separator ? parameter != type : !analyzer.accepts(parameter, type)) {
				checkArgumentNames(call, i + 1);
				const auto expected = separator ? std::format("{} or {}", toString(KCharacter), toString(KString)) : parameter.spelling();
				fail(Code::InvalidBuiltinArgument, argument.getSpan(),
				     std::format("'{}' requires {}, got {}", contract->name, expected, type.spelling()));
			}
		}
		parameters.push_back(std::move(parameter));
		continues &= actual.fallsThrough;
	}
	auto result = onPath(continues, unreachable).invoke(call.name, call.getSpan(), call.suppliedArguments, methods, nullptr, &parameters);
	result.fallsThrough &= continues;
	return result;
}
}
