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
#include <ast/expr/member_call_expression.hpp>
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

// An enclosing return contract is fixed even while its containing argument is being probed.
struct ReturnDiagnostic : cde::ClothDiagnostic {
	explicit ReturnDiagnostic (const ClothDiagnostic &error) : ClothDiagnostic(error) {
	}
};

[[noreturn]] void        fail (Code code, const TokenSpan &span, std::string message, const Token *related = nullptr);
const CallableSignature *callable (const Symbol &symbol);
const TypeParameterList *typeParameters (const Symbol &symbol);

struct Candidate {
	const Symbol            *symbol;
	Substitutions            arguments           = {};
	const TypeParameterList *inferOwner          = nullptr;
	const Symbol            *overridingMethod    = nullptr;
	Substitutions            overridingArguments = {};
};

enum class MemberLookup { Declaration, CallContract };

class Analyzer {
    public:
	explicit Analyzer (CheckedProgram &program);
	CheckedProgram                                      &program;
	std::unordered_map<const Symbol *, DeclarationInfo> &declarations;

	[[nodiscard]] ResolvedType type (const TypeReference &reference, const Symbol &context, unsigned depth = 0, bool allowOpen = false) const;
	[[nodiscard]] ResolvedType checkedType (const TypeReference &reference, const Symbol &context, const Substitutions &arguments = {},
	                                        bool allowOpen = false) const;
	[[nodiscard]] ResolvedType substitute (ResolvedType type, const Substitutions &arguments) const;
	void                       applyArguments (ResolvedType &type, ResultTypes arguments) const;
	[[nodiscard]] const TypeParameterDeclaration *genericParameter (const Symbol &context, const std::string &name) const;
	[[nodiscard]] bool                            hasTypeName (const Symbol &context, const Token &name) const;
	[[nodiscard]] ResolvedType                    ownerType (const Symbol &symbol) const;
	[[nodiscard]] Substitutions                   argumentsOf (const ResolvedType &type) const;
	[[nodiscard]] Candidates                      lookup (const Symbol &context, const std::string &name) const;
	[[nodiscard]] Candidates                      lookupFile (const Symbol &context, const std::string &name) const;
	[[nodiscard]] std::vector<Candidate>          members (const ResolvedType &type, std::string_view name, SymbolKind kind,
	                                                       MemberLookup lookup = MemberLookup::Declaration) const;
	[[nodiscard]] bool                            accessible (const Symbol &symbol, const Symbol &context) const;
	void                                          requireAccess (const Symbol &symbol, const Symbol &context, const TokenSpan &use) const;
	[[nodiscard]] bool                            accepts (const ResolvedType &expected, const ResolvedType &actual) const;
	[[nodiscard]] bool                            supportsEquality (const ResolvedType &type) const;
	[[nodiscard]] bool                            referenceType (const ResolvedType &type) const;
	void                                          checkBounds (const ResolvedType &type, const TokenSpan &where, const Symbol &use) const;
	bool satisfies (const TypeParameterDeclaration &parameter, const Substitutions &arguments, const Symbol &declaration, const Symbol &use) const;
	void checkBodies ();
	const ResultTypes     &resultsOf (const Symbol &symbol);
	void                   useType (const ResolvedType &type, const Symbol &context, const TokenSpan &where);
	const GenericInstance *useCallable (const Symbol &symbol, const Substitutions &arguments, const Symbol &context, const TokenSpan &where);
	const Symbol          *constructorFor (const ResolvedType &type, const ResultTypes &parameters, const Symbol *access) const;
	Candidate              boundMethod (const ExpressionInfo &call, const std::vector<Candidate> &candidates, const Substitutions &arguments) const;
	const GenericInstance *guaranteeOwner (const Symbol &guarantee, const Substitutions &arguments, const Symbol &context, const TokenSpan &where);

    private:
	struct ByArguments {
		bool operator()(const std::shared_ptr<const TypeArguments> &left, const std::shared_ptr<const TypeArguments> &right) const {
			return left->values < right->values;
		}
	};

	mutable std::set<std::shared_ptr<const TypeArguments>, ByArguments> s_arguments;
	[[nodiscard]] DeclarationInfo                                       specialize (DeclarationInfo info, const Substitutions &arguments) const;

	struct InstanceUse {
		InstanceKey key;
		TokenSpan   where;
	};

