// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/language/expressions/Builtins.rst

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include <ast/expr/expression.hpp>
#include <ast/expr/expressions.hpp>
#include <ast/expr/member_call_expression.hpp>
#include <ast/type/type_reference.hpp>
#include <diagnostics.hpp>
#include <semantic/checked_program.hpp>
#include <semantic/checker.hpp>
#include <token/token.hpp>

namespace cloth::semantic {
namespace {
enum class Family { Function, Character, String, CharacterOrBoolean, CharacterOrString };
enum class Slot { None, Never, Text, NativeInt, Self, Int, Size, NullableSize, Bool, Char, String, NullableString, Chars, Strings, CharOrString };

struct Definition {
	BuiltinOperation    operation;
	std::string_view    name;
	Family              family;
	Slot                result;
	std::array<Slot, 2> parameters   = {};
	bool                optionalLast = false;
};

// These are ordinary identifiers. Only an unresolved function name or a primitive value can select a definition.
constexpr Definition V_FUNCTIONS[] = {
	{ .operation = BuiltinOperation::Print, .name = "print", .family = Family::Function, .result = Slot::None, .parameters = { Slot::Text } },
	{ .operation    = BuiltinOperation::Println,
	  .name         = "println",
	  .family       = Family::Function,
	  .result       = Slot::None,
	  .parameters   = { Slot::Text },
	  .optionalLast = true },
	{ .operation    = BuiltinOperation::Input,
	  .name         = "input",
	  .family       = Family::Function,
	  .result       = Slot::NullableString,
	  .parameters   = { Slot::NullableString },
	  .optionalLast = true },
	{ .operation    = BuiltinOperation::Exit,
	  .name         = "exit",
	  .family       = Family::Function,
	  .result       = Slot::Never,
	  .parameters   = { Slot::NativeInt, Slot::String },
	  .optionalLast = true },
};

constexpr Definition V_METHODS[] = {
	{ .operation = BuiltinOperation::ToInt, .name = "toInt", .family = Family::CharacterOrBoolean, .result = Slot::Int },
	{ .operation = BuiltinOperation::IsWhitespace, .name = "isWhitespace", .family = Family::Character, .result = Slot::Bool },
	{ .operation = BuiltinOperation::IsLetter, .name = "isLetter", .family = Family::Character, .result = Slot::Bool },
	{ .operation = BuiltinOperation::IsDigit, .name = "isDigit", .family = Family::Character, .result = Slot::Bool },
	{ .operation = BuiltinOperation::IsLetterOrDigit, .name = "isLetterOrDigit", .family = Family::Character, .result = Slot::Bool },
	{ .operation = BuiltinOperation::IsUpperCase, .name = "isUpperCase", .family = Family::Character, .result = Slot::Bool },
	{ .operation = BuiltinOperation::IsLowerCase, .name = "isLowerCase", .family = Family::Character, .result = Slot::Bool },
	{ .operation = BuiltinOperation::ToUpperCase, .name = "toUpperCase", .family = Family::CharacterOrString, .result = Slot::Self },
	{ .operation = BuiltinOperation::ToLowerCase, .name = "toLowerCase", .family = Family::CharacterOrString, .result = Slot::Self },
	{ .operation = BuiltinOperation::IsEmpty, .name = "isEmpty", .family = Family::String, .result = Slot::Bool },
	{ .operation = BuiltinOperation::Contains, .name = "contains", .family = Family::String, .result = Slot::Bool, .parameters = { Slot::String } },
	{ .operation = BuiltinOperation::StartsWith, .name = "startsWith", .family = Family::String, .result = Slot::Bool, .parameters = { Slot::String } },
	{ .operation = BuiltinOperation::EndsWith, .name = "endsWith", .family = Family::String, .result = Slot::Bool, .parameters = { Slot::String } },
	{ .operation = BuiltinOperation::CharAt, .name = "charAt", .family = Family::String, .result = Slot::Char, .parameters = { Slot::Size } },
	{ .operation  = BuiltinOperation::Substring,
	  .name       = "substring",
	  .family     = Family::String,
	  .result     = Slot::String,
	  .parameters = { Slot::Size, Slot::Size } },
	{ .operation = BuiltinOperation::IndexOf, .name = "indexOf", .family = Family::String, .result = Slot::NullableSize, .parameters = { Slot::String } },
	{ .operation  = BuiltinOperation::LastIndexOf,
	  .name       = "lastIndexOf",
	  .family     = Family::String,
	  .result     = Slot::NullableSize,
	  .parameters = { Slot::String } },
	{ .operation = BuiltinOperation::ToChars, .name = "toChars", .family = Family::String, .result = Slot::Chars },
	{ .operation = BuiltinOperation::Split, .name = "split", .family = Family::String, .result = Slot::Strings, .parameters = { Slot::CharOrString } },
	{ .operation  = BuiltinOperation::Replace,
	  .name       = "replace",
	  .family     = Family::String,
	  .result     = Slot::String,
	  .parameters = { Slot::String, Slot::String } },
	{ .operation  = BuiltinOperation::ReplaceFirst,
	  .name       = "replaceFirst",
	  .family     = Family::String,
	  .result     = Slot::String,
	  .parameters = { Slot::String, Slot::String } },
	{ .operation  = BuiltinOperation::RemovePrefix,
	  .name       = "removePrefix",
	  .family     = Family::String,
	  .result     = Slot::String,
	  .parameters = { Slot::String } },
	{ .operation  = BuiltinOperation::RemoveSuffix,
	  .name       = "removeSuffix",
	  .family     = Family::String,
	  .result     = Slot::String,
	  .parameters = { Slot::String } },
	{ .operation = BuiltinOperation::Trim, .name = "trim", .family = Family::String, .result = Slot::String },
	{ .operation = BuiltinOperation::TrimStart, .name = "trimStart", .family = Family::String, .result = Slot::String },
	{ .operation = BuiltinOperation::TrimEnd, .name = "trimEnd", .family = Family::String, .result = Slot::String },
	{ .operation = BuiltinOperation::IsBlank, .name = "isBlank", .family = Family::String, .result = Slot::Bool },
};

const Definition *definition (const std::string_view name, const bool method) {
	const std::span<const Definition> sp_definitions = method ? std::span<const Definition>(V_METHODS) : std::span<const Definition>(V_FUNCTIONS);
	const auto                        it_found       = std::ranges::find(sp_definitions, name, &Definition::name);
	return it_found == sp_definitions.end() ? nullptr : &*it_found;
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
	default:
		return false;
	}
}

// A receiver that exits supplies no type. Use the same representative as the primitive catalog for unreachable calls.
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

std::optional<ResolvedType> slotType (const Slot slot, const std::optional<ResolvedType> &receiver) {
	switch (slot) {
	case Slot::Self:
		return receiver;
	case Slot::Int:
		return ResolvedType(KInteger);
	case Slot::Size:
		return ResolvedType(KUsize);
	case Slot::Char:
		return ResolvedType(KCharacter);
	case Slot::Bool:
		return ResolvedType(KBoolean);
	case Slot::String:
		return ResolvedType(KString);
	case Slot::NullableString:
	case Slot::NullableSize: {
		ResolvedType rt_type(slot == Slot::NullableString ? KString : KUsize);
		rt_type.suffixes.emplace_back(toString(OQuestion));
		return rt_type;
	}
	case Slot::Chars:
	case Slot::Strings: {
		ResolvedType rt_type(slot == Slot::Chars ? KCharacter : KString);
		rt_type.suffixes.push_back(arraySuffix());
		return rt_type;
	}
	default:
		return {};
	}
}

}

