// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <exception>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
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

std::string_view unsignedIntegerSpelling ();
double           floatingLiteral (std::string_view text, unsigned width);
NumericConstant  numericLimit (const ResolvedType &type, TargetLayout layout, bool maximum);
int              compareIntegers (const IntegerConstant &left, const IntegerConstant &right);
void             checkNumeric (const Expression &node, ExpressionInfo &result, const ExpressionFacts &facts, TargetLayout layout);
void             requireFinite (const Expression &operand, const ExpressionFacts &facts, cde::DiagnosticCode code);
void             requirePresentOperand (const ResolvedType &type, const TokenSpan &use);
std::string      arraySuffix (std::string_view extent = {});
enum class ElementEvidence { None, Literal, Typed };

// A numeric expression can have an integer peer and a separate format for floating literals.
struct FloatContext {
	bool     contextual = false;
	unsigned width      = 0;
};

struct NumericPeers {
	std::optional<ResolvedType> integer;
	unsigned                    floatingWidth = 0;
};

ElementEvidence              elementEvidence (const Expression &operand);
std::optional<ArrayMutation> arrayMutationNamed (std::string_view name);
bool                         builtinFunctionNamed (std::string_view name);
bool                         builtinNeverReturns (std::string_view name);
bool                         checksNumericMethod (std::string_view name);
bool                         hasArrayShape (const ResolvedType &type);
const ResolvedType          *rangeEndpoint (const ResolvedType &type);
void                         requireRangeMember (const Expression &member, const ResolvedType &actual, const ResolvedType &endpoint);
ResolvedType                 comparisonShape (ResolvedType type, const ArrayLiteral *literal = nullptr);
ResolvedType                 refineShape (ResolvedType target, const ResolvedType &source, const TokenSpan *use = nullptr);
ResolvedType                 commonShape (ResolvedType left, const ResolvedType &right);

using Code       = cde::DiagnosticCode;
using Candidates = std::vector<const Symbol *>;
using Names      = std::map<std::string, Candidates, std::less<>>;

struct GenericLimit {
	Code        code;
	std::string message;
};

struct GenericDependency {
	TokenSpan        use;
	std::string_view name;
};

[[noreturn]] void failGenericLimit (const GenericLimit &limit, const Token &declaration, const TokenSpan &use, const std::vector<GenericDependency> &ancestry);

std::optional<GenericLimit> genericLimit (unsigned nesting, std::size_t depth, std::size_t count, bool symbolic);

// The enclosing overload cannot change an intrinsic expression or explicit type.
struct IntrinsicDiagnostic : cde::ClothDiagnostic {
	using ClothDiagnostic::ClothDiagnostic;

	explicit IntrinsicDiagnostic (const ClothDiagnostic &error) : ClothDiagnostic(error) {
	}
};

bool contextualFailure (const cde::ClothDiagnostic &error);
bool validateArgumentLabels (const std::vector<CallArgument> &arguments);

[[noreturn]] void                fail (Code code, const TokenSpan &span, std::string message, const Token *related = nullptr);
[[noreturn]] void                unknownName (const Token &name);
[[noreturn]] void                duplicateBinding (const Token &name, const Token &previous);
const StateSelectionDeclaration *stateSelection (const Symbol &symbol);
bool                             acceptsTypeArgumentCount (const TypeParameterList *parameters, std::size_t count);
[[noreturn]] void                rejectTypeArgumentCount (const TypeArgumentList &arguments, const Symbol &declaration, bool generic);
const CallableSignature         *callable (const Symbol &symbol);
const TypeParameterList         *typeParameters (const Symbol &symbol);
const StructDeclaration         *asStruct (const Symbol *symbol);
const EnumDeclaration           *enumeration (const ResolvedType &type);
const ClassDeclaration          *asClass (const Symbol *symbol);
const Symbol                    &objectRoot ();
const InterfaceDeclaration      *asInterface (const Symbol *symbol);
const TypeReference             &fieldType (const Symbol &symbol);
const TypeReference             *widthSelector (const Symbol &symbol);

