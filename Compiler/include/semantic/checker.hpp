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
#include <ast/decl/enum_declaration.hpp>
#include <ast/decl/interface_declaration.hpp>
#include <ast/expr/member_call_expression.hpp>
#include <ast/expr/provider_expressions.hpp>
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
inline constexpr std::string_view S_NAME = "name", S_VALUES = "values";
inline constexpr std::string_view S_ELEMENT = "element", S_RANK = "rank", S_EXTENT = "extent", S_WIDTH = "width";
inline constexpr std::string_view S_ISARRAY = "isarray", S_HASEXTENT = "hasextent", S_ISUNSIGNED = "isunsigned", S_ISNUMERIC = "isnumeric";
}

std::string_view             unsignedIntegerSpelling ();
double                       floatingLiteral (std::string_view text, unsigned width);
NumericConstant              numericLimit (const ResolvedType &type, TargetLayout layout, bool maximum);
void                         checkNumeric (const Expression &node, ExpressionInfo &result, const BodyFacts &facts, TargetLayout layout);
void                         requireFinite (const Expression &operand, const BodyFacts &facts, cde::DiagnosticCode code);
std::string                  arraySuffix (std::string_view extent = {});
enum class ElementEvidence { None, Literal, Typed };
ElementEvidence              elementEvidence (const Expression &operand);
std::optional<ArrayMutation> arrayMutationNamed (std::string_view name);
bool                         hasArrayShape (const ResolvedType &type);
ResolvedType                 comparisonShape (ResolvedType type, const ArrayLiteral *literal = nullptr);
ResolvedType                 refineShape (ResolvedType target, const ResolvedType &source, const TokenSpan *use = nullptr);
ResolvedType                 commonShape (ResolvedType left, const ResolvedType &right);

using Code       = cde::DiagnosticCode;
using Candidates = std::vector<const Symbol *>;
using Names      = std::map<std::string, Candidates, std::less<>>;

// An enclosing return contract is fixed even while its containing argument is being probed.
struct ReturnDiagnostic : cde::ClothDiagnostic {
	explicit ReturnDiagnostic (const ClothDiagnostic &error) : ClothDiagnostic(error) {
	}
};

[[noreturn]] void           fail (Code code, const TokenSpan &span, std::string message, const Token *related = nullptr);
const CallableSignature    *callable (const Symbol &symbol);
const TypeParameterList    *typeParameters (const Symbol &symbol);
const StructDeclaration    *asStruct (const Symbol *symbol);
const ClassDeclaration     *asClass (const Symbol *symbol);
const Symbol               &objectRoot ();
const InterfaceDeclaration *asInterface (const Symbol *symbol);
const TypeReference        &fieldType (const Symbol &symbol);

struct Candidate {
	const Symbol               *symbol;
	Substitutions               arguments           = {};
	const TypeParameterList    *inferOwner          = nullptr;
	const Symbol               *overridingMethod    = nullptr;
	Substitutions               overridingArguments = {};
	std::optional<CallDispatch> dispatch            = std::nullopt;
};

enum class MemberLookup { Declaration, CallContract };

class Analyzer {
    public:
	explicit Analyzer (CheckedProgram &program);
	CheckedProgram                                      &program;
	std::unordered_map<const Symbol *, DeclarationInfo> &declarations;

	[[nodiscard]] const Symbol            *indexValidator (const Symbol &operation) const;
	void                                   resolveOperator (const Symbol &operation, const MethodDeclaration &method);
	[[nodiscard]] bool                     hasOperator (std::string_view name) const;
	void                                   checkCollectionOperator (const Symbol &operation) const;
	[[nodiscard]] std::optional<Iteration> snapshotLayout (const ResolvedType &type) const;

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
	[[nodiscard]] bool                            numeric (const ResolvedType &type) const;
	[[nodiscard]] ResolvedType                    boundShape (const ResolvedType &type) const;
	[[nodiscard]] ResultTypes                     applicationsOf (const ResolvedType &actual, const Symbol &definition) const;
	[[nodiscard]] bool                            supportsEquality (const ResolvedType &type) const;
	[[nodiscard]] bool                            classEqualityMayCall (const ResolvedType &type, Operator operation = oEqual) const;
	[[nodiscard]] bool                            mayBeAbsent (const ResolvedType &type) const;
	[[nodiscard]] bool                            hasMultipleTypeBounds (const ResolvedType &type) const;
	[[nodiscard]] bool                            referenceType (const ResolvedType &type) const;
	void                                          checkBounds (const ResolvedType &type, const TokenSpan &where, const Symbol &use) const;
	void checkBounds (const TypeParameterDeclaration &parameter, const Substitutions &arguments, const Symbol &use, const TokenSpan &where) const;

