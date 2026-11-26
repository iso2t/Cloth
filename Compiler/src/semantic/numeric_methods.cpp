// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <array>
#include <cmath>
#include <format>

#include <ast/expr/member_call_expression.hpp>
#include <diagnostics.hpp>
#include <semantic/checker.hpp>

namespace cloth::semantic {
namespace {
using Op = BuiltinOperation;

struct NumericContract {
	std::string_view name;
	BuiltinOperation operation;
	bool             integerOnly = false;
};

// The standalone signatures also identify which library overloads carry the historical diagnostics.
constexpr NumericContract CONTRACTS[] = {
	{ "abs", Op::Abs },
	{ "clamp", Op::Clamp },
	{ "signum", Op::Signum },
	{ "toStringRadix", Op::ToStringRadix, true },
	{ "leadingZeros", Op::LeadingZeros, true },
	{ "trailingZeros", Op::TrailingZeros, true },
};

const NumericContract *numericContract (std::string_view name) {
	const auto it = std::ranges::find(CONTRACTS, name, &NumericContract::name);
	return it == std::end(CONTRACTS) ? nullptr : it;
}

struct NumericSignature {
	ResultTypes  parameters;
	ResolvedType result;
};

NumericSignature standaloneSignature (Op operation, const ResolvedType &self) {
	switch (operation) {
	case Op::Abs:
		return { {}, self };
	case Op::Clamp:
		return { { self, self }, self };
	case Op::Signum:
		return { {}, ResolvedType(KInteger) };
	case Op::ToStringRadix:
		return { { ResolvedType(KInteger) }, ResolvedType(KString) };
	default:
		return { {}, ResolvedType(KUsize) };
	}
}

bool matchesContract (const Analyzer &analyzer, const Candidate &method, const NumericSignature &contract) {
	const auto &signature = analyzer.declarations.at(method.symbol);
	if (typeParameters(*method.symbol) || std::ranges::any_of(callable(*method.symbol)->parameters, &ParameterDeclaration::isOptional) ||
	    signature.parameters.size() != contract.parameters.size() || signature.returns.size() != 1 ||
	    analyzer.substitute(signature.returns.front(), method.arguments) != contract.result)
		return false;
	return std::ranges::equal(signature.parameters, contract.parameters, {},
	                          [&] (const ResolvedType &type) { return analyzer.substitute(type, method.arguments); });
}

void checkNumericMethod (BuiltinOperation operation, const CallSite &call, const ExpressionInfo &receiver, const std::array<NumericConstant, 2> &arguments,
                         const TargetLayout layout) {
	const ResolvedType &rt_receiver = receiver.types.front();
	const TokenSpan     ts_method   = call.name.getSpan();
	if (rt_receiver.integer()) {
		const IntegerConstant *ic_minimum = std::get_if<IntegerConstant>(&arguments[0]), *ic_maximum = std::get_if<IntegerConstant>(&arguments[1]);
		if (operation == BuiltinOperation::Abs) {
			if (const IntegerConstant *ic_value = std::get_if<IntegerConstant>(&receiver.numericConstant);
			    ic_value && ic_value->negative && receiver.numericConstant == numericLimit(rt_receiver, layout, false))
				fail(Code::IntegerAbsOverflow, ts_method, "The absolute value of the minimum integer exceeds " + rt_receiver.spelling());
		} else if (operation == BuiltinOperation::Clamp && ic_minimum && ic_maximum && compareIntegers(*ic_minimum, *ic_maximum) > 0)
			fail(Code::InvalidClampBounds, ts_method, "The minimum clamp bound exceeds the maximum");
		else if (operation == BuiltinOperation::ToStringRadix && ic_minimum &&
		         (compareIntegers(*ic_minimum, { .magnitude = { 2 } }) < 0 || compareIntegers(*ic_minimum, { .magnitude = { 36 } }) > 0))
			fail(Code::InvalidIntegerRadix, call.suppliedArguments.front().expression->getSpan(),
			     "An integer formatting radix must be between 2 and 36");
	} else if (rt_receiver.floating()) {
		if (operation == BuiltinOperation::Signum) {
			if (const double *d_value = std::get_if<double>(&receiver.numericConstant); d_value && std::isnan(*d_value))
				fail(Code::FloatSignumNan, ts_method, "NaN has no signum");
		} else if (operation == BuiltinOperation::Clamp) {
			const double *d_maximum = std::get_if<double>(&arguments[1]);
			if (const double *d_minimum = std::get_if<double>(&arguments[0]); (d_minimum && std::isnan(*d_minimum)) ||
			                                                                  (d_maximum && std::isnan(*d_maximum)) ||
			                                                                  (d_minimum && d_maximum && *d_minimum > *d_maximum))
				fail(Code::InvalidFloatClampBounds, ts_method, "Floating clamp bounds must be ordered and cannot be NaN");
		}
	}
}
}

bool numericMethodNamed (std::string_view name, const ResolvedType &receiver) {
	const auto *contract = numericContract(name);
	return contract && receiver.primitiveFamily() && (!contract->integerOnly || receiver.integer());
}

bool checksNumericMethod (std::string_view name) {
	const auto *contract = numericContract(name);
	return contract && (!contract->integerOnly || contract->operation == Op::ToStringRadix);
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
			return opt_hint->primitiveFamily() ? *opt_hint : ResolvedType(KInteger);
		}
	}
	return ResolvedType(KInteger);
}

