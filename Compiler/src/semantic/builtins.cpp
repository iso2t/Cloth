// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/language/expressions/Builtins.rst

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <variant>

#include <ast/expr/expression.hpp>
#include <ast/expr/expressions.hpp>
#include <ast/expr/member_call_expression.hpp>
#include <ast/type/type_reference.hpp>
#include <diagnostics.hpp>
#include <semantic/checked_program.hpp>
#include <semantic/checker.hpp>
#include <semantic/target_layout.hpp>
#include <token/token.hpp>

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
	{ .operation = BuiltinOperation::Abs, .name = "abs", .family = Family::Numeric, .result = Slot::Self },
	{ .operation = BuiltinOperation::Clamp, .name = "clamp", .family = Family::Numeric, .result = Slot::Self, .parameters = { Slot::Self, Slot::Self } },
	{ .operation = BuiltinOperation::Signum, .name = "signum", .family = Family::Numeric, .result = Slot::Int },
	{ .operation  = BuiltinOperation::ToStringRadix,
	  .name       = "toStringRadix",
	  .family     = Family::Integer,
	  .result     = Slot::String,
	  .parameters = { Slot::Int } },
	{ .operation = BuiltinOperation::LeadingZeros, .name = "leadingZeros", .family = Family::Integer, .result = Slot::Size },
	{ .operation = BuiltinOperation::TrailingZeros, .name = "trailingZeros", .family = Family::Integer, .result = Slot::Size },
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
	if (type.declaration || type.parameter || !type.suffixes.empty()) return false;
	switch (family) {
	case Family::Numeric:
		return type.integer() || type.floating();
	case Family::Integer:
		return type.integer();
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

void checkNumericMethod (const Definition &builtin, const CallSite &call, const ExpressionInfo &receiver, const std::array<NumericConstant, 2> &arguments,
                         const TargetLayout layout) {
	const ResolvedType &rt_receiver = receiver.types.front();
	const TokenSpan     ts_method   = call.name.getSpan();
	if (rt_receiver.integer()) {
		const IntegerConstant *ic_minimum = std::get_if<IntegerConstant>(&arguments[0]), *ic_maximum = std::get_if<IntegerConstant>(&arguments[1]);
		if (builtin.operation == BuiltinOperation::Abs) {
			if (const IntegerConstant *ic_value = std::get_if<IntegerConstant>(&receiver.numericConstant);
			    ic_value && ic_value->negative && receiver.numericConstant == numericLimit(rt_receiver, layout, false))
				fail(Code::IntegerAbsOverflow, ts_method, "The absolute value of the minimum integer exceeds " + rt_receiver.spelling());
		} else if (builtin.operation == BuiltinOperation::Clamp && ic_minimum && ic_maximum && compareIntegers(*ic_minimum, *ic_maximum) > 0)
			fail(Code::InvalidClampBounds, ts_method, "The minimum clamp bound exceeds the maximum");
		else if (builtin.operation == BuiltinOperation::ToStringRadix && ic_minimum &&
		         (compareIntegers(*ic_minimum, { .magnitude = { 2 } }) < 0 || compareIntegers(*ic_minimum, { .magnitude = { 36 } }) > 0))
			fail(Code::InvalidIntegerRadix, call.suppliedArguments.front().expression->getSpan(),
			     "An integer formatting radix must be between 2 and 36");
	} else if (rt_receiver.floating()) {
		if (builtin.operation == BuiltinOperation::Signum) {
			if (const double *d_value = std::get_if<double>(&receiver.numericConstant); d_value && std::isnan(*d_value))
				fail(Code::FloatSignumNan, ts_method, "NaN has no signum");
		} else if (builtin.operation == BuiltinOperation::Clamp) {
			const double *d_maximum = std::get_if<double>(&arguments[1]);
			if (const double *d_minimum = std::get_if<double>(&arguments[0]); (d_minimum && std::isnan(*d_minimum)) ||
			                                                                  (d_maximum && std::isnan(*d_maximum)) ||
			                                                                  (d_minimum && d_maximum && *d_minimum > *d_maximum))
				fail(Code::InvalidFloatClampBounds, ts_method, "Floating clamp bounds must be ordered and cannot be NaN");
		}
	}
}
}

bool builtinFunctionNamed (const std::string_view name) {
	return definition(name, false) != nullptr;
}

bool builtinNeverReturns (const std::string_view name) {
	const Definition *d_builtin = definition(name, false);
	return d_builtin && d_builtin->result == Slot::Never;
}

bool checksNumericMethod (const std::string_view name) {
	const Definition *bd_method = definition(name, true);
	return bd_method && (bd_method->family == Family::Numeric || bd_method->operation == BuiltinOperation::ToStringRadix);
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

ResolvedType BodyChecker::nonReturningNumericReceiver (const CallSite &call) const {
	// A typed bound supplies its identity even when an earlier bound is an untyped literal.
	BodyChecker bc_probe = *this;
	bc_probe.record      = false;
	bc_probe.we_capture  = nullptr;
	for (const ElementEvidence ee_tier : { ElementEvidence::Typed, ElementEvidence::Literal }) {
		for (const CallArgument &ca_argument : call.suppliedArguments) {
			if (elementEvidence(*ca_argument.expression) != ee_tier) continue;
			const std::optional<ResolvedType> opt_hint = bc_probe.scalar(*ca_argument.expression);
			if (!opt_hint || (ee_tier == ElementEvidence::Literal && !opt_hint->floating())) continue;
			return acceptsReceiver(Family::Numeric, *opt_hint) ? *opt_hint : ResolvedType(KInteger);
		}
	}
	return ResolvedType(KInteger);
}

std::optional<ExpressionInfo> BodyChecker::builtinCall (const CallSite &call, const TypeArgumentList *typeArguments, const ExpressionInfo *receiver) {
	const Definition *bd_builtin = definition(call.name.getNameKey(), receiver != nullptr);
	if (!bd_builtin) return {};
	std::optional<ResolvedType> opt_receiver;
	if (receiver && receiver->fallsThrough) {
		opt_receiver = receiver->types.front();
		if (!acceptsReceiver(bd_builtin->family, *opt_receiver)) return {};
	}
	const std::size_t st_supplied = call.suppliedArguments.size();
	const std::size_t st_maximum  = std::ranges::count_if(bd_builtin->parameters, [] (const Slot slot) { return slot != Slot::None; });
	checkBuiltinArguments(call, typeArguments, st_maximum - bd_builtin->optionalLast, st_maximum);

	if (receiver && !opt_receiver)
		opt_receiver = bd_builtin->family == Family::Numeric && st_supplied ? nonReturningNumericReceiver(call) : representative(bd_builtin->family);
	BuiltinCall                    bc_bound{ .operation = bd_builtin->operation, .receiver = opt_receiver, .parameters = {} };
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
