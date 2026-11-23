// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/language/expressions/Builtins.rst

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <span>

#include <semantic/checker.hpp>

namespace cloth::semantic {
namespace {
enum class Family { Function, Numeric, Integer, Character, String, CharacterOrBoolean, CharacterOrString };
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
	{ BuiltinOperation::Print, "print", Family::Function, Slot::None, { Slot::Text } },
	{ BuiltinOperation::Println, "println", Family::Function, Slot::None, { Slot::Text }, true },
	{ BuiltinOperation::Input, "input", Family::Function, Slot::NullableString, { Slot::NullableString }, true },
	{ BuiltinOperation::Exit, "exit", Family::Function, Slot::Never, { Slot::NativeInt, Slot::String }, true },
};

constexpr Definition V_METHODS[] = {
	{ BuiltinOperation::Abs, "abs", Family::Numeric, Slot::Self },
	{ BuiltinOperation::Clamp, "clamp", Family::Numeric, Slot::Self, { Slot::Self, Slot::Self } },
	{ BuiltinOperation::Signum, "signum", Family::Numeric, Slot::Int },
	{ BuiltinOperation::ToStringRadix, "toStringRadix", Family::Integer, Slot::String, { Slot::Int } },
	{ BuiltinOperation::LeadingZeros, "leadingZeros", Family::Integer, Slot::Size },
	{ BuiltinOperation::TrailingZeros, "trailingZeros", Family::Integer, Slot::Size },
	{ BuiltinOperation::ToInt, "toInt", Family::CharacterOrBoolean, Slot::Int },
	{ BuiltinOperation::IsWhitespace, "isWhitespace", Family::Character, Slot::Bool },
	{ BuiltinOperation::IsLetter, "isLetter", Family::Character, Slot::Bool },
	{ BuiltinOperation::IsDigit, "isDigit", Family::Character, Slot::Bool },
	{ BuiltinOperation::IsLetterOrDigit, "isLetterOrDigit", Family::Character, Slot::Bool },
	{ BuiltinOperation::IsUpperCase, "isUpperCase", Family::Character, Slot::Bool },
	{ BuiltinOperation::IsLowerCase, "isLowerCase", Family::Character, Slot::Bool },
	{ BuiltinOperation::ToUpperCase, "toUpperCase", Family::CharacterOrString, Slot::Self },
	{ BuiltinOperation::ToLowerCase, "toLowerCase", Family::CharacterOrString, Slot::Self },
	{ BuiltinOperation::IsEmpty, "isEmpty", Family::String, Slot::Bool },
	{ BuiltinOperation::Contains, "contains", Family::String, Slot::Bool, { Slot::String } },
	{ BuiltinOperation::StartsWith, "startsWith", Family::String, Slot::Bool, { Slot::String } },
	{ BuiltinOperation::EndsWith, "endsWith", Family::String, Slot::Bool, { Slot::String } },
	{ BuiltinOperation::CharAt, "charAt", Family::String, Slot::Char, { Slot::Size } },
	{ BuiltinOperation::Substring, "substring", Family::String, Slot::String, { Slot::Size, Slot::Size } },
	{ BuiltinOperation::IndexOf, "indexOf", Family::String, Slot::NullableSize, { Slot::String } },
	{ BuiltinOperation::LastIndexOf, "lastIndexOf", Family::String, Slot::NullableSize, { Slot::String } },
	{ BuiltinOperation::ToChars, "toChars", Family::String, Slot::Chars },
	{ BuiltinOperation::Split, "split", Family::String, Slot::Strings, { Slot::CharOrString } },
	{ BuiltinOperation::Replace, "replace", Family::String, Slot::String, { Slot::String, Slot::String } },
	{ BuiltinOperation::ReplaceFirst, "replaceFirst", Family::String, Slot::String, { Slot::String, Slot::String } },
	{ BuiltinOperation::RemovePrefix, "removePrefix", Family::String, Slot::String, { Slot::String } },
	{ BuiltinOperation::RemoveSuffix, "removeSuffix", Family::String, Slot::String, { Slot::String } },
	{ BuiltinOperation::Trim, "trim", Family::String, Slot::String },
	{ BuiltinOperation::TrimStart, "trimStart", Family::String, Slot::String },
	{ BuiltinOperation::TrimEnd, "trimEnd", Family::String, Slot::String },
	{ BuiltinOperation::IsBlank, "isBlank", Family::String, Slot::Bool },
};

const Definition *definition (std::string_view name, bool method) {
	const std::span<const Definition>           sp_definitions = method ? std::span<const Definition>(V_METHODS) : std::span<const Definition>(V_FUNCTIONS);
	const std::span<const Definition>::iterator it_found       = std::ranges::find(sp_definitions, name, &Definition::name);
	return it_found == sp_definitions.end() ? nullptr : &*it_found;
}

bool acceptsReceiver (Family family, const ResolvedType &type) {
	if (type.declaration || type.parameter || !type.suffixes.empty()) return false;
	switch (family) {
	case Family::Numeric:
		return type.integer() || type.floating();
	case Family::Integer:
		return type.integer();
	case Family::Character:
		return type == ResolvedType(kCharacter);
	case Family::String:
		return type == ResolvedType(kString);
	case Family::CharacterOrBoolean:
		return type == ResolvedType(kCharacter) || type == ResolvedType(kBoolean);
	case Family::CharacterOrString:
		return type == ResolvedType(kCharacter) || type == ResolvedType(kString);
	default:
		return false;
	}
}

// A receiver that exits supplies no type. Use the same representative as the primitive catalog for unreachable calls.
ResolvedType representative (Family family) {
	switch (family) {
	case Family::Character:
		return ResolvedType(kCharacter);
	case Family::CharacterOrBoolean:
		return ResolvedType(kBoolean);
	case Family::String:
	case Family::CharacterOrString:
		return ResolvedType(kString);
	default:
		return ResolvedType(kInteger);
	}
}

std::optional<ResolvedType> slotType (Slot slot, const std::optional<ResolvedType> &receiver) {
	switch (slot) {
	case Slot::Self:
		return receiver;
	case Slot::Int:
		return ResolvedType(kInteger);
	case Slot::Size:
		return ResolvedType(kUsize);
	case Slot::Char:
		return ResolvedType(kCharacter);
	case Slot::Bool:
		return ResolvedType(kBoolean);
	case Slot::String:
		return ResolvedType(kString);
	case Slot::NullableString:
	case Slot::NullableSize: {
		ResolvedType rt_type(slot == Slot::NullableString ? kString : kUsize);
		rt_type.suffixes.emplace_back(to_string(oQuestion));
		return rt_type;
	}
	case Slot::Chars:
	case Slot::Strings: {
		ResolvedType rt_type(slot == Slot::Chars ? kCharacter : kString);
		rt_type.suffixes.push_back(arraySuffix());
		return rt_type;
	}
	default:
		return {};
	}
}

void checkNumericMethod (const Definition &builtin, const CallSite &call, const ExpressionInfo &receiver, const std::array<NumericConstant, 2> &arguments,
                         TargetLayout layout) {
	const ResolvedType &rt_receiver = receiver.types.front();
	const TokenSpan     ts_method   = call.name.get_span();
	if (rt_receiver.integer()) {
		const IntegerConstant *ic_minimum = std::get_if<IntegerConstant>(&arguments[0]), *ic_maximum = std::get_if<IntegerConstant>(&arguments[1]);
		if (builtin.operation == BuiltinOperation::Abs) {
			const IntegerConstant *ic_value = std::get_if<IntegerConstant>(&receiver.numericConstant);
			if (ic_value && ic_value->negative && receiver.numericConstant == numericLimit(rt_receiver, layout, false))
				fail(Code::INTEGER_ABS_OVERFLOW, ts_method, "The absolute value of the minimum integer exceeds " + rt_receiver.spelling());
		} else if (builtin.operation == BuiltinOperation::Clamp && ic_minimum && ic_maximum && compareIntegers(*ic_minimum, *ic_maximum) > 0)
			fail(Code::INVALID_CLAMP_BOUNDS, ts_method, "The minimum clamp bound exceeds the maximum");
		else if (builtin.operation == BuiltinOperation::ToStringRadix && ic_minimum &&
		         (compareIntegers(*ic_minimum, { { 2 } }) < 0 || compareIntegers(*ic_minimum, { { 36 } }) > 0))
			fail(Code::INVALID_INTEGER_RADIX, call.suppliedArguments.front().expression->get_span(),
			     "An integer formatting radix must be between 2 and 36");
	} else if (rt_receiver.floating()) {
		if (builtin.operation == BuiltinOperation::Signum) {
			const double *d_value = std::get_if<double>(&receiver.numericConstant);
			if (d_value && std::isnan(*d_value)) fail(Code::FLOAT_SIGNUM_NAN, ts_method, "NaN has no signum");
		} else if (builtin.operation == BuiltinOperation::Clamp) {
			const double *d_minimum = std::get_if<double>(&arguments[0]), *d_maximum = std::get_if<double>(&arguments[1]);
			if ((d_minimum && std::isnan(*d_minimum)) || (d_maximum && std::isnan(*d_maximum)) ||
			    (d_minimum && d_maximum && *d_minimum > *d_maximum))
				fail(Code::INVALID_FLOAT_CLAMP_BOUNDS, ts_method, "Floating clamp bounds must be ordered and cannot be NaN");
		}
	}
}
}