	struct LifetimeBody {
		const Symbol               *context;
		BodyFacts                  *facts;
		std::optional<ResolvedType> host;
		const Symbol               *state;
	};

	void                   checkBodies ();
	[[nodiscard]] const Symbol *entryPoint () const;
	void                   recordLifetimeBody (LifetimeBody body, bool initializationMayCall);
	[[nodiscard]] bool     preservesNewReceiver (const ResolvedType &host, const Symbol &constructor) const;
	const ResultTypes     &resultsOf (const Symbol &symbol);
	void                   useType (const ResolvedType &type, const Symbol &context, const TokenSpan &where);
	const GenericInstance *useCallable (const Symbol &symbol, const Substitutions &arguments, const Symbol &context, const TokenSpan &where,
	                                    const ResolvedType *stateHost = nullptr);
	void                   useDispatch (const CallDispatch &dispatch, const Substitutions &arguments, const Symbol &context, const TokenSpan &where);
	const Symbol          *constructorFor (const ResolvedType &type, const ResultTypes &parameters, const Symbol *access) const;
	Candidate              boundMethod (const ExpressionInfo &call, const ResolvedType &receiver, const std::vector<Candidate> &candidates,
	                                    const Substitutions &arguments) const;
	bool                   accessible (const Candidate &candidate, const Symbol &context) const;
	const TypeDispatch    &dispatchFor (const ResolvedType &receiver) const;
	bool                   hasStateName (const Symbol &context, std::string_view name) const;
	const BodyFacts       &openBody (const ResolvedType *attachment) const;
	const GenericInstance *guaranteeOwner (const Symbol &guarantee, const Substitutions &arguments, const Symbol &context, const TokenSpan &where);

    private:
	const Symbol                                    *sym_number     = nullptr;
	const Symbol                                    *sym_owningList = nullptr;
	std::set<std::string, std::less<>>               s_operatorNames;
	std::vector<LifetimeBody>                        v_lifetimeBodies;
	std::map<ResolvedType, std::set<const Symbol *>> m_initializationEffects;
	void                                             checkLifetimes ();

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

	struct DispatchUse {
		CallDispatch call;
		TokenSpan    where;
	};

	using DispatchUses = std::map<InstanceKey, DispatchUse>;
	DispatchUses             m_dispatchUses;
	std::set<ResolvedType>   s_receivers;
	void                     registerReceiver (const ResolvedType &receiver);
	void                     instantiateTargets (const InstanceKey &key, const DispatchUse &use, const ResolvedType &receiver);
	std::set<InstanceKey>    s_templates;
	std::deque<InstanceWork> q_templates, q_instances;
	const InstanceWork      *iw_active    = nullptr;
	std::size_t              st_templates = 0, st_instances = 0;
	using StorageEdges = std::vector<std::pair<ResolvedType, const Symbol *>>;

	struct StorageVisit {
		const ResolvedType &type;
		const StorageEdges &fields;
		std::size_t         next = 0;
	};