struct Candidate {
	const Symbol               *symbol;
	Substitutions               arguments           = {};
	const TypeParameterList    *inferOwner          = nullptr;
	const Symbol               *overridingMethod    = nullptr;
	Substitutions               overridingArguments = {};
	std::optional<CallDispatch> dispatch            = std::nullopt;
	unsigned                    operatorWidth       = 0;
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
	                                        bool allowOpen = false, unsigned operatorWidth = 0) const;
	[[nodiscard]] ResolvedType substitute (ResolvedType type, const Substitutions &arguments) const;
	void                       applyArguments (ResolvedType &type, ResultTypes arguments) const;
	[[nodiscard]] const TypeParameterDeclaration *genericParameter (const Symbol &context, const std::string &name) const;
	[[nodiscard]] bool                            hasTypeName (const Symbol &context, const Token &name, bool qualified = false) const;
	[[nodiscard]] ResolvedType                    ownerType (const Symbol &symbol) const;
	[[nodiscard]] Substitutions                   argumentsOf (const ResolvedType &type) const;
	[[nodiscard]] Candidates                      lookup (const Symbol &context, const std::string &name, std::optional<SymbolKind> kind = {}) const;
	[[nodiscard]] const Symbol                   *lookupTypeRoot (const Symbol &context, const std::string &name, bool qualified) const;
	[[nodiscard]] const Symbol                   *lookupFileState (const Symbol &context, const std::string &name) const;
	[[nodiscard]] Candidates                      lookupFile (const Symbol &context, const std::string &name) const;
	[[nodiscard]] std::vector<Candidate>          members (const ResolvedType &type, std::string_view name, SymbolKind kind,
	                                                       MemberLookup lookup = MemberLookup::Declaration) const;
	[[nodiscard]] bool                            accessible (const Symbol &symbol, const Symbol &context) const;
	void                                          requireAccess (const Symbol &symbol, const Symbol &context, const TokenSpan &use) const;
	[[nodiscard]] bool                            accepts (const ResolvedType &expected, const ResolvedType &actual) const;

	[[nodiscard]] bool hasReferenceDeclarations () const {
		return b_hasReferenceDeclarations;
	}

	[[nodiscard]] bool         numeric (const ResolvedType &type) const;
	[[nodiscard]] ResolvedType boundShape (const ResolvedType &type) const;
	[[nodiscard]] ResultTypes  typeBounds (const ResolvedType &parameter) const;
	[[nodiscard]] ResultTypes  applicationsOf (const ResolvedType &actual, const Symbol &definition) const;
	[[nodiscard]] bool         supportsEquality (const ResolvedType &type) const;
	[[nodiscard]] bool         classEqualityMayCall (const ResolvedType &type, Operator operation = oEqual) const;
	[[nodiscard]] bool         anyReceiverClass (const ResolvedType &type, const std::function<bool(const ResolvedType &)> &matches) const;
	void                       checkFormatterDefault (const ResolvedType &element, const TokenSpan &receiver, const Token &parameter, bool record);
	bool                       checkDefaultExpression (const std::function<bool()> &check);
	[[nodiscard]] bool         mayBeAbsent (const ResolvedType &type) const;
	[[nodiscard]] bool         hasMultipleTypeBounds (const ResolvedType &type) const;
	[[nodiscard]] bool         referenceType (const ResolvedType &type) const;
	void                       checkBounds (const ResolvedType &type, const TokenSpan &where, const Symbol &use) const;
	void checkBounds (const TypeParameterDeclaration &parameter, const Substitutions &arguments, const Symbol &use, const TokenSpan &where) const;

	struct LifetimeBody {
		const Symbol               *context;
		BodyFacts                  *facts;
		std::optional<ResolvedType> host;
		const Symbol               *state;
	};

	void                        checkBodies ();
	[[nodiscard]] const Symbol *entryPoint () const;
	void                        recordLifetimeBody (LifetimeBody body, bool initializationMayCall);
	[[nodiscard]] bool          preservesNewReceiver (const ResolvedType &host, const Symbol &constructor) const;
	const ResultTypes          &resultsOf (const Symbol &symbol);
	DeclarationInfo             widthSignature (const Symbol &symbol, const Substitutions &arguments, unsigned width);
	const BodyFacts            &widthTemplate (const Symbol &symbol, unsigned width);
	void                        useWidthAnnotation (const TypeReference &reference, const Symbol &owner);
	void                        useType (const ResolvedType &type, const Symbol &context, const TokenSpan &where);
	const GenericInstance      *useCallable (const Symbol &symbol, const Substitutions &arguments, const Symbol &context, const TokenSpan &where,
	                                         const ResolvedType *stateHost = nullptr, unsigned operatorWidth = 0);
	void                        useDispatch (const CallDispatch &dispatch, const Substitutions &arguments, const Symbol &context, const TokenSpan &where);
	const Symbol               *constructorFor (const ResolvedType &type, const ResultTypes &parameters, const Symbol *access) const;
	Candidate                   boundMethod (const ExpressionInfo &call, const ResolvedType &receiver, const std::vector<Candidate> &candidates,
	                                         const Substitutions &arguments) const;
	bool                        accessible (const Candidate &candidate, const Symbol &context) const;
	const TypeDispatch         &dispatchFor (const ResolvedType &receiver) const;
	bool                        hasStateName (const Symbol &context, std::string_view name) const;
	const BodyFacts            &openBody (const ResolvedType *attachment) const;
	const GenericInstance      *guaranteeOwner (const Symbol &guarantee, const Substitutions &arguments, const Symbol &context, const TokenSpan &where);

    private:
	bool                                             b_hasReferenceDeclarations = false;
	const Symbol                                    *sym_number                 = nullptr;
	const Symbol                                    *sym_owningList             = nullptr;
	std::set<std::string, std::less<>>               s_operatorNames;
	std::vector<LifetimeBody>                        v_lifetimeBodies;
	std::map<ResolvedType, std::set<const Symbol *>> m_initializationEffects;
	void                                             checkLifetimes ();

	struct FormatterDefault {
		ResultTypes        bounds;
		TokenSpan          receiver;
		const Token       *parameter;
		std::exception_ptr followingError;
	};

	std::vector<FormatterDefault> v_formatterDefaults;
	std::set<ResolvedType>        s_formatterClasses;
	void                          collectFormatterClasses ();
	void                          useFormatterClass (const ResolvedType &type);
	[[nodiscard]] bool            hasFormatterClass (const ResultTypes &bounds) const;
	void                          checkFormatterDefaults () const;

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
		GenericInstance    *instance{};
		const InstanceWork *parent{};
		TokenSpan           where;
		std::size_t         depth{};
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
	const InstanceWork      *iw_active        = nullptr;
	std::size_t              st_openTemplates = 0, st_closedTemplates = 0, st_instances = 0;
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
	// A unit can be reached through several imports. Only selections from the standard library confer a library path.
	using LibraryPaths = std::map<std::size_t, std::set<std::string, std::less<>>>;
	LibraryPaths m_libraryPaths;
	void         loadImports ();
	void         registerIntrinsics ();
	void         checkIntrinsicSignature (const Symbol &symbol, LibraryIntrinsic operation) const;
	void         registerScope (const SymbolScope &scope, std::size_t file, const Symbol *owner);
	void         resolveDeclaration (const Symbol &symbol);
	ResolvedType resolveType (const TypeReference &reference, const Symbol &context, unsigned depth, bool allowOpen,
	                          std::set<const TypeParameterDeclaration *> &activeDefaults, unsigned operatorWidth = 0) const;
	void         checkParameterNames (const Symbol &symbol) const;
	void         checkDeclaredBounds (const TypeParameterDeclaration &parameter, const Symbol &symbol) const;
	void         checkArrayBound (const TypeParameterDeclaration &parameter, const ResolvedType &shape, const TokenSpan &where) const;
	void         checkDeclarations ();
	void         checkInheritance () const;
	bool         usesSpecializedDeclarations () const;
	void         checkOverload (const Symbol &symbol, const Substitutions &arguments, const TokenSpan *application) const;
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
	void          checkPublicSurface (const Symbol &symbol, const Symbol *exportingHost = nullptr, const DeclarationInfo *specialization = nullptr) const;
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
	ParameterContract                  parameterContract (const TypeParameterDeclaration &parameter) const;
	bool                               acceptsBound (const ResolvedType &required, const ResolvedType &actual) const;
	bool                               sameSignature (const Candidate &left, const Candidate &right, bool includeResults = false) const;
	void                               checkBodyOf (const Symbol &symbol);
	std::set<const Symbol *>           s_checking, s_checked;
	std::map<InstanceKey, ResultTypes> m_widthResults;
	std::set<InstanceKey>              s_inferringWidths;
	std::map<std::pair<const Symbol *, unsigned>, GenericInstance>       m_widthTemplates;
	std::vector<std::pair<const TypeReference *, const Symbol *>>        v_widthAnnotations;
	mutable bool                                                         b_hasTypeApplications = false;
	void                                                                 checkWidthAnnotations () const;
	std::unordered_map<const TypeParameterDeclaration *, const Symbol *> m_genericOwners;
	using Guarantees = std::unordered_map<const TypeParameterDeclaration *, Candidates>;
	Guarantees m_guarantees;
};