bool builtinFunctionNamed (std::string_view name) {
	return definition(name, false) != nullptr;
}

bool builtinNeverReturns (std::string_view name) {
	const Definition *d_builtin = definition(name, false);
	return d_builtin && d_builtin->result == Slot::Never;
}

bool checksNumericMethod (std::string_view name) {
	const Definition *bd_method = definition(name, true);
	return bd_method && (bd_method->family == Family::Numeric || bd_method->operation == BuiltinOperation::ToStringRadix);
}

void BodyChecker::checkTypeArguments (const TypeArgumentList &arguments) const {
	for (const TypeReference &tr_type : arguments.arguments)
		if (!tr_type.is_placeholder() && bodyType(tr_type) == ResolvedType(kVoid))
			fail(Code::INVALID_GENERIC_ARGUMENT, tr_type.get_span(), "A type argument must be a value type");
}

void BodyChecker::checkArgumentNames (const CallSite &call, std::size_t start) {
	// Preserve name diagnostics in the remaining operands when this call already has a binding error.
	BodyChecker bc_probe       = *this;
	bc_probe.record            = false;
	bc_probe.we_capture        = nullptr;
	bc_probe.ef_probeConstants = nullptr;
	for (std::size_t st_index = start; st_index < call.suppliedArguments.size(); ++st_index) {
		try {
			bc_probe.expression(*call.suppliedArguments[st_index].expression);
		} catch (const cde::ClothDiagnostic &cd_error) {
			const Code dc_code = cd_error.get_code();
			if (dc_code == Code::UNKNOWN_NAME || dc_code == Code::NOT_CALLABLE || dc_code == Code::FUNCTION_VALUE ||
			    dc_code == Code::UNSUPPORTED_TYPE || dc_code == Code::INVALID_GENERIC_ARGUMENT || dc_code == Code::INTERNAL)
				throw;
		}
	}
}