	struct InstanceWork {
		const InstanceKey  &key;
		GenericInstance    *instance;
		const InstanceWork *parent;
		TokenSpan           where;
		std::size_t         depth;
	};

	std::unordered_map<const Symbol *, std::vector<InstanceUse>> m_uses;
	std::set<InstanceKey>                                        s_templates;
	std::deque<InstanceWork>                                     q_templates, q_instances;
	const InstanceWork                                          *iw_active    = nullptr;
	std::size_t                                                  st_templates = 0, st_instances = 0;
	using StorageEdges = std::vector<std::pair<ResolvedType, const Symbol *>>;

	struct StorageVisit {
		const ResolvedType &type;
		const StorageEdges &fields;
		std::size_t         next = 0;
	};

	std::map<ResolvedType, StorageEdges> m_storage;
	[[nodiscard]] const Symbol          *instanceOwner (const Symbol &symbol) const;
	[[nodiscard]] bool                   isTemplate (const Symbol &symbol) const;
	[[nodiscard]] Substitutions          instanceArguments (const InstanceKey &key) const;
	[[nodiscard]] bool                   hasConstructorProof (const ResolvedType &type, const ResultTypes &parameters) const;
	void                                 checkInstances ();
	void                                 completeInstance (const InstanceWork &work);
	void                                 checkStorage () const;
	void                                 collectSignature (const Symbol &symbol);
	void                                 useDeclaredBounds (const Symbol &symbol, const Substitutions &arguments);
	void                                 visitTypeMembers (const ResolvedType &type, const Substitutions &arguments, GenericInstance *instance);
	const GenericInstance               *useInstance (InstanceKey key, const Symbol &context, const TokenSpan &where);
	const GenericInstance               *enqueue (InstanceKey key, const TokenSpan &where, bool symbolic);
	std::vector<Names>                   v_names;
	Candidates                           v_order;
	void                                 loadImports ();
	void                                 registerScope (const SymbolScope &scope, std::size_t file, const Symbol *owner);
	void                                 resolveDeclaration (const Symbol &symbol);
	void                                 checkDeclarations ();
	void                                 checkOverload (const Symbol &symbol, const Substitutions &arguments, const TokenSpan *application) const;
	void                                 checkInterfaces (const Symbol &symbol) const;
	bool                                 sameSignature (const Candidate &left, const Candidate &right, bool includeResults = false) const;
	void                                 checkBodyOf (const Symbol &symbol);
	std::set<const Symbol *>             s_checking, s_checked;
	std::unordered_map<const TypeParameterDeclaration *, const Symbol *> m_genericOwners;
	using Guarantees = std::unordered_map<const TypeParameterDeclaration *, Candidates>;
	Guarantees m_guarantees;
};

struct Binding {
	// An unannotated binding after a noncontinuing initializer has no stored value.
	std::optional<ResolvedType> type;
	const Token                *name;
	bool                        mutableBinding;
	bool                        deferred = false;
};

class BodyChecker {
    public:
	BodyChecker (Analyzer &analyzer, const Symbol &context, BodyFacts &facts, const Substitutions &arguments, DeclarationInfo &signature);
	void check ();

    private:
	enum class ReceiverPhase { Ready, ConstructorDefault, Delegation, FieldInitializer };

	struct Receiver {
		ResolvedType      type;
		bool              isType       = false;
		ReceiverFieldPath fieldPath    = ReceiverFieldPath::None;
		bool              fallsThrough = true;
	};

	struct GuardedPayload {
		const GuardedReceiver &node;
		ExpressionInfo         value;
	};

	const GuardedPayload                       *gp_payload       = nullptr;
	ReceiverPhase                               rp_receiverPhase = ReceiverPhase::Ready;
	const ParameterDeclaration                 *pd_default       = nullptr;
	bool                                        b_nestedFunction = false;
	Analyzer                                   &analyzer;
	const Symbol                               &context;
	BodyFacts                                  &bf_facts;
	const Substitutions                        &m_arguments;
	DeclarationInfo                            &di_signature;
	const Symbol                               *owner;
	ResultTypes                                 returns;
	std::vector<std::map<std::string, Binding>> scopes{ 1 };
	using Narrowing = std::map<const Token *, ResolvedType>;

