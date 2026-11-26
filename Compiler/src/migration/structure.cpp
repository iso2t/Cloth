// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <typeindex>
#include <utility>
#include <variant>
#include <vector>

#include <ast/decl/class_declaration.hpp>
#include <ast/decl/declarations.hpp>
#include <ast/decl/enum_declaration.hpp>
#include <ast/decl/function_declaration.hpp>
#include <ast/decl/import_declaration.hpp>
#include <ast/decl/interface_declaration.hpp>
#include <ast/decl/state_declaration.hpp>
#include <ast/decl/struct_declaration.hpp>
#include <ast/expr/class_expression.hpp>
#include <ast/expr/expression.hpp>
#include <ast/expr/expressions.hpp>
#include <ast/expr/member_call_expression.hpp>
#include <ast/expr/provider_expressions.hpp>
#include <ast/expr/struct_expressions.hpp>
#include <ast/source_unit.hpp>
#include <ast/stmt/do_while_statement.hpp>
#include <ast/stmt/for_statement.hpp>
#include <ast/stmt/loop_control.hpp>
#include <ast/stmt/statement.hpp>
#include <ast/stmt/statements.hpp>
#include <ast/stmt/while_statement.hpp>
#include <ast/type/type_parameters.hpp>
#include <ast/type/type_reference.hpp>
#include <migration/structure.hpp>
#include <token/token.hpp>

namespace cloth::migration {
namespace {

enum class Boundary { List, End, Missing, Present };
using Value = std::variant<Boundary, bool, std::size_t, std::string, std::type_index>;

struct Part {
	Value     value;
	TokenSpan at;
};

// An exact preorder comparison. List boundaries preserve nesting and arity. Coordinates
// travel with each part only to locate differences and never participate in equality.
class Structure {
    public:
	explicit Structure (const SourceUnit &unit) : at(unit.end.value_or(TokenSpan{})) {
		fields(unit.imports, unit.declarations);
	}

	std::vector<Part> parts;

    private:
	TokenSpan                            at;
	std::vector<const GuardedReceiver *> guards;

	void mark (Value value) {
		parts.push_back({ .value = std::move(value), .at = at });
	}

	template <class T> void node (const T &value, const TokenSpan &span) {
		at = span;
		mark(std::type_index(typeid(value)));
	}

	template <class... T> void fields (const T &...values) {
		(add(values), ...);
	}

	template <class T> void add (const std::vector<T> &values) {
		mark(Boundary::List);
		for (const T &value : values) add(value);
		mark(Boundary::End);
	}

	template <class T> void add (const std::optional<T> &v) {
		mark(v ? Boundary::Present : Boundary::Missing);
		if (v) add(*v);
	}

	template <class T> void add (const std::shared_ptr<const T> &v) {
		mark(v ? Boundary::Present : Boundary::Missing);
		if (v) add(*v);
	}

	template <class... T> void add (const std::variant<T...> &v) {
		mark(v.index());
		std::visit([this] (const auto &item) { this->add(item); }, v);
	}

	void add (const bool v) {
		mark(v);
	}

	void add (const Token &v) {
		at = v.getSpan();
		mark(static_cast<std::size_t>(v.getType()));
		mark(v.getLexeme());
	}

	void add (const TypeReference &v) {
		at = v.getSpan();
		mark(v.getSpelling());
	}

	void add (const TypeArgumentList &v) {
		add(v.arguments);
	}

	void add (const TypeParameterList &v) {
		add(v.parameters);
	}

	void add (const TypeParameterDeclaration &v) {
		fields(v.name, v.bounds, v.defaultType);
	}

	void add (const TypeParameterBound &v) {
		add(v.value);
	}

	void add (const ConstructorBound &v) {
		signature(v.declaration);
	}

	void add (const ParameterDeclaration &v) {
		fields(v.name, v.type, v.finalKeyword, v.optKeyword, v.defaultDelimiter, v.defaultValue);
	}