enum class PlaceFact { Presence, CurrentShape, RetainedShape, RepresentedType };

// An integer literal has at least one magnitude word. An empty index matches any element on a write.
using PlaceSegment = std::variant<const Symbol *, IntegerValue>;

struct StoragePlace {
	const Token              *root;
	std::vector<PlaceSegment> path                                    = {};
	PlaceFact                 fact                                    = PlaceFact::Presence;
	std::strong_ordering      operator<=>(const StoragePlace &) const = default;
	[[nodiscard]] bool        stable () const;
	[[nodiscard]] bool        indexed () const;
	[[nodiscard]] bool        covers (const StoragePlace &proof, bool descendantsOnly = false) const;
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
	// A deferred read inside a loop also accounts for writes in unreachable source in the deferred body.
	std::shared_ptr<const WriteEffects> presenceWrites = {};
};

class BodyChecker {
    public:
	BodyChecker (Analyzer &analyzer, const Symbol &context, BodyFacts &facts, const Substitutions &arguments, DeclarationInfo &signature,
	             const ResolvedType *attachment = nullptr, unsigned operatorWidth = 0);
	void check ();
	void inferOperatorResult ();
	void resolveDeclarationNames ();

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
		const Expression      *node{};
		ExpressionInfo         value;
		const CapturedOperand *previous{};
	};

	const CapturedOperand *captured      = nullptr;
	const Expression      *validateAfter = nullptr;
	IndexedAccess          indexAccess (const ExpressionInfo &operation, const TokenSpan &span);
	bool writeIndex (const Statement &statement, const ExpressionPtr &target, const ExpressionInfo &array, const Token &op, const ExpressionPtr &rhs,
	                 const ExpressionPtr &compound);
	[[nodiscard]] ResolvedType    shapeOf (const Expression &value, const ResolvedType &fallback, std::size_t slot = 0) const;
	void                          receiveShape (const Token &binding, const ResolvedType &declared, const ResolvedType &shape, const TokenSpan &use);
	ExpressionInfo                mutateArray (const MemberCallExpression &call, const ExpressionInfo &receiver, ArrayMutation operation);
	std::optional<ExpressionInfo> builtinCall (const CallSite &call, const TypeArgumentList *typeArguments, const ExpressionInfo *receiver = nullptr);
	void              checkBuiltinArguments (const CallSite &call, const TypeArgumentList *typeArguments, std::size_t minimum, std::size_t maximum);
	void              checkArgumentNames (const CallSite &call, std::size_t start);
	[[noreturn]] void checkBuiltinTypeArguments (const CallSite &call, const TypeArgumentList &arguments);
	void              checkTypeArguments (const TypeArgumentList &arguments) const;
	ResolvedType      nonReturningNumericReceiver (const CallSite &call);
	[[noreturn]] void unknownMember (const MemberCallExpression &call, std::string_view receiver);
	ExpressionInfo    memberCall (const MemberCallExpression &call);
	void              rowShape (const Expression &target, const Expression &value, const ResolvedType &incoming);
	void              checkValidatorReceiver (const Expression *receiver, const TokenSpan &use) const;
	void              enumValue (const EnumFieldDeclaration &field, const Expression &value, const Token *caseName);
	std::optional<ProviderSelection>            textConversion (const ResolvedType &type) const;
	bool                                        formatterMayRun (const Expression &receiver, const ResolvedType &type, bool declared);
	void                                        applyTextConversion (const Expression &receiver, ProviderSelection &format);
	bool                                        bindText (const Expression &operand, const ResolvedType &type);
	ExpressionInfo                              providerAccess (const ProviderAccessExpression &node);
	const GuardedPayload                       *gp_payload        = nullptr;
	ReceiverPhase                               rp_receiverPhase  = ReceiverPhase::Ready;
	const ParameterDeclaration                 *pd_default        = nullptr;
	const Token                                *t_deferredBinding = nullptr;
	Analyzer                                   &analyzer;
	const Symbol                               &context;
	BodyFacts                                  &bf_facts;
	ExpressionFacts                            *ef_probeConstants = nullptr;
	const Substitutions                        &m_arguments;
	DeclarationInfo                            &di_signature;
	const Symbol                               *owner;
	const ResolvedType                         *rt_attachment;
	unsigned                                    u_operatorWidth;
	ResultTypes                                 returns;
	std::vector<std::map<std::string, Binding>> scopes{ 1 };

	struct FlowFact {
		ResolvedType type;
		ResultTypes  nominalTests                       = {};
		bool         operator==(const FlowFact &) const = default;
	};

	using Narrowing = std::map<StoragePlace, FlowFact>;
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
		bool         overloaded;
	};

	using OperatorProbes           = std::unordered_map<const Expression *, OperandProbe>;
	OperatorProbes *operatorProbes = nullptr;

	struct TestPath {
		Narrowing facts;
		bool      reachable = false;
	};

	struct TestPaths {
		TestPath yes, no;
	};

	struct TypedPlace {
		StoragePlace place;
		ResolvedType type;
	};

	const LoopTarget             *lt_loop    = nullptr;
	WriteEffects                 *we_capture = nullptr;
	std::shared_ptr<WriteEffects> we_deferred;
	const Expression             *e_test = nullptr;
	TestPaths                     tp_test;
	static void                   intersect (Narrowing &facts, const Narrowing &other);
	static void                   join (std::optional<Narrowing> &facts, const Narrowing &other);
	static void                   mergeExits (Exits &target, const Exits &source);
	void                          invalidate (const StoragePlace &place, bool descendantsOnly = false);
	[[nodiscard]] bool            sharedPlace (const StoragePlace &place) const;
	void                          forgetShared (bool indexedOnly = false);
	void                          mutatingReceiver (const Expression &receiver, const ResolvedType &type);
	void                          invalidateShared ();
	void                          forgetMutableMetadata (Narrowing &facts) const;
	void                          applyWrites (const Binding &binding);

	struct SourceFlow {
		// The write walk visits all source. These paths describe which statements can reach a post-test loop's next iteration.
		bool                     fallsThrough = true, breaks = false, continues = false;
		std::optional<bool>      truth = {};
		[[nodiscard]] SourceFlow then (SourceFlow next) const;
		[[nodiscard]] SourceFlow either (const SourceFlow &other) const;
	};

	SourceFlow                              collectWrites (const Expression &node, WriteEffects &writes);
	SourceFlow                              collectWrites (const std::vector<StatementPtr> &statements, WriteEffects &writes);
	SourceFlow                              collectRepeatedWrites (const LoopStatement &node, WriteEffects &writes, bool *canRepeat = nullptr);
	std::shared_ptr<const WriteEffects>     deferredPresenceWrites (const FunctionBlockExpression &node);
	Narrowing                               loopPresence (const LoopStatement &node, bool &canRepeat);
	bool                                    containsNullability (const ResolvedType &type) const;
	void                                    checkInferredBinding (const ResolvedType &type, const TokenSpan &span) const;
	void                                    setPresence (const StoragePlace &place, const ResolvedType &declared, const std::optional<ResolvedType> &value);
	void                                    assumePresent (const Expression &source);
	static void                             forgetPresence (Narrowing &facts, const StoragePlace &place);
	[[nodiscard]] std::optional<TypedPlace> locate (const Expression &expression) const;
	[[nodiscard]] ResolvedType              readType (const StoragePlace &place, const ResolvedType &declared) const;

	struct ReceiverProof {
		StoragePlace place;
		TokenSpan    span;
	};

	using ReceiverProofs = std::vector<ReceiverProof>;
	[[nodiscard]] ReceiverProofs receiverProofs (const Expression *receiver) const;
	void                         requireReceiverProofs (const ReceiverProofs &proofs) const;
	void                         narrowRead (ExpressionInfo &value, const StoragePlace &place) const;
	TestPaths                    test (const Expression &node);
	TestPaths                    splitTest (const Expression &node, const ExpressionInfo &result);
	bool                         checkStatement (const Statement &node);
	void                         finishStatement (const Statement &node, bool fallsThrough, const Exits &incoming);
	void                         typeBody ();
	void                         resolveCallName (const CallExpression &call);
	void                         resolveNames (const Expression &node);
	void                         resolveNames (const FunctionBody &body);
	void                         assignmentTarget (const Token &target);
	std::optional<Candidate>     initialStateTest (const TypeCheckExpression &test, const Token *state);
	void                         resolveNames (const std::vector<StatementPtr> &statements);
	void                         resolveNames (const Statement &node);
	void                         leaveScope ();

	struct ArgumentProbe {
		std::optional<ExpressionInfo> result;
		FlowState                     before, after;
	};

	FlowState fs_flow;
	bool      record              = true;
	bool      b_inferenceDefaults = false;

	[[nodiscard]] const Binding *local (const std::string &name) const;
	[[nodiscard]] const Binding *assignmentBinding (const Token &target) const;
	Binding                     &bind (const Token &name, std::optional<ResolvedType> type, bool mutableBinding, bool deferred = false);
	void                         bindDeferred (const Token &name, std::optional<ResolvedType> type, bool mutableBinding, DeferredBinding result,
	                                           const std::shared_ptr<const WriteEffects> &presenceWrites);
	void                         checkBindingName (const Token &name) const;
	void                         checkDefaultName (const Token &name) const;
	bool                         readOnlyDefaultRoot (const Expression &node) const;
	[[nodiscard]] ResolvedType   selfType () const;
	ResolvedType                 bodyType (const TypeReference &reference, bool allowOpen = false) const;
	const BodyFacts             &openFacts () const;
	std::optional<ResolvedType>  receiverClassBound (const Expression &receiver, const ResolvedType &actual) const;
	[[nodiscard]] bool           hasInstance () const;
	void                         checkFieldRead (const Symbol &field, const Token &use) const;
	void                         checkFieldWrite (const ExpressionInfo &target, const TokenSpan &span) const;
	ResolvedType                 recordSelf (const Expression &node);
	ExpressionInfo               call (const CallExpression &node);
	std::vector<Candidate>       callCandidates (const CallExpression &node);
	static bool                  isSelfReference (const Expression &node);
	ExpressionInfo               create (const ClassCreationExpression &node);
	ExpressionInfo               cast (const CastExpression &node);
	bool                         loop (const LoopStatement &node);
	ExpressionInfo expression (const Expression &node, const ResultTypes *expected = nullptr, bool discard = false, std::optional<FloatContext> floats = {},
	                           const ResolvedType *broadcast = nullptr);
	BodyChecker   &onPath (bool reachable, std::optional<BodyChecker> &unreachable);
	bool           checkArguments (const std::vector<CallArgument> &arguments, const ResultTypes &parameters, const std::vector<ArgumentProbe> &probes,
	                               bool contextual);
	ExpressionInfo infer (const Expression &node, const ResultTypes *expected, bool discard, FloatContext floats);
	std::optional<ResolvedType>   scalar (const Expression &node, const ResolvedType *expected = nullptr, std::optional<FloatContext> floats = {});
	ExpressionInfo                scalarInfo (const Expression &node, const ResolvedType *expected = nullptr, std::optional<FloatContext> floats = {});
	void                          numericPeers (const Expression &node, NumericPeers &peers, bool contextualFloat);
	std::optional<ResolvedType>   elementHint (const Expression &node, FloatContext floats);
	BodyChecker                   comparisonProbe () const;
	std::optional<ResolvedType>   comparisonHint (const Expression &node, bool defaultLiterals = false) const;
	ExpressionInfo                arrayLiteralComparison (const BinaryExpression &node, const ArrayLiteral *left, const ArrayLiteral *right);
	std::optional<ExpressionInfo> contextualRangeComparison (const BinaryExpression &node);
	ExpressionInfo                rangeValue (const BinaryExpression &node, const ResolvedType *endpoint, Code code);
	std::optional<ResolvedType>   rangeEndpointHint (const Expression &start, const Expression &end);
	void require (const ResultTypes &actual, const ResultTypes &expected, const TokenSpan &span, Code code = Code::TYPE_MISMATCH) const;
	void requireStoredValue (const ResolvedType &expected, const ResolvedType &actual, const TokenSpan &span, Code code) const;
	bool expect (const Expression &node, const ResolvedType &type);
	bool checkReturn (const Expression &node);
	void checkReturnContext (const ReturnStatement &statement) const;
	enum class NameUse { Value, Assignment, Lexical };
	ExpressionInfo                name (const Token &token, NameUse use = NameUse::Value);
	bool                          hasValueName (const Token &name) const;
	std::optional<ResolvedType>   receiverType (const Expression &node, bool forConstruction = false);
	std::optional<Receiver>       receiver (const Expression &node);
	ExpressionInfo                field (const FieldAccessExpression &node);
	std::optional<Candidate>      stateTarget (const ResolvedType &receiver, const Token &name, const Expression &operand);
	bool                          statePayload (const ExpressionPtr &payload, const ResolvedType &required, const TokenSpan &span, const Token &state);
	ExpressionInfo                transition (const Candidate &state, const ExpressionPtr &payload, const TokenSpan &span);
	ExpressionInfo                typeCheck (const TypeCheckExpression &node);
	ExpressionInfo                binary (const BinaryExpression &node, const ResolvedType *expected, FloatContext floats, bool allowContexts = true);
	ExpressionInfo                builtinBinary (const BinaryExpression &node, const ResolvedType *expected, FloatContext floats);
	ExpressionInfo                elvis (const ElvisExpression &node, const ResultTypes *expected, FloatContext floats);
	std::optional<ExpressionInfo> binaryOperator (const BinaryExpression &node, const ResolvedType *expected, bool allowContexts, FloatContext floats);
	std::optional<ExpressionInfo> applyBinaryOperator (const BinaryExpression &node, const ResolvedType &rt_left, const ResolvedType &rt_right,
	                                                   const ResolvedType *expected, bool allowContexts, FloatContext floats);
	ExpressionInfo                equality (const BinaryExpression &node, const ResolvedType &left, const ResolvedType &right);
	std::optional<Candidate>      selectOperator (const Token &op, const TokenSpan &span, const ResolvedType &left, const ResolvedType &right,
	                                              bool &rightReceiver, const BinaryExpression *operands = nullptr, const Symbol **unmatched = nullptr) const;
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
	ExpressionInfo condition (const IfExpression &node, const ResultTypes *expected, bool discard, FloatContext floats, const ResolvedType *broadcast);
	ExpressionInfo block (const std::vector<StatementPtr> &statements, const ResultTypes *expected = nullptr, bool value = false,
	                      std::optional<FloatContext> floats = {}, const ResolvedType *broadcast = nullptr);
	bool           statement (const Statement &node);
	void           assign (const Token &name, const std::optional<ResolvedType> &value, const Expression *source = nullptr, std::size_t slot = 0,
	                       const std::optional<ResolvedType> &represented = {});
	bool           receive (const std::vector<ReceivingTarget> &targets, const Expression &producer, const std::vector<TypeReference> *annotations,
	                        bool mutableBinding = false);
	void           writable (const Expression &target);
	std::optional<ExpressionInfo> updateOperator (const Statement &statement, const ResolvedType &receiver, const Token &op, const ExpressionPtr &rhs);
	bool writeStorage (const Statement &statement, const ExpressionPtr &target, const ExpressionInfo &selected, const Token &op, const ExpressionPtr &rhs,
	                   const ExpressionPtr &compound);
	bool writeMember (const Statement &statement, const ExpressionPtr &target, const Token &op, const ExpressionPtr &rhs, const ExpressionPtr &compound);
	void checkBody (const FunctionBody &body);
	void remember (const Token &use, const Token &declaration) const;
};

}