	struct FlowState {
		Narrowing narrowed;
		bool      loopMayBreak                        = false;
		bool      loopMayContinue                     = false;
		bool      operator==(const FlowState &) const = default;
	};

	struct ArgumentProbe {
		std::optional<ExpressionInfo> result;
		FlowState                     before, after;
	};

	FlowState fs_flow;
	unsigned  loopDepth = 0;
	bool      record    = true;

	[[nodiscard]] const Binding *local (const std::string &name) const;
	void                         bind (const Token &name, std::optional<ResolvedType> type, bool mutableBinding, bool deferred = false);
	void                         checkDefaultName (const Token &name) const;
	bool                         readOnlyDefaultRoot (const Expression &node) const;
	[[nodiscard]] ResolvedType   selfType () const;
	ResolvedType                 bodyType (const TypeReference &reference, bool allowOpen = false);
	[[nodiscard]] bool           hasInstance () const;
	void                         checkFieldRead (const Symbol &field, const Token &use) const;
	void                         checkFieldWrite (const ExpressionInfo &target, const TokenSpan &span) const;
	ResolvedType                 recordSelf (const Expression &node);
	ExpressionInfo               call (const CallExpression &node);
	ExpressionInfo               create (const ClassCreationExpression &node, const ResolvedType *hint);
	ExpressionInfo               cast (const CastExpression &node);
	bool                         loop (const LoopStatement &node);
	ExpressionInfo               expression (const Expression &node, const ResultTypes *expected = nullptr, bool discard = false);
	BodyChecker                 &onPath (bool reachable, std::optional<BodyChecker> &unreachable);
	bool           checkArguments (const std::vector<CallArgument> &arguments, const ResultTypes &parameters, const std::vector<ArgumentProbe> &probes,
	                               bool contextual);
	ExpressionInfo infer (const Expression &node, const ResultTypes *expected, bool discard);
	std::optional<ResolvedType> scalar (const Expression &node, const ResolvedType *expected = nullptr);
	ExpressionInfo              scalarInfo (const Expression &node, const ResolvedType *expected = nullptr);
	void                     require (const ResultTypes &actual, const ResultTypes &expected, const TokenSpan &span, Code code = Code::TYPE_MISMATCH) const;
	bool                     expect (const Expression &node, const ResolvedType &type);
	void                     checkReturn (const Expression &node);
	ExpressionInfo           name (const Token &token);
	std::optional<Receiver>  receiver (const Expression &node, bool allowOpen = false);
	ExpressionInfo           field (const FieldAccessExpression &node);
	std::optional<Candidate> stateTarget (const ResolvedType &receiver, const Token &name);
	ExpressionInfo           transition (const Candidate &state, const ExpressionPtr &payload, const TokenSpan &span);
	ExpressionInfo           typeCheck (const TypeCheckExpression &node);
	ExpressionInfo           binary (const BinaryExpression &node, const ResolvedType *expected);
	ExpressionInfo invoke (const Token &name, const TokenSpan &span, const std::vector<CallArgument> &arguments, const std::vector<Candidate> &group,
	                       const TypeArgumentList *typeArguments = nullptr, const ResultTypes *knownArguments = nullptr);
	std::vector<Candidate> methodCandidates (const MemberCallExpression &site, const ResolvedType &receiver) const;
	ExpressionInfo         condition (const IfExpression &node, const ResultTypes *expected, bool discard);
	ExpressionInfo         block (const std::vector<StatementPtr> &statements, const ResultTypes *expected = nullptr, bool value = false);
	bool                   statement (const Statement &node);
	void                   assign (const Token &name, const std::optional<ResolvedType> &value);
	bool                   receive (const std::vector<ReceivingTarget> &targets, const Expression &producer, const std::vector<TypeReference> *annotations,
	                                bool mutableBinding = false);
	void                   writable (const Expression &target);
	std::optional<ExpressionInfo> updateOperator (const Statement &statement, const ResolvedType &receiver, const Token &op, const ExpressionPtr &rhs);
	bool writeMember (const Statement &statement, const ExpressionPtr &target, const Token &op, const ExpressionPtr &rhs, const ExpressionPtr &compound);
	void checkBody (const FunctionBody &body);
	void remember (const Token &use, const Token &declaration) const;
	void refine (const Expression &test, bool truth);
};

}