	void add (const ImportDeclaration &v) {
		fields(v.path.components, v.selection.value);
	}

	void add (const ImportSelection::Names &v) {
		add(v.names);
	}

	void add (const ImportSelection::Wildcard &v) {
		add(v.star);
	}

	void add (const ClassParameterDeclaration &v) {
		fields(v.parameter, v.fieldKeyword, v.visibility);
	}

	void add (const ClassParentClause &v) {
		fields(v.type, v.constructorArguments);
	}

	void add (const ClassFieldDeclaration &v) {
		fields(v.keyword, v.type, v.name, v.visibility, v.initializer);
	}

	void add (const StructFieldDeclaration &v) {
		fields(v.type, v.name, v.visibility);
	}

	void add (const EnumFieldDeclaration &v) {
		fields(v.name, v.type, v.visibility, v.defaultValue);
	}

	void add (const EnumCaseDeclaration &v) {
		fields(v.name, v.visibility, v.values);
	}

	void add (const ClassMember &v) {
		add(v.value);
	}

	void add (const OperatorMember &v) {
		fields(v.selector, v.rightReceiver, v.provider);
	}

	void add (const CallArgument &v) {
		fields(v.label, v.expression);
	}

	void add (const InitialStatePrefix &v) {
		fields(v.keyword, v.payload);
	}

	void signature (const CallableSignature &v) {
		fields(v.keyword, v.getName(), v.visibility, v.returnTypes, v.parameters);
	}

	void add (const FunctionBody &v) {
		add(v.value);
	}

	void add (const BlockBody &v) {
		fields(v.equals.has_value(), v.statements);
	}

	void add (const ExpressionBody &v) {
		add(v.expression);
	}

	void add (const IntrinsicBody &v) {
		add(v.operation);
	}

	void add (const InterfaceMethodFallback &v) {
		add(v.body);
	}

	void add (const MethodDeclaration &v) {
		signature(v);
		fields(v.typeParameters, v.operatorMember, v.overridePrefix.has_value(), v.virtualPrefix.has_value(), v.body);
	}

	void add (const InterfaceMethodDeclaration &v) {
		signature(v);
		fields(v.typeParameters, v.fallback);
	}

	void add (const ConstructorDeclaration &v) {
		signature(v);
		fields(v.delegation, v.body);
	}

	void add (const DestructorDeclaration &v) {
		signature(v);
		add(v.body);
	}

	void stateBody (const StateBodyDeclaration &v) {
		fields(v.keyword, v.name, v.visibility, v.payloadType, v.members);
	}

	void add (const StateClassMember &member) {
		node(member, member.getSpan());
		if (const auto *v = dynamic_cast<const StateMethodContractDeclaration *>(&member)) {
			signature(*v);
			add(v->typeParameters);
		} else if (const auto *v = dynamic_cast<const StateAttachmentDeclaration *>(&member))
			fields(v->name, v->initial, v->visibility);
		else if (const auto *v = dynamic_cast<const NestedStateDeclaration *>(&member)) {
			stateBody(*v);
			add(v->initial);
		} else
			throw std::logic_error("Unrecognized state declaration in migration comparison");
	}

	void add (const StateMember &member) {
		node(member, member.getSpan());
		if (const auto *v = dynamic_cast<const StateInitializerDeclaration *>(&member))
			fields(v->visibility, v->body);
		else
			add(dynamic_cast<const MethodDeclaration &>(member));
	}

