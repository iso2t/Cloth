// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

#include <ast/expr/expression.hpp>
#include <ast/expr/expressions.hpp>
#include <ast/expr/provider_expressions.hpp>
#include <semantic/checked_program.hpp>
#include <semantic/checker.hpp>
#include <semantic/target_layout.hpp>
#include <token/numeric_literals.hpp>
#include <token/token.hpp>

namespace cloth::semantic {
namespace {
using Words = IntegerValue;

bool zero (const Words &left) {
	return left.size() == 1 && !left.front();
}

void trim (Words &left) {
	while (left.size() > 1 && !left.back()) left.pop_back();
}

unsigned bits (const Words &left) {
	return static_cast<unsigned>(32 * (left.size() - 1) + std::bit_width(left.back()));
}

int compare (const Words &left, const Words &right) {
	if (left.size() != right.size()) return left.size() > right.size() ? 1 : -1;
	for (std::size_t st_word = left.size(); st_word-- > 0;)
		if (left[st_word] != right[st_word]) return left[st_word] > right[st_word] ? 1 : -1;
	return 0;
}

Words add (const Words &left, const Words &right) {
	Words         v_result(std::max(left.size(), right.size()));
	std::uint64_t u64_carry = 0;
	for (std::size_t st_word = 0; st_word < v_result.size(); ++st_word) {
		u64_carry += st_word < left.size() ? left[st_word] : 0u;
		u64_carry += st_word < right.size() ? right[st_word] : 0u;
		v_result[st_word] = static_cast<std::uint32_t>(u64_carry);
		u64_carry >>= 32;
	}
	if (u64_carry) v_result.push_back(1);
	return v_result;
}

// The caller establishes a >= b.
void subtract (Words &left, const Words &right) {
	std::uint64_t u64_borrow = 0;
	for (std::size_t st_word = 0; st_word < left.size(); ++st_word) {
		const std::uint64_t u64_term = u64_borrow + (st_word < right.size() ? right[st_word] : 0u);
		u64_borrow                   = left[st_word] < u64_term;
		left[st_word] -= static_cast<std::uint32_t>(u64_term);
	}
	trim(left);
}

IntegerConstant plus (IntegerConstant left, IntegerConstant right) {
	if (left.negative == right.negative) return { .magnitude = add(left.magnitude, right.magnitude), .negative = left.negative };
	if (compare(left.magnitude, right.magnitude) < 0) std::swap(left, right);
	subtract(left.magnitude, right.magnitude);
	return left;
}

Words multiply (const Words &left, const Words &right) {
	Words v_result(left.size() + right.size());
	for (std::size_t st_word = 0; st_word < left.size(); ++st_word) {
		std::uint64_t u64_carry = 0;
		for (std::size_t st_factor = 0; st_factor < right.size(); ++st_factor) {
			u64_carry += static_cast<std::uint64_t>(left[st_word]) * right[st_factor] + v_result[st_word + st_factor];
			v_result[st_word + st_factor] = static_cast<std::uint32_t>(u64_carry);
			u64_carry >>= 32;
		}
		v_result[st_word + right.size()] = static_cast<std::uint32_t>(u64_carry);
	}
	trim(v_result);
	return v_result;
}

bool bit (const Words &left, const unsigned index) {
	return index / 32 < left.size() && left[index / 32] >> (index % 32) & 1u;
}

Words divide (const Words &left, const Words &right, const bool remainder) {
	Words v_quotient(left.size());
	if (right.size() == 1) {
		std::uint64_t u64_remainder = 0;
		for (std::size_t st_word = left.size(); st_word-- > 0;) {
			u64_remainder       = (u64_remainder << 32) | left[st_word];
			v_quotient[st_word] = static_cast<std::uint32_t>(u64_remainder / right[0]);
			u64_remainder %= right[0];
		}
		if (remainder) return { static_cast<std::uint32_t>(u64_remainder) };
	} else {
		Words v_remainder{ 0 };
		for (unsigned u_bit = bits(left); u_bit-- > 0;) {
			unsigned u_carry = bit(left, u_bit);
			for (std::uint32_t &u32_word : v_remainder) {
				const unsigned u_high = u32_word >> 31;
				u32_word              = (u32_word << 1) | u_carry;
				u_carry               = u_high;
			}
			if (u_carry) v_remainder.push_back(u_carry);
			if (compare(v_remainder, right) >= 0) {
				subtract(v_remainder, right);
				v_quotient[u_bit / 32] |= 1u << (u_bit % 32);
			}
		}
		if (remainder) return v_remainder;
	}
	trim(v_quotient);
	return v_quotient;
}

Words powerOfTwo (const unsigned exponent) {
	Words v_result(1 + exponent / 32);
	v_result.back() = 1u << (exponent % 32);
	return v_result;
}

IntegerConstant wrap (IntegerConstant value, const unsigned width, const bool isUnsigned) {
	const Words v_modulus   = powerOfTwo(width);
	Words      &v_magnitude = value.magnitude;
	if (bits(v_magnitude) > width) {
		v_magnitude.resize((width + 31) / 32);
		if (width % 32) v_magnitude.back() &= (1u << (width % 32)) - 1;
		trim(v_magnitude);
	}
	if (value.negative && !zero(v_magnitude)) {
		Words v_complement = v_modulus;
		subtract(v_complement, v_magnitude);
		v_magnitude = std::move(v_complement);
	}
	value.negative = !isUnsigned && bit(v_magnitude, width - 1);
	if (value.negative) {
		Words v_complement = v_modulus;
		subtract(v_complement, v_magnitude);
		v_magnitude = std::move(v_complement);
	}
	return value;
}

double roundedFloat (const double value, const unsigned width) {
	if (width == 64) return value;
	if (width == 32) return static_cast<float>(value);
	if (!std::isfinite(value) || value == 0) return value;
	if (std::abs(value) >= 65520) return std::copysign(std::numeric_limits<double>::infinity(), value);
	const double d_step = std::ldexp(1., std::max(-24, std::ilogb(value) - 10));
	return std::nearbyint(value / d_step) * d_step;
}

double asFloat (const NumericConstant &constant, const unsigned width) {
	if (const double *v = std::get_if<double>(&constant)) return *v;
	const auto &[magnitude, negative] = std::get<IntegerConstant>(constant);
	const unsigned u_bits = bits(magnitude), u_precision = width == 64 ? 53 : 24;
	const unsigned u_shift      = u_bits > u_precision ? u_bits - u_precision : 0;
	std::uint64_t  u64_mantissa = 0;
	for (unsigned i = u_bits; i-- > u_shift;) u64_mantissa = (u64_mantissa << 1) | bit(magnitude, i);
	if (u_shift && bit(magnitude, u_shift - 1)) {
		bool b_sticky = false;
		for (unsigned i = 0; i + 1 < u_shift && !b_sticky; ++i) b_sticky = bit(magnitude, i);
		if (b_sticky || u64_mantissa & 1) ++u64_mantissa;
	}
	const double f = std::ldexp(static_cast<double>(u64_mantissa), static_cast<int>(u_shift));
	return negative ? -f : f;
}

IntegerConstant truncateFloat (const double value) {
	IntegerConstant result{ .magnitude = { 0 }, .negative = value < 0 };
	double          magnitude = std::trunc(std::abs(value));
	for (std::size_t i = 0; magnitude >= 1; ++i) {
		const auto word = static_cast<std::uint32_t>(std::fmod(magnitude, 4294967296.));
		if (i == 0)
			result.magnitude[0] = word;
		else
			result.magnitude.push_back(word);
		magnitude = std::floor(std::ldexp(magnitude, -32));
	}
	if (zero(result.magnitude)) result.negative = false;
	return result;
}

template <typename T> T parseFloat (std::string_view text) {
	T value{};
	if (const std::from_chars_result fc_result = std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::fixed);
	    fc_result.ec == std::errc::result_out_of_range) {
		const std::size_t st_point   = text.find('.');
		const bool        b_belowOne = st_point != std::string_view::npos && text.substr(0, st_point).find_first_not_of('0') == std::string_view::npos;
		return b_belowOne ? 0 : std::numeric_limits<T>::infinity();
	}
	return value;
}

// The language diagnoses a literal divisor or an integer provider, optionally signed.
bool literalOperand (const Expression &node) {
	if (const auto *ue_unary = dynamic_cast<const UnaryExpression *>(&node)) {
		const Operator o_op = ue_unary->op.getOperator();
		return (o_op == OPlus || o_op == OMinus) && literalOperand(*ue_unary->operand);
	}
	return dynamic_cast<const IntegerLiteral *>(&node) || dynamic_cast<const ProviderAccessExpression *>(&node);
}

template <typename T> T arithmetic (const Operator op, T left, T right) {
	switch (op) {
	case OPlus:
		return left + right;
	case OMinus:
		return left - right;
	case OMul:
		return left * right;
	case ODiv:
		return left / right;
	default:
		return std::fmod(left, right);
	}
}
}