bool builtinFunctionNamed (const std::string_view name) {
	return definition(name, false) != nullptr;
}

bool builtinMethodNamed (std::string_view name, const ResolvedType &receiver) {
	const Definition *method = definition(name, true);
	return (method && acceptsReceiver(method->family, receiver)) || numericMethodNamed(name, receiver);
}

bool builtinNeverReturns (const std::string_view name) {
	const Definition *d_builtin = definition(name, false);
	return d_builtin && d_builtin->result == Slot::Never;
}

void BodyChecker::checkTypeArguments (const TypeArgumentList &arguments) const {
	for (const TypeReference &tr_type : arguments.arguments)
		if (!tr_type.isPlaceholder() && bodyType(tr_type) == ResolvedType(KVoid))
			fail(Code::InvalidGenericArgument, tr_type.getSpan(), "A type argument must be a value type");
}

void BodyChecker::checkArgumentNames (const CallSite &call, const std::size_t start) const {
	// Preserve name diagnostics in the remaining operands when this call already has a binding error.
	BodyChecker bc_probe       = *this;
	bc_probe.record            = false;
	bc_probe.we_capture        = nullptr;
	bc_probe.ef_probeConstants = nullptr;
	for (std::size_t st_index = start; st_index < call.suppliedArguments.size(); ++st_index) {
		try {
			bc_probe.expression(*call.suppliedArguments[st_index].expression);
		} catch (const cde::ClothDiagnostic &cd_error) {
			if (const Code dc_code = cd_error.getCode(); dc_code == Code::UnknownName || dc_code == Code::NotCallable ||
			                                             dc_code == Code::FunctionValue || dc_code == Code::UnsupportedType ||
			                                             dc_code == Code::InvalidGenericArgument || dc_code == Code::INTERNAL)
				throw;
		}
	}
}

void BodyChecker::checkBuiltinTypeArguments (const CallSite &call, const TypeArgumentList &arguments) const {
	checkTypeArguments(arguments);
	if (dynamic_cast<const MemberCallExpression *>(&call)) checkArgumentNames(call, 0);
	fail(Code::GenericArity, arguments.getSpan(), std::format("'{}' takes no type arguments", call.name.getLexeme()));
}

