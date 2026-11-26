// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/language/expressions/Builtins.rst

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
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
bool isBuiltinExitName (const std::string_view name) {
	return name == "exit";
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

ExpressionInfo BodyChecker::exitCall (const CallSite &call, const TypeArgumentList *typeArguments) {
	checkBuiltinArguments(call, typeArguments, 1, 2);
	BuiltinCall                bound{ .operation = BuiltinOperation::Exit, .parameters = {} };
	bool                       continues = true;
	std::optional<BodyChecker> unreachable;
	for (std::size_t i = 0; i < call.suppliedArguments.size(); ++i) {
		const Expression  &argument = *call.suppliedArguments[i].expression;
		const ResolvedType parameter(i == 0 ? KInteger : KString);
		const auto         actual = onPath(continues, unreachable).scalar(argument, i == 0 ? nullptr : &parameter);
		if (actual && !(i == 0 ? actual->integer() : analyzer.accepts(parameter, *actual))) {
			checkArgumentNames(call, i + 1);
			fail(Code::InvalidBuiltinArgument, argument.getSpan(),
			     std::format("'exit' requires {}, got {}", i == 0 ? "an integer" : parameter.spelling(), actual->spelling()));
		}
		bound.parameters.push_back(parameter);
		continues &= actual.has_value();
	}
	return { .types = {}, .fallsThrough = false, .builtin = std::move(bound) };
}
}