int compareIntegers (const IntegerConstant &left, const IntegerConstant &right) {
	if (left.negative != right.negative) return left.negative ? -1 : 1;
	const int i_magnitude = compare(left.magnitude, right.magnitude);
	return left.negative ? -i_magnitude : i_magnitude;
}

double floatingLiteral (std::string_view text, const unsigned width) {
	if (width == 32) return parseFloat<float>(text);
	const auto value = parseFloat<double>(text);
	if (width == 64 || !std::isfinite(value) || value == 0 || value > 65520.) return roundedFloat(value, width);
	// A decimal can lie on either side of a half midpoint even when both round to the same double.
	if (const double d_step = std::ldexp(1., std::max(-24, std::ilogb(value) - 10)); std::fmod(value / d_step, 1.) == .5) {
		char             v_decimal[64];
		const char      *p_end = std::to_chars(v_decimal, v_decimal + sizeof(v_decimal), value, std::chars_format::fixed, 25).ptr;
		std::string_view s_midpoint(v_decimal, p_end);
		while (!s_midpoint.empty() && s_midpoint.back() == '0') s_midpoint.remove_suffix(1);
		while (text.size() > 1 && text.front() == '0' && text[1] != '.') text.remove_prefix(1);
		while (text.ends_with('0')) text.remove_suffix(1);
		if (text.starts_with('.') && s_midpoint.starts_with('0')) s_midpoint.remove_prefix(1);
		if (text != s_midpoint) return roundedFloat(std::nextafter(value, text < s_midpoint ? 0. : std::numeric_limits<double>::infinity()), width);
	}
	return roundedFloat(value, width);
}