void BodyChecker::checkBuiltinTypeArguments (const CallSite &call, const TypeArgumentList &arguments) {
	checkTypeArguments(arguments);
	if (dynamic_cast<const MemberCallExpression *>(&call)) checkArgumentNames(call, 0);
	fail(Code::GENERIC_ARITY, arguments.get_span(), std::format("'{}' takes no type arguments", call.name.get_lexeme()));
}

void BodyChecker::checkBuiltinArguments (const CallSite &call, const TypeArgumentList *typeArguments, std::size_t minimum, std::size_t maximum) {
	if (typeArguments) checkBuiltinTypeArguments(call, *typeArguments);
	const std::vector<CallArgument>::const_iterator it_label =
	        std::ranges::find_if(call.suppliedArguments, [] (const CallArgument &argument) { return argument.label.has_value(); });
	const std::size_t st_supplied = call.suppliedArguments.size();
	if (it_label == call.suppliedArguments.end() && st_supplied >= minimum && st_supplied <= maximum) return;
	checkArgumentNames(call, 0);
	if (it_label != call.suppliedArguments.end())
		fail(Code::INVALID_ARGUMENT_BINDING, it_label->label->get_span(), std::format("'{}' takes positional arguments", call.name.get_lexeme()));
	fail(Code::CALL_ARITY, call.name.get_span(), std::format("No overload of '{}' takes {} arguments", call.name.get_lexeme(), st_supplied));
}