void BodyChecker::checkBuiltinArguments (const CallSite &call, const TypeArgumentList *typeArguments, const std::size_t minimum,
                                         const std::size_t maximum) const {
	if (typeArguments) checkBuiltinTypeArguments(call, *typeArguments);
	const auto        it_label    = std::ranges::find_if(call.suppliedArguments, [] (const CallArgument &argument) { return argument.label.has_value(); });
	const std::size_t st_supplied = call.suppliedArguments.size();
	if (it_label == call.suppliedArguments.end() && st_supplied >= minimum && st_supplied <= maximum) return;
	checkArgumentNames(call, 0);
	if (it_label != call.suppliedArguments.end())
		fail(Code::InvalidArgumentBinding, it_label->label->getSpan(), std::format("'{}' takes positional arguments", call.name.getLexeme()));
	fail(Code::CallArity, call.name.getSpan(), std::format("No overload of '{}' takes {} arguments", call.name.getLexeme(), st_supplied));
}

std::optional<ExpressionInfo> BodyChecker::builtinCall (const CallSite &call, const TypeArgumentList *typeArguments, const ExpressionInfo *receiver) {
	const Definition *bd_builtin = definition(call.name.getNameKey(), receiver != nullptr);
	if (!bd_builtin) return {};
	const std::size_t           st_maximum = std::ranges::count_if(bd_builtin->parameters, [] (Slot slot) { return slot != Slot::None; });
	std::optional<ResolvedType> opt_receiver;
	if (receiver && receiver->fallsThrough) {
		opt_receiver = receiver->types.front();
		if (!acceptsReceiver(bd_builtin->family, *opt_receiver)) return {};
	}
	const std::size_t st_supplied = call.suppliedArguments.size();
	checkBuiltinArguments(call, typeArguments, st_maximum - bd_builtin->optionalLast, st_maximum);

	if (receiver && !opt_receiver) opt_receiver = representative(bd_builtin->family);
	BuiltinCall                    bc_bound{ .operation = bd_builtin->operation, .receiver = opt_receiver, .parameters = {} };
	bool                           b_continues = !receiver || receiver->fallsThrough;
	std::optional<BodyChecker>     opt_unreachable;
	for (std::size_t st_index = 0; st_index < st_supplied; ++st_index) {
		const Expression           &e_argument    = *call.suppliedArguments[st_index].expression;
		const Slot                  bs_slot       = bd_builtin->parameters[st_index];
		std::optional<ResolvedType> opt_parameter = slotType(bs_slot, opt_receiver);
		BodyChecker                &bc_operand    = onPath(b_continues, opt_unreachable);
		ExpressionInfo              ei_actual     = bc_operand.scalarInfo(e_argument, opt_parameter ? &*opt_parameter : nullptr);
		const ResolvedType         *rt_actual     = ei_actual.fallsThrough ? &ei_actual.types.front() : nullptr;
		bool                        b_accepts     = true;
		std::string                 s_expected;
		switch (bs_slot) {
		case Slot::Text:
			opt_parameter = ResolvedType(KString);
			s_expected    = "a printable value";
			b_accepts     = !rt_actual || bc_operand.bindText(e_argument, *rt_actual);
			break;
		case Slot::NativeInt:
			opt_parameter = ResolvedType(KInteger);
			s_expected    = "an integer";
			b_accepts     = !rt_actual || rt_actual->integer();
			break;
		case Slot::CharOrString:
			opt_parameter = rt_actual && *rt_actual == ResolvedType(KString) ? *rt_actual : ResolvedType(KCharacter);
			s_expected    = std::format("{} or {}", toString(KCharacter), toString(KString));
			b_accepts     = !rt_actual || *opt_parameter == *rt_actual;
			break;
		default:
			s_expected = opt_parameter->spelling();
			b_accepts  = !rt_actual || analyzer.accepts(*opt_parameter, *rt_actual);
			break;
		}
		if (!b_accepts) {
			checkArgumentNames(call, st_index + 1);
			fail(Code::InvalidBuiltinArgument, e_argument.getSpan(),
			     std::format("'{}' requires {}, got {}", bd_builtin->name, s_expected, rt_actual->spelling()));
		}
		bc_bound.parameters.push_back(std::move(*opt_parameter));
		b_continues &= ei_actual.fallsThrough;
	}
	ExpressionInfo ei_result{ .types = {}, .fallsThrough = b_continues && bd_builtin->result != Slot::Never, .builtin = std::move(bc_bound) };
	if (std::optional<ResolvedType> opt_result = slotType(bd_builtin->result, opt_receiver)) ei_result.types.push_back(std::move(*opt_result));
	return ei_result;
}
}