NumericConstant numericLimit (const ResolvedType &type, const TargetLayout layout, const bool maximum) {
	if (type.floating()) {
		const double d_limit = type.width == 16 ? 65504. : type.width == 32 ? std::numeric_limits<float>::max() : std::numeric_limits<double>::max();
		return maximum ? d_limit : -d_limit;
	}
	const bool b_unsigned = type.unsignedInteger();
	if (!maximum && b_unsigned) return IntegerConstant{ .magnitude = { 0 } };
	Words magnitude = powerOfTwo((type.width ? type.width : layout.sizeBits) - !b_unsigned);
	if (maximum) subtract(magnitude, { 1 });
	return IntegerConstant{ .magnitude = std::move(magnitude), .negative = !maximum };
}

void requireFinite (const Expression &operand, const ExpressionFacts &facts, const Code code) {
	const auto it = facts.find(&operand);
	if (it == facts.end()) return;
	if (const double *f = std::get_if<double>(&it->second.numericConstant); f && !std::isfinite(*f))
		fail(code, operand.getSpan(), "A floating range requires finite values");
}

void checkNumeric (const Expression &node, ExpressionInfo &result, const ExpressionFacts &facts, TargetLayout layout) {
	if (result.symbol || result.leftContext || result.rightContext || result.types.size() != 1 || !result.fallsThrough) return;
	const ResolvedType &rt_declared = result.types.front();
	const ResolvedType  type        = rt_declared.nullable() ? rt_declared.element() : rt_declared;
	if (type == ResolvedType(KBoolean)) {
		std::optional<bool> opt_truth;
		if (const auto *be_test = dynamic_cast<const BinaryExpression *>(&node); be_test && result.booleanEquality) {
			const std::optional<bool> opt_left  = facts.at(be_test->left.get()).integerTruth(),
			                          opt_right = facts.at(be_test->right.get()).integerTruth();
			if (opt_left && opt_right) opt_truth = be_test->op.getOperator() == OEqual ? *opt_left == *opt_right : *opt_left != *opt_right;
		} else if (const auto *ce_cast = dynamic_cast<const CastExpression *>(&node)) {
			opt_truth = facts.at(ce_cast->operand.get()).integerTruth();
		} else if (const auto *ue_not = dynamic_cast<const UnaryExpression *>(&node); ue_not && ue_not->op.getOperator() == ONot) {
			if (const std::optional<bool> opt_operand = facts.at(ue_not->operand.get()).integerTruth()) opt_truth = !*opt_operand;
		}
		if (opt_truth) result.numericConstant = IntegerConstant{ .magnitude = { *opt_truth } };
		return;
	}
	if (!type.integer() && !type.floating()) return;
	const unsigned width      = type.width ? type.width : layout.sizeBits;
	const bool     b_unsigned = type.unsignedInteger();
	if (const auto *literal = dynamic_cast<const IntegerLiteral *>(&node)) {
		const IntegerConstant value{ .magnitude = literal->value };
		result.numericConstant = type.floating() ? NumericConstant{ roundedFloat(asFloat(value, width), width) } : NumericConstant{ value };
	} else if (const auto *literalExpr = dynamic_cast<const FloatingPointLiteral *>(&node)) {
		result.numericConstant = floatingLiteral(literalExpr->value, width);
	} else if (const auto *ue_unary = dynamic_cast<const UnaryExpression *>(&node)) {
		const NumericConstant &operand = facts.at(ue_unary->operand.get()).numericConstant;
		const Operator         o_op    = ue_unary->op.getOperator();
		if (const IntegerConstant *i = std::get_if<IntegerConstant>(&operand)) {
			IntegerConstant ic_value = *i;
			if (o_op == OMinus || o_op == OTilde) ic_value.negative = !ic_value.negative;
			// In two's complement, flipping every bit gives -x - 1 at the operand's width.
			if (o_op == OTilde) ic_value = plus(std::move(ic_value), { .magnitude = { 1 }, .negative = true });
			result.numericConstant = wrap(std::move(ic_value), width, b_unsigned);
		} else if (const double *f = std::get_if<double>(&operand))
			result.numericConstant = o_op == OMinus ? -*f : *f;
	} else if (const auto *ce_cast = dynamic_cast<const CastExpression *>(&node)) {
		const NumericConstant &operand = facts.at(ce_cast->operand.get()).numericConstant;
		if (std::holds_alternative<std::monostate>(operand)) return;
		if (type.floating())
			result.numericConstant = roundedFloat(asFloat(operand, width), width);
		else if (const IntegerConstant *i = std::get_if<IntegerConstant>(&operand))
			result.numericConstant = wrap(*i, width, b_unsigned);
		else {
			if (const double value = std::get<double>(operand); std::isfinite(value)) {
				if (const IntegerConstant ic_truncated = truncateFloat(value); wrap(ic_truncated, width, b_unsigned) == ic_truncated) {
					result.numericConstant = ic_truncated;
					return;
				}
			}
			result.invalidNumericCast = true;
		}
	} else if (const auto *be_binary = dynamic_cast<const BinaryExpression *>(&node)) {
		const Operator op = be_binary->op.getOperator();
		if (op != OPlus && op != OMinus && op != OMul && op != ODiv && op != OMod) return;
		const NumericConstant &left = facts.at(be_binary->left.get()).numericConstant, &right = facts.at(be_binary->right.get()).numericConstant;
		const IntegerConstant *a = std::get_if<IntegerConstant>(&left), *b = std::get_if<IntegerConstant>(&right);
		if (type.integer() && b && (op == ODiv || op == OMod)) {
			if (zero(b->magnitude)) {
				if (literalOperand(*be_binary->right)) fail(Code::IntegerZeroDivisor, be_binary->right->getSpan(), "Integer divisor is zero");
				return;
			}
			if (a && !b_unsigned && *a == std::get<IntegerConstant>(numericLimit(type, layout, false)) &&
			    *b == IntegerConstant{ .magnitude = { 1 }, .negative = true }) {
				if (literalOperand(*be_binary->left) && literalOperand(*be_binary->right))
					fail(Code::IntegerDivisionOverflow, be_binary->op.getSpan(), "Signed division exceeds the result type");
				return;
			}
		}
		if (std::holds_alternative<std::monostate>(left) || std::holds_alternative<std::monostate>(right)) return;
		if (type.floating()) {
			const double x = asFloat(left, width), y = asFloat(right, width);
			result.numericConstant =
			        roundedFloat(width == 64 ? arithmetic(op, x, y) : arithmetic(op, static_cast<float>(x), static_cast<float>(y)), width);
		} else if (a && b) {
			IntegerConstant ic_computed;
			if (op == OPlus || op == OMinus)
				ic_computed = plus(*a, { .magnitude = b->magnitude, .negative = b->negative != (op == OMinus) });
			else if (op == OMul)
				ic_computed = { .magnitude = multiply(a->magnitude, b->magnitude), .negative = a->negative != b->negative };
			else
				ic_computed = { .magnitude = divide(a->magnitude, b->magnitude, op == OMod),
					        .negative  = op == OMod ? a->negative : a->negative != b->negative };
			result.numericConstant = wrap(std::move(ic_computed), width, b_unsigned);
		}
	}
}
}