std::optional<ExpressionInfo> BodyChecker::numericMethodCall (const MemberCallExpression &call, const ExpressionInfo &receiver) {
	const auto *contract = numericContract(call.name.getNameKey());
	if (!contract) return {};
	const auto  *typeArguments = call.typeArguments ? &*call.typeArguments : nullptr;
	ResolvedType self(KInteger);
	if (receiver.fallsThrough) {
		self = receiver.types.front();
		if (!self.primitiveFamily() || (contract->integerOnly && !self.integer())) return {};
	} else if (contract->operation == Op::Clamp) {
		checkBuiltinArguments(call, typeArguments, 2, 2);
		self = nonReturningNumericReceiver(call);
	}
	const NumericSignature signature = standaloneSignature(contract->operation, self);
	std::vector<Candidate> methods;
	if (receiver.fallsThrough && analyzer.primitiveOwner(self)) {
		methods = analyzer.members(self, call.name.getNameKey(), SymbolKind::Method);
		if (methods.empty()) return {};
		if (methods.size() > 1) {
			// Resolve overloads before applying a precondition to the selected library method.
			ExpressionInfo result   = invoke(call.name, call.getSpan(), call.suppliedArguments, methods, typeArguments);
			const auto     selected = std::ranges::find(methods, result.symbol, &Candidate::symbol);
			if (selected == methods.end() || !matchesContract(analyzer, *selected, signature)) return result;
			checkBuiltinArguments(call, typeArguments, signature.parameters.size(), signature.parameters.size());
			if (const ExpressionFacts *facts = record ? &bf_facts.expressions : ef_probeConstants) {
				std::array<NumericConstant, 2> constants;
				for (std::size_t i = 0; i < call.suppliedArguments.size(); ++i) {
					const auto it = facts->find(call.suppliedArguments[i].expression.get());
					if (it != facts->end()) constants[i] = it->second.numericConstant;
				}
				checkNumericMethod(contract->operation, call, receiver, constants, analyzer.program.layout);
			}
			return result;
		}
		if (!matchesContract(analyzer, methods.front(), signature)) return {};
	}

	checkBuiltinArguments(call, typeArguments, signature.parameters.size(), signature.parameters.size());
	bool                           continues = receiver.fallsThrough;
	std::optional<BodyChecker>     unreachable;
	std::array<NumericConstant, 2> constants;
	for (std::size_t i = 0; i < signature.parameters.size(); ++i) {
		const Expression   &argument  = *call.suppliedArguments[i].expression;
		const ResolvedType &parameter = signature.parameters[i];
		ExpressionInfo      actual    = onPath(continues, unreachable).scalarInfo(argument, &parameter);
		if (actual.fallsThrough && !analyzer.accepts(parameter, actual.types.front())) {
			checkArgumentNames(call, i + 1);
			fail(Code::InvalidBuiltinArgument, argument.getSpan(),
			     std::format("'{}' requires {}, got {}", contract->name, parameter.spelling(), actual.types.front().spelling()));
		}
		constants[i] = std::move(actual.numericConstant);
		continues &= actual.fallsThrough;
	}
	if ((record || ef_probeConstants) && receiver.fallsThrough) checkNumericMethod(contract->operation, call, receiver, constants, analyzer.program.layout);
	if (!methods.empty()) {
		ExpressionInfo result = invoke(call.name, call.getSpan(), call.suppliedArguments, methods, nullptr, &signature.parameters);
		result.fallsThrough &= continues;
		return result;
	}
	return ExpressionInfo{ .types        = { signature.result },
		               .fallsThrough = continues,
		               .builtin      = BuiltinCall{ .operation = contract->operation, .receiver = self, .parameters = signature.parameters } };
}
}