	std::map<ResolvedType, StorageEdges>         m_storage;
	[[nodiscard]] const Symbol                  *instanceOwner (const Symbol &symbol) const;
	[[nodiscard]] bool                           isTemplate (const Symbol &symbol) const;
	[[nodiscard]] Substitutions                  instanceArguments (const InstanceKey &key) const;
	[[nodiscard]] bool                           hasConstructorProof (const ResolvedType &type, const ResultTypes &parameters) const;
	void                                         checkInstances ();
	void                                         completeInstance (const InstanceWork &work);
	void                                         checkStorage () const;
	void                                         collectSignature (const Symbol &symbol);
	void                                         useDeclaredBounds (const Symbol &symbol, const Substitutions &arguments);
	void                                         visitTypeMembers (const ResolvedType &type, const Substitutions &arguments, GenericInstance *instance);
	const GenericInstance                       *useInstance (InstanceKey key, const Symbol &context, const TokenSpan &where);
	const GenericInstance                       *enqueue (InstanceKey key, const TokenSpan &where, bool symbolic);
	std::vector<Names>                           v_names;
	Candidates                                   v_order;
	// A unit can be reached through several imports. Only selections from the standard library confer a library path.
	using LibraryPaths = std::map<std::size_t, std::set<std::string, std::less<>>>;
	LibraryPaths                                 m_libraryPaths;
	void                                         loadImports ();
	void                                         registerIntrinsics ();
	void                                         checkIntrinsicSignature (const Symbol &symbol, LibraryIntrinsic operation) const;
	void                                         registerScope (const SymbolScope &scope, std::size_t file, const Symbol *owner);
	void                                         resolveDeclaration (const Symbol &symbol);
	ResolvedType                                 resolveType (const TypeReference &reference, const Symbol &context, unsigned depth, bool allowOpen,
	                                                          std::set<const TypeParameterDeclaration *> &activeDefaults) const;
	void                                         checkParameterNames (const Symbol &symbol) const;
	void                                         checkDeclaredBounds (const TypeParameterDeclaration &parameter, const Symbol &symbol) const;
	void checkArrayBound (const TypeParameterDeclaration &parameter, const ResolvedType &shape, const TokenSpan &where) const;
	void                                         checkDeclarations ();
	void                                         checkOverload (const Symbol &symbol, const Substitutions &arguments, const TokenSpan *application) const;
	mutable std::map<ResolvedType, TypeDispatch> m_dispatch;
	using MethodGroup  = std::vector<MethodRef>;
	using MethodGroups = std::map<std::string, std::vector<MethodGroup>, std::less<>>;
	void          buildMethods (const ResolvedType &receiver, TypeDispatch &table) const;
	void          addClassMethod (const MethodRef &method, const MethodGroups &requirements, TypeDispatch &table) const;
	void          mergeRequirements (const ResolvedType &receiver, const MethodGroup &group, TypeDispatch &table) const;
	void          implementRequirements (const ResolvedType &receiver, const MethodGroup &group, const MethodRef &contract, const MethodGroup &defaults,
	                                     TypeDispatch &table) const;
	void          buildStates (const ResolvedType &receiver, TypeDispatch &table) const;
	void          publishDispatch ();
	void          checkPublicSurface (const Symbol &symbol, const Symbol *exportingHost = nullptr) const;
	bool          sameMethod (const MethodRef &left, const MethodRef &right, bool results = false) const;
	Substitutions methodArguments (const MethodRef &source, const Symbol &target) const;
	bool          sameMethodBounds (const MethodRef &left, const MethodRef &right, bool constructorContracts = false) const;
	bool          sameTypeDefaults (const MethodRef &left, const MethodRef &right) const;
	void          inheritMethodBounds (const MethodRef &body, const MethodRef &contract, Code code) const;

	struct ParameterContract {
		const TypeParameterDeclaration *parameter;
		Substitutions                   arguments;
	};