	void add (const Declaration &decl) {
		node(decl, decl.span);
		if (const auto *type = dynamic_cast<const TypeDeclaration *>(&decl)) fields(type->name, type->visibility, type->nestedTypes);
		if (const auto *v = dynamic_cast<const FunctionDeclaration *>(&decl)) {
			signature(*v);
			fields(v->typeParameters, v->body);
		} else if (const auto *v = dynamic_cast<const ClassDeclaration *>(&decl))
			fields(v->finalKeyword, v->typeParameters, v->parameters, v->parents, v->members);
		else if (const auto *v = dynamic_cast<const StructDeclaration *>(&decl))
			fields(v->typeParameters, v->fields, v->methods);
		else if (const auto *v = dynamic_cast<const EnumDeclaration *>(&decl))
			fields(v->fields, v->cases, v->operators);
		else if (const auto *v = dynamic_cast<const InterfaceDeclaration *>(&decl))
			fields(v->typeParameters, v->parents, v->methods);
		else if (const auto *v = dynamic_cast<const TopLevelStateDeclaration *>(&decl)) {
			stateBody(*v);
			fields(v->host, v->nestedTypes);
		} else
			throw std::logic_error("Unrecognized declaration in migration comparison");
	}

	void add (const ReceivingTarget &v) {
		add(v.getToken());
	}

	void add (const ForStepper &v) {
		fields(v.target, v.op, v.value);
	}

	void add (const Statement &stmt) {
		node(stmt, stmt.getSpan());
		if (const auto *v = dynamic_cast<const ExpressionStatement *>(&stmt))
			add(v->expression);
		else if (const auto *v = dynamic_cast<const ReturnStatement *>(&stmt))
			add(v->value);
		else if (const auto *v = dynamic_cast<const BindingDeclaration *>(&stmt))
			fields(v->name, v->type, v->isMutable, v->initializer);
		else if (const auto *v = dynamic_cast<const BindingGroupDeclaration *>(&stmt))
			fields(v->targets, v->types, v->isMutable, v->initializer);
		else if (const auto *v = dynamic_cast<const AssignmentStatement *>(&stmt))
			fields(v->name, v->compoundOperator, v->value);
		else if (const auto *v = dynamic_cast<const MultiAssignmentStatement *>(&stmt))
			fields(v->targets, v->value);
		else if (const auto *v = dynamic_cast<const FieldWriteStatement *>(&stmt))
			fields(v->target, v->op, v->value);
		else if (const auto *v = dynamic_cast<const ElementAssignmentStatement *>(&stmt))
			fields(v->target, v->op, v->value);
		else if (const auto *v = dynamic_cast<const UpdateStatement *>(&stmt))
			fields(v->name, v->op);
		else if (const auto *v = dynamic_cast<const LoopJumpStatement *>(&stmt))
			add(v->keyword);
		else if (const auto *v = dynamic_cast<const WhileStatement *>(&stmt))
			fields(v->condition, v->body.statements);
		else if (const auto *v = dynamic_cast<const DoWhileStatement *>(&stmt))
			fields(v->body.statements, v->condition);
		else if (const auto *v = dynamic_cast<const ForStatement *>(&stmt)) {
			fields(v->binding.type, v->binding.name);
			mark(v->source.value.index());
			fields(v->source.getExpression(), v->stepper, v->body.statements);
		} else
			throw std::logic_error("Unrecognized statement in migration comparison");
	}

	void add (const ConditionalBranch &v) {
		fields(v.condition, v.statements);
	}