ResolvedType BodyChecker::nonReturningNumericReceiver (const CallSite &call) {
	// A typed bound supplies its identity even when an earlier bound is an untyped literal.
	BodyChecker bc_probe = *this;
	bc_probe.record      = false;
	bc_probe.we_capture  = nullptr;
	for (const ElementEvidence ee_tier : { ElementEvidence::Typed, ElementEvidence::Literal }) {
		for (const CallArgument &ca_argument : call.suppliedArguments) {
			if (elementEvidence(*ca_argument.expression) != ee_tier) continue;
			const std::optional<ResolvedType> opt_hint = bc_probe.scalar(*ca_argument.expression);
			if (!opt_hint || (ee_tier == ElementEvidence::Literal && !opt_hint->floating())) continue;
			return acceptsReceiver(Family::Numeric, *opt_hint) ? *opt_hint : ResolvedType(kInteger);
		}
	}
	return ResolvedType(kInteger);
}

std::optional<ExpressionInfo> BodyChecker::builtinCall (const CallSite &call, const TypeArgumentList *typeArguments, const ExpressionInfo *receiver) {
	const Definition *bd_builtin = definition(call.name.get_name_key(), receiver != nullptr);
	if (!bd_builtin) return {};
	std::optional<ResolvedType> opt_receiver;
	if (receiver && receiver->fallsThrough) {
		opt_receiver = receiver->types.front();
		if (!acceptsReceiver(bd_builtin->family, *opt_receiver)) return {};
	}
	const std::size_t st_supplied = call.suppliedArguments.size();
	const std::size_t st_maximum  = std::ranges::count_if(bd_builtin->parameters, [] (Slot slot) { return slot != Slot::None; });
	checkBuiltinArguments(call, typeArguments, st_maximum - bd_builtin->optionalLast, st_maximum);

	if (receiver && !opt_receiver)
		opt_receiver = bd_builtin->family == Family::Numeric && st_supplied ? nonReturningNumericReceiver(call) : representative(bd_builtin->family);
	BuiltinCall                    bc_bound{ bd_builtin->operation, opt_receiver, {} };
	bool                           b_continues = !receiver || receiver->fallsThrough;
	std::optional<BodyChecker>     opt_unreachable;
	std::array<NumericConstant, 2> v_constants;
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
			opt_parameter = ResolvedType(kString);
			s_expected    = "a printable value";
			b_accepts     = !rt_actual || bc_operand.bindText(e_argument, *rt_actual);
			break;
		case Slot::NativeInt:
			opt_parameter = ResolvedType(kInteger);
			s_expected    = "an integer";
			b_accepts     = !rt_actual || rt_actual->integer();
			break;
		case Slot::CharOrString:
			opt_parameter = rt_actual && *rt_actual == ResolvedType(kString) ? *rt_actual : ResolvedType(kCharacter);
			s_expected    = std::format("{} or {}", to_string(kCharacter), to_string(kString));
			b_accepts     = !rt_actual || *opt_parameter == *rt_actual;
			break;
		default:
			s_expected = opt_parameter->spelling();
			b_accepts  = !rt_actual || analyzer.accepts(*opt_parameter, *rt_actual);
			break;
		}
		if (!b_accepts) {
			checkArgumentNames(call, st_index + 1);
			fail(Code::INVALID_BUILTIN_ARGUMENT, e_argument.get_span(),
			     std::format("'{}' requires {}, got {}", bd_builtin->name, s_expected, rt_actual->spelling()));
		}
		bc_bound.parameters.push_back(std::move(*opt_parameter));
		v_constants[st_index] = std::move(ei_actual.numericConstant);
		b_continues &= ei_actual.fallsThrough;
	}
	if ((record || ef_probeConstants) && receiver && receiver->fallsThrough)
		checkNumericMethod(*bd_builtin, call, *receiver, v_constants, analyzer.program.layout);
	ExpressionInfo ei_result{ .types = {}, .fallsThrough = b_continues && bd_builtin->result != Slot::Never, .builtin = std::move(bc_bound) };
	if (std::optional<ResolvedType> opt_result = slotType(bd_builtin->result, opt_receiver)) ei_result.types.push_back(std::move(*opt_result));
	return ei_result;
}
}