	using ParameterContracts = std::map<const TypeParameterDeclaration *, ParameterContract>;
	mutable ParameterContracts                           m_parameterContracts;
	mutable std::map<const Symbol *, ParameterContracts> m_attachmentContracts;
	std::unordered_map<const Symbol *, ResultTypes>      m_stateHosts;
	const ResolvedType                                  *rt_activeAttachment = nullptr;
	const Symbol                                        *standaloneState (const Symbol &member) const;
	void checkAttachedBody (const Symbol &member, const ResolvedType &host, BodyFacts &facts, const Substitutions &arguments, DeclarationInfo &signature);
	ParameterContract        parameterContract (const TypeParameterDeclaration &parameter) const;
	ResultTypes              typeBounds (const ResolvedType &parameter) const;
	bool                     acceptsBound (const ResolvedType &required, const ResolvedType &actual) const;
	bool                     sameSignature (const Candidate &left, const Candidate &right, bool includeResults = false) const;
	void                     checkBodyOf (const Symbol &symbol);
	std::set<const Symbol *> s_checking, s_checked;
	std::unordered_map<const TypeParameterDeclaration *, const Symbol *> m_genericOwners;
	using Guarantees = std::unordered_map<const TypeParameterDeclaration *, Candidates>;
	Guarantees m_guarantees;
};

enum class PlaceFact { Presence, CurrentShape, RetainedShape, RepresentedType };

struct StoragePlace {
	const Token                *root;
	std::vector<const Symbol *> fields                                  = {};
	PlaceFact                   fact                                    = PlaceFact::Presence;
	std::strong_ordering        operator<=>(const StoragePlace &) const = default;
};

struct WriteEffects {
	std::set<StoragePlace> places, receivers;
	bool                   sharedFields = false;
};

struct Binding {
	// An unannotated binding after a noncontinuing initializer has no stored value.
	std::optional<ResolvedType>         type;
	const Token                        *name;
	bool                                mutableBinding;
	bool                                deferred = false;
	std::shared_ptr<const WriteEffects> effects  = {};
};

class BodyChecker {
    public:
	BodyChecker (Analyzer &analyzer, const Symbol &context, BodyFacts &facts, const Substitutions &arguments, DeclarationInfo &signature,
	             const ResolvedType *attachment = nullptr);
	void check ();

    private:
	enum class ReceiverPhase { Ready, ConstructorDefault, Delegation, FieldInitializer };

	struct Receiver {
		ResolvedType                type;
		bool                        isType       = false;
		ReceiverFieldPath           fieldPath    = ReceiverFieldPath::None;
		bool                        fallsThrough = true;
		std::optional<ResolvedType> represented  = {};
	};

	struct GuardedPayload {
		const GuardedReceiver &node;
		ExpressionInfo         value;
	};

	struct CapturedOperand {
		const Expression      *node;
		ExpressionInfo         value;
		const CapturedOperand *previous;
	};

	const CapturedOperand *captured      = nullptr;
	const Expression      *validateAfter = nullptr;
	IndexedAccess          indexAccess (const ExpressionInfo &operation, const TokenSpan &span);
	bool writeIndex (const Statement &statement, const ExpressionPtr &target, const ExpressionInfo &array, const Token &op, const ExpressionPtr &rhs,
	                 const ExpressionPtr &compound);
	std::optional<ResolvedType>      rangeEndpoints (const Expression &start, const Expression &end, const ResolvedType *hint, Code code, bool &continues);
	ResolvedType                     shapeOf (const Expression &value, const ResolvedType &fallback, std::size_t slot = 0) const;
	void                             receiveShape (const Token &binding, const ResolvedType &declared, const ResolvedType &shape, const TokenSpan &use);
	ExpressionInfo                   mutateArray (const MemberCallExpression &call, const ExpressionInfo &receiver, ArrayMutation operation);
	void                             rowShape (const Expression &target, const Expression &value, const ResolvedType &incoming);
	void                             checkValidatorReceiver (const Expression *receiver, const TokenSpan &use) const;
	void                             enumValue (const EnumFieldDeclaration &field, const Expression &value, const Token *caseName);
	std::optional<ProviderSelection> textConversion (const ResolvedType &type, const TokenSpan &use);
	ExpressionInfo                   providerAccess (const ProviderAccessExpression &node);
	const GuardedPayload            *gp_payload       = nullptr;
	ReceiverPhase                    rp_receiverPhase = ReceiverPhase::Ready;
	const ParameterDeclaration      *pd_default       = nullptr;
	bool                             b_nestedFunction = false;
	Analyzer                        &analyzer;
	const Symbol                    &context;
	BodyFacts                       &bf_facts;
	const Substitutions             &m_arguments;
	DeclarationInfo                 &di_signature;
	const Symbol                    *owner;
	const ResolvedType              *rt_attachment;
	ResultTypes                      returns;
	std::vector<std::map<std::string, Binding>> scopes{ 1 };
	using Narrowing = std::map<StoragePlace, ResolvedType>;
	using JumpPaths = std::map<const LoopTarget *, Narrowing>;

