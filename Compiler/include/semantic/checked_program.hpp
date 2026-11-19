// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <compare>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <ast/expr/expression.hpp>
#include <ast/type/type_parameters.hpp>
#include <semantic/symbol_table.hpp>
#include <token/token.hpp>

namespace cloth {

struct BinaryExpression;
struct ClassParentClause;
struct GenericInstance;
struct TypeArguments;
struct TypeCheckExpression;

struct ResolvedType {
	std::string                          name;
	const Symbol                        *declaration = nullptr;
	const TypeParameterDeclaration      *parameter   = nullptr;
	std::shared_ptr<const TypeArguments> application;
	// Suffixes are ordered from the element outward, so int?[] differs from int[]?.
	std::vector<std::string> suffixes;
	unsigned                 width = 0;

	explicit ResolvedType (std::string name = {});
	explicit ResolvedType (Keyword keyword);
	bool                                           operator==(const ResolvedType &other) const;
	std::strong_ordering                           operator<=>(const ResolvedType &other) const;
	[[nodiscard]] const std::vector<ResolvedType> &arguments () const;
	[[nodiscard]] bool                             dependent () const;
	[[nodiscard]] unsigned                         argumentDepth () const;
	void                                           visitApplications (const std::function<void(const ResolvedType &)> &visit) const;
	[[nodiscard]] bool                             nullable () const;
	[[nodiscard]] bool                             array () const;
	[[nodiscard]] bool                             integer () const;
	[[nodiscard]] bool                             floating () const;
	[[nodiscard]] ResolvedType                     element () const;
	[[nodiscard]] std::string                      spelling () const;
};

using ResultTypes   = std::vector<ResolvedType>;
using Substitutions = std::map<const TypeParameterDeclaration *, ResolvedType>;

// Canonical argument lists share nested types, including repeated uses of the same parameter.
struct TypeArguments {
	ResultTypes values;
	unsigned    depth;
	bool        dependent;
};

struct DeclarationInfo {
	std::size_t                 file;
	const Symbol               *owner;
	ResultTypes                 parameters         = {};
	ResultTypes                 returns            = {};
	ResultTypes                 parents            = {};
	std::optional<ResolvedType> value              = {};
	const Symbol               *attachedState      = nullptr;
	bool                        inferResult        = false;
	const Symbol               *primaryConstructor = nullptr;
};

// Stored paths select storage in the enclosing instance. Indirect paths continue
// through a reference or a user-defined index operation.
enum class ReceiverFieldPath { None, Stored, Indirect };

enum class ResultKind { Empty, Single, Multiple, Never };

// The owner distinguishes applications of the same generic method declaration.
struct MethodRef {
	const Symbol *symbol;
	ResolvedType  owner;
	bool          operator==(const MethodRef &) const = default;
};

enum class DispatchKind { Direct, Virtual, Interface, State };

struct MethodSlot {
	MethodRef                contract;
	std::optional<MethodRef> implementation;
	DispatchKind             kind;
};

struct InterfaceTarget {
	ResolvedType view;
	MethodRef    contract;
	MethodRef    implementation;
};

struct StateSelection {
	const Symbol *selection;
	const Symbol *definition;
	ResolvedType  owner;
	ResolvedType  payload;
};

struct StateMethod {
	MethodRef contract;
	// Each key is a supported state selection on this host.
	std::map<const Symbol *, MethodRef> implementations;
};

using StateSelections = std::map<std::string, StateSelection, std::less<>>;

struct TypeDispatch {
	std::vector<MethodSlot>      methods;
	std::vector<ResolvedType>    interfaces;
	std::vector<InterfaceTarget> interfaceTargets;
	StateSelections              states;
	const Symbol                *initialState = nullptr;
	std::vector<StateMethod>     stateMethods;
};

// The static receiver and contract select a slot in the concrete receiver's dispatch table.
struct CallDispatch {
	DispatchKind kind;
	ResolvedType receiver;
	MethodRef    contract;
};

struct ExpressionInfo {
	ResultTypes   types;
	const Symbol *symbol  = nullptr;
	const Token  *binding = nullptr;
	// Owns the labels, default expressions and generic parameters used to bind this call.
	const Symbol *argumentContract = nullptr;
	// Supplied expressions stay in source order. Omitted parameters follow in declaration order.
	std::vector<std::size_t>    parameterForArgument = {};
	std::vector<std::size_t>    omittedParameters    = {};
	bool                        fallsThrough         = true;
	Substitutions               typeArguments        = {};
	ReceiverFieldPath           receiverFieldPath    = ReceiverFieldPath::None;
	const GenericInstance      *instance             = nullptr;
	const GenericInstance      *contractInstance     = nullptr;
	std::optional<ResolvedType> representedType      = std::nullopt;
	std::optional<CallDispatch> dispatch             = std::nullopt;

	[[nodiscard]] ResultKind resultKind () const;
	static ExpressionInfo    never ();
};

struct BodyFacts {
	std::unordered_map<const Expression *, ExpressionInfo>        expressions;
	std::unordered_map<const ClassParentClause *, ExpressionInfo> parentConstructors;
	std::unordered_map<const Token *, ResolvedType>               localTypes;
	// An "is" target can resolve to a value after parsing. Keep that expression alive with its bindings.
	std::unordered_map<const TypeCheckExpression *, std::shared_ptr<const BinaryExpression>> valueTests;
	std::unordered_map<const Statement *, ExpressionInfo>                                    writes;
	std::unordered_map<const Token *, const Token *>                                         references;
};

struct InstanceKey {
	const Symbol               *declaration;
	std::optional<ResolvedType> receiver;
	ResultTypes                 arguments;

	std::strong_ordering operator<=>(const InstanceKey &other) const;
};

struct GenericInstance {
	DeclarationInfo                        signature;
	BodyFacts                              body;
	std::map<const Symbol *, ResolvedType> fields;
};

struct CheckedProgram : BodyFacts {
	explicit CheckedProgram (SymbolTable entry);

	// Own the declarations and stable instances referenced by the body facts.
	std::vector<std::unique_ptr<const SymbolTable>>     units;
	std::deque<Symbol>                                  constructorRequirements;
	std::unordered_map<const Symbol *, DeclarationInfo> declarations;
	std::map<InstanceKey, GenericInstance>              instances;
	std::map<ResolvedType, TypeDispatch>                dispatchTables;
	// A standalone state keeps its lexical scope. State selections in its body belong to each attaching class.
	std::map<ResolvedType, BodyFacts> attachmentBodies;
};
}
