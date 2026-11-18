// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <ast/decl/class_declaration.hpp>
#include <ast/stmt/loop_control.hpp>
#include <ast/stmt/statements.hpp>
#include <cde/cloth_diagnostic.hpp>
#include <semantic/checked_program.hpp>

namespace cloth::semantic {

// Built-in providers are selected by identifier after the provider-access operator.
namespace provider {
inline constexpr std::string_view S_SIZE   = "size";
inline constexpr std::string_view S_TYPE   = "type";
inline constexpr std::string_view S_STRING = "str";
inline constexpr std::string_view S_MIN    = "min";
inline constexpr std::string_view S_MAX    = "max";
}

std::string_view unsignedIntegerSpelling ();
std::string      arraySuffix (std::string_view extent = {});

using Code       = cde::DiagnosticCode;
using Candidates = std::vector<const Symbol *>;
using Names      = std::map<std::string, Candidates, std::less<>>;

[[noreturn]] void        fail (Code code, const TokenSpan &span, std::string message, const Token *related = nullptr);
const CallableSignature *callable (const Symbol &symbol);
const TypeParameterList *typeParameters (const Symbol &symbol);
ResolvedType             substitute (ResolvedType type, const Substitutions &arguments);

struct Candidate {
	const Symbol            *symbol;
	Substitutions            arguments  = {};
	const TypeParameterList *inferOwner = nullptr;
};

class Analyzer {
    public:
	explicit Analyzer (CheckedProgram &program);
	CheckedProgram                                      &program;
	std::unordered_map<const Symbol *, DeclarationInfo> &declarations;

	[[nodiscard]] ResolvedType type (const TypeReference &reference, const Symbol &context, unsigned depth = 0, bool allowOpen = false) const;
	[[nodiscard]] ResolvedType checkedType (const TypeReference &reference, const Symbol &context) const;
	[[nodiscard]] const TypeParameterDeclaration *genericParameter (const Symbol &context, const std::string &name) const;
	[[nodiscard]] bool                            hasTypeName (const Symbol &context, const Token &name) const;
	[[nodiscard]] ResolvedType                    ownerType (const Symbol &symbol) const;
	[[nodiscard]] Substitutions                   argumentsOf (const ResolvedType &type) const;
	[[nodiscard]] Candidates                      lookup (const Symbol &context, const std::string &name) const;
	[[nodiscard]] std::vector<Candidate>          members (const ResolvedType &type, std::string_view name, SymbolKind kind) const;
	[[nodiscard]] bool                            accessible (const Symbol &symbol, const Symbol &context) const;
	void                                          requireAccess (const Symbol &symbol, const Symbol &context, const TokenSpan &use) const;
	[[nodiscard]] bool                            accepts (const ResolvedType &expected, const ResolvedType &actual) const;
	void                                          checkBounds (const ResolvedType &type, const TokenSpan &where, const Symbol &use) const;
	bool satisfies (const TypeParameterDeclaration &parameter, const Substitutions &arguments, const Symbol &declaration, const Symbol &use) const;
	void checkBodies ();
	const ResultTypes &resultsOf (const Symbol &symbol);

    private:
	std::vector<Names>       v_names;
	Candidates               v_order;
	void                     loadImports ();
	void                     registerScope (const SymbolScope &scope, std::size_t file, const Symbol *owner);
	void                     resolveDeclaration (const Symbol &symbol);
	void                     checkDeclarations ();
	void                     checkInterfaces (const Symbol &symbol) const;
	bool                     sameSignature (const Candidate &left, const Candidate &right, bool includeResults = false) const;
	void                     checkBodyOf (const Symbol &symbol);
	std::set<const Symbol *> s_checking, s_checked;
	std::unordered_map<const TypeParameterDeclaration *, const Symbol *> m_genericOwners;
	std::unordered_map<const TypeParameterDeclaration *, Candidates>     m_guarantees;
};

struct Binding {
	ResolvedType type;
	const Token *name;
	bool         mutableBinding;
	bool         deferred = false;
};

class BodyChecker {
    public:
	BodyChecker (Analyzer &analyzer, const Symbol &context);
	void check ();

    private:
	Analyzer                                   &analyzer;
	const Symbol                               &context;
	const Symbol                               *owner;
	ResultTypes                                 returns;
	std::vector<std::map<std::string, Binding>> scopes{ 1 };
	using Narrowing = std::map<const Token *, ResolvedType>;
	Narrowing narrowed;
	unsigned  loopDepth       = 0;
	bool      loopMayBreak    = false;
	bool      loopMayContinue = false;
	bool      record          = true;

	[[nodiscard]] const Binding *local (const std::string &name) const;
	void                         bind (const Token &name, ResolvedType type, bool mutableBinding, bool deferred = false);
	[[nodiscard]] ResolvedType   selfType () const;
	ExpressionInfo               call (const CallExpression &node);
	ExpressionInfo               create (const ClassCreationExpression &node, const ResolvedType *hint);
	ExpressionInfo               cast (const CastExpression &node);
	bool                         loop (const LoopStatement &node);
	ExpressionInfo               expression (const Expression &node, const ResultTypes *expected = nullptr, bool discard = false);
	ExpressionInfo               infer (const Expression &node, const ResultTypes *expected, bool discard);
	ResolvedType                 scalar (const Expression &node, const ResolvedType *expected = nullptr);
	void                     require (const ResultTypes &actual, const ResultTypes &expected, const TokenSpan &span, Code code = Code::TYPE_MISMATCH) const;
	void                     expect (const Expression &node, const ResolvedType &type);
	ExpressionInfo           name (const Token &token);
	ResolvedType             receiver (const Expression &node, bool &isType, bool allowOpen = false);
	ExpressionInfo           field (const FieldAccessExpression &node);
	std::optional<Candidate> stateTarget (const ResolvedType &receiver, const Token &name);
	ExpressionInfo           transition (const Candidate &state, const ExpressionPtr &payload, const TokenSpan &span);
	ExpressionInfo           typeCheck (const TypeCheckExpression &node);
	ExpressionInfo           binary (const BinaryExpression &node, const ResolvedType *expected);
	ExpressionInfo invoke (const Token &name, const TokenSpan &span, const std::vector<CallArgument> &arguments, const std::vector<Candidate> &group,
	                       const TypeArgumentList *typeArguments = nullptr, const ResultTypes *knownArguments = nullptr);
	ExpressionInfo condition (const IfExpression &node, const ResultTypes *expected, bool discard);
	ExpressionInfo block (const std::vector<StatementPtr> &statements, const ResultTypes *expected = nullptr, bool value = false);
	bool           statement (const Statement &node);
	void           assign (const Token &name, const ResolvedType &value);
	bool           receive (const std::vector<ReceivingTarget> &targets, const Expression &producer, const std::vector<TypeReference> *annotations,
	                        bool mutableBinding = false);
	void           writable (const Expression &target);
	bool           updateOperator (const Statement &statement, const ResolvedType &receiver, const Token &op, const ExpressionPtr &rhs);
	void writeMember (const Statement &statement, const ExpressionPtr &target, const Token &op, const ExpressionPtr &rhs, const ExpressionPtr &compound);
	void checkBody (const FunctionBody &body);
	void remember (const Token &use, const Token &declaration) const;
	void refine (const Expression &test, bool truth);
};

}