	struct Exits {
		bool      returned = false;
		JumpPaths breaks, continues;
		bool      operator==(const Exits &) const = default;
	};

	struct FlowState {
		Narrowing narrowed;
		Exits     exits;
		bool      operator==(const FlowState &) const = default;
	};

	struct OperandProbe {
		FlowState    before, after;
		ResolvedType type;
	};

	using OperatorProbes           = std::unordered_map<const Expression *, OperandProbe>;
	OperatorProbes *operatorProbes = nullptr;

	struct TestPaths {
		std::optional<Narrowing> yes, no;
	};

	struct TypedPlace {
		StoragePlace place;
		ResolvedType type;
	};

	const LoopTarget                       *lt_loop    = nullptr;
	WriteEffects                           *we_capture = nullptr;
	std::shared_ptr<WriteEffects>           we_deferred;
	const Expression                       *e_test = nullptr;
	TestPaths                               tp_test;
	static void                             intersect (Narrowing &facts, const Narrowing &other);
	static void                             join (std::optional<Narrowing> &facts, const Narrowing &other);
	static void                             mergeExits (Exits &target, const Exits &source);
	void                                    invalidate (const StoragePlace &place, bool descendantsOnly = false);
	void                                    mutatingReceiver (const Expression &receiver, const ResolvedType &type);
	void                                    invalidateShared ();
	void                                    applyWrites (const Binding &binding);
	void                                    checkInferredBinding (const ResolvedType &type, const TokenSpan &span) const;
	void                                    setPresence (const StoragePlace &place, const ResolvedType &declared, const std::optional<ResolvedType> &value);
	void                                    assumePresent (const Expression &source);
	[[nodiscard]] std::optional<TypedPlace> locate (const Expression &expression) const;

	struct ReceiverProof {
		StoragePlace place;
		TokenSpan    span;
	};

	using ReceiverProofs = std::vector<ReceiverProof>;
	[[nodiscard]] ReceiverProofs            receiverProofs (const Expression *receiver) const;
	void                                    requireReceiverProofs (const ReceiverProofs &proofs) const;
	void                                    narrowRead (ExpressionInfo &value, const StoragePlace &place) const;
	TestPaths                               test (const Expression &node);
	TestPaths                               splitTest (const Expression &node, const ExpressionInfo &result);
	bool                                    checkStatement (const Statement &node);
	void                                    finishStatement (const Statement &node, bool fallsThrough, const Exits &incoming);
	void                                    typeBody ();
	void                                    leaveScope ();

	struct ArgumentProbe {
		std::optional<ExpressionInfo> result;
		FlowState                     before, after;
	};

	FlowState fs_flow;
	bool      record = true;
	bool      b_inferenceDefaults = false;