	void add (const Expression &expr) {
		node(expr, expr.getSpan());
		if (const auto *v = dynamic_cast<const NameExpression *>(&expr))
			add(v->name);
		else if (const auto *v = dynamic_cast<const IntegerLiteral *>(&expr))
			add(v->token);
		else if (const auto *v = dynamic_cast<const FloatingPointLiteral *>(&expr))
			mark(v->value);
		else if (const auto *v = dynamic_cast<const BooleanLiteral *>(&expr))
			add(v->value);
		else if (const auto *v = dynamic_cast<const CharacterLiteral *>(&expr))
			mark(static_cast<std::size_t>(v->value));
		else if (const auto *v = dynamic_cast<const StringLiteral *>(&expr))
			mark(v->value);
		else if (const auto *v = dynamic_cast<const InterpolatedString *>(&expr))
			add(v->parts);
		else if (const auto *v = dynamic_cast<const ArrayLiteral *>(&expr))
			add(v->elements);
		else if (const auto *v = dynamic_cast<const StructLiteral *>(&expr))
			add(v->fields);
		else if (const auto *v = dynamic_cast<const ResultListExpression *>(&expr))
			add(v->values);
		else if (const auto *v = dynamic_cast<const IfExpression *>(&expr))
			fields(v->branches, v->otherwise);
		else if (const auto *v = dynamic_cast<const FunctionBlockExpression *>(&expr))
			add(v->declaration);
		else if (const auto *v = dynamic_cast<const BinaryExpression *>(&expr))
			fields(v->left, v->op, v->right);
		else if (const auto *v = dynamic_cast<const UnaryExpression *>(&expr))
			fields(v->op, v->operand);
		else if (const auto *v = dynamic_cast<const ElvisExpression *>(&expr))
			fields(v->left, v->op, v->fallback);
		else if (const auto *v = dynamic_cast<const IndexExpression *>(&expr))
			fields(v->array, v->index);
		else if (const auto *v = dynamic_cast<const FieldAccessExpression *>(&expr))
			fields(v->receiver, v->name);
		else if (const auto *v = dynamic_cast<const ProviderAccessExpression *>(&expr))
			fields(v->receiver, v->name);
		else if (const auto *v = dynamic_cast<const TypeReceiverExpression *>(&expr))
			add(v->annotation);
		else if (const auto *v = dynamic_cast<const CastExpression *>(&expr))
			fields(v->operand, v->op, v->target);
		else if (const auto *v = dynamic_cast<const TypeCheckExpression *>(&expr))
			fields(v->operand, v->op, v->target, v->valueCandidate);
		else if (const auto *v = dynamic_cast<const ClassDeletionExpression *>(&expr))
			fields(v->receiver, v->keyword);
		else if (const auto *v = dynamic_cast<const StateTransitionExpression *>(&expr))
			fields(v->receiver, v->op, v->target, v->payload);
		else if (const auto *v = dynamic_cast<const GuardedAccessExpression *>(&expr)) {
			fields(v->op, v->getReceiver());
			guards.push_back(v->payload.get());
			add(v->continuation);
			guards.pop_back();
		} else if (const auto *v = dynamic_cast<const GuardedReceiver *>(&expr)) {
			const auto it = std::find(guards.rbegin(), guards.rend(), v);
			if (it == guards.rend()) throw std::logic_error("Unbound guarded receiver in migration comparison");
			mark(static_cast<std::size_t>(it - guards.rbegin()));
		} else if (const auto *call = dynamic_cast<const CallSite *>(&expr)) {
			if (const auto *v = dynamic_cast<const CallExpression *>(call))
				add(v->typeArguments);
			else if (const auto *v = dynamic_cast<const MemberCallExpression *>(call))
				fields(v->receiver, v->typeArguments);
			else if (const auto *v = dynamic_cast<const StateCallExpression *>(call))
				fields(v->receiver, v->typeArguments);
			else if (const auto *v = dynamic_cast<const ClassCreationExpression *>(call))
				add(v->receiver);
			else
				add(dynamic_cast<const ConstructorDelegation &>(*call).type);
			fields(call->name, call->suppliedArguments);
		} else if (!dynamic_cast<const NullLiteral *>(&expr))
			throw std::logic_error("Unrecognized expression in migration comparison");
	}
};
}

std::optional<TokenSpan> firstParseDifference (const SourceUnit &reference, const SourceUnit &candidate) {
	const Structure before(reference), after(candidate);
	const auto      mismatch = std::ranges::mismatch(before.parts, after.parts, {}, &Part::value, &Part::value);
	if (mismatch.in1 != before.parts.end()) return mismatch.in1->at;
	if (mismatch.in2 != after.parts.end()) return reference.end.value_or(TokenSpan{});
	return std::nullopt;
}
}