	[[nodiscard]] const Binding *local (const std::string &name) const;
	void                         bind (const Token &name, std::optional<ResolvedType> type, bool mutableBinding, bool deferred = false);
	void                         checkDefaultName (const Token &name) const;
	bool                         readOnlyDefaultRoot (const Expression &node) const;
	[[nodiscard]] ResolvedType   selfType () const;
	ResolvedType                 bodyType (const TypeReference &reference, bool allowOpen = false) const;
	[[nodiscard]] bool           hasInstance () const;
	void                         checkFieldRead (const Symbol &field, const Token &use) const;
	void                         checkFieldWrite (const ExpressionInfo &target, const TokenSpan &span) const;
	ResolvedType                 recordSelf (const Expression &node);
	ExpressionInfo               call (const CallExpression &node);
	ExpressionInfo               create (const ClassCreationExpression &node);
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
	bool                     checkReturn (const Expression &node);
	ExpressionInfo           name (const Token &token);
	std::optional<Receiver>  receiver (const Expression &node, bool allowOpen = false);
	ExpressionInfo           field (const FieldAccessExpression &node);
	std::optional<Candidate> stateTarget (const ResolvedType &receiver, const Token &name, const Expression &operand);
	bool                     statePayload (const ExpressionPtr &payload, const ResolvedType &required, const TokenSpan &span, const Token &state);
	ExpressionInfo           transition (const Candidate &state, const ExpressionPtr &payload, const TokenSpan &span);
	ExpressionInfo           typeCheck (const TypeCheckExpression &node);
	ExpressionInfo               binary (const BinaryExpression &node, const ResolvedType *expected, bool allowContexts = true);
	ExpressionInfo               elvis (const ElvisExpression &node, const ResultTypes *expected);
	std::optional<ExpressionInfo> binaryOperator (const BinaryExpression &node, const ResolvedType *expected, bool allowContexts);
	ExpressionInfo                equality (const BinaryExpression &node, const ResolvedType &left, const ResolvedType &right);
	std::optional<Candidate>      selectOperator (const Token &op, const TokenSpan &span, const ResolvedType &left, const ResolvedType &right,
	                                              bool &rightReceiver, const BinaryExpression *operands = nullptr) const;
	std::optional<Candidate>      selectContext (const Token &op, const ResolvedType &receiver, const ResolvedType &peer, bool typeTarget = false) const;
	ExpressionInfo callOperator (const Candidate &candidate, const Expression &receiver, const ResolvedType &type, const Token &op, const TokenSpan &span,
	                             const ExpressionPtr &peer = {}, const ResolvedType *peerType = nullptr);
	ExpressionInfo bindCall (const Token &name, const TokenSpan &span, const std::vector<CallArgument> &arguments, const std::vector<Candidate> &group,
	                         const TypeArgumentList *typeArguments = nullptr, const ResultTypes *knownArguments = nullptr,
	                         const ReceiverProofs &proofs = {});
	ExpressionInfo invoke (const Token &name, const TokenSpan &span, const std::vector<CallArgument> &arguments, const std::vector<Candidate> &group,
	                       const TypeArgumentList *typeArguments = nullptr, const ResultTypes *knownArguments = nullptr,
	                       const Expression *storedReceiver = nullptr);
	[[nodiscard]] std::vector<Candidate> methodCandidates (const MemberCallExpression &site, const ResolvedType &receiver) const;
	ExpressionInfo                       condition (const IfExpression &node, const ResultTypes *expected, bool discard);
	ExpressionInfo                       block (const std::vector<StatementPtr> &statements, const ResultTypes *expected = nullptr, bool value = false);
	bool                                 statement (const Statement &node);
	void assign (const Token &name, const std::optional<ResolvedType> &value, const Expression *source = nullptr, std::size_t slot = 0,
	             const std::optional<ResolvedType> &represented = {});
	bool receive (const std::vector<ReceivingTarget> &targets, const Expression &producer, const std::vector<TypeReference> *annotations,
	              bool mutableBinding = false);
	void writable (const Expression &target);
	std::optional<ExpressionInfo> updateOperator (const Statement &statement, const ResolvedType &receiver, const Token &op, const ExpressionPtr &rhs);
	bool writeStorage (const Statement &statement, const ExpressionPtr &target, const ExpressionInfo &selected, const Token &op, const ExpressionPtr &rhs,
	                   const ExpressionPtr &compound);
	bool writeMember (const Statement &statement, const ExpressionPtr &target, const Token &op, const ExpressionPtr &rhs, const ExpressionPtr &compound);
	void checkBody (const FunctionBody &body);
	void remember (const Token &use, const Token &declaration) const;
};

}
