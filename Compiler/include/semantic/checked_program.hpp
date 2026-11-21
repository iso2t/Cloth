// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include <ast/expr/expression.hpp>
#include <ast/type/type_parameters.hpp>
#include <semantic/program_kind.hpp>
#include <semantic/symbol_table.hpp>
#include <semantic/target_layout.hpp>
#include <token/numeric_literals.hpp>
#include <token/token.hpp>

namespace cloth {

struct BinaryExpression;
struct ClassParentClause;
struct FunctionBlockExpression;
struct GenericInstance;
struct ForStatement;
struct LoopTarget;
struct LoopJumpStatement;
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
	// A symbolic leaf of a nullable or array bound selects its payload from the eventual argument.
	std::size_t projectedSuffixes = 0;

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
	// A typed destination normalizes a present integer to zero or one. Nullable destinations preserve absence.
	[[nodiscard]] bool                             convertsIntegerToBool (const ResolvedType &source) const;
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

enum class LibraryIntrinsic {
	DecodeUtf8,
	FileOpen,
	FileRead,
	OwnershipAcquire,
	OwnershipRelease,
	OwnershipDelete,
	OwnershipCheckMutation,
	OwnershipSetCleanup
};

struct DeclarationInfo {
	std::size_t                     file;
	const Symbol                   *owner;
	ResultTypes                     parameters         = {};
	ResultTypes                     returns            = {};
	ResultTypes                     parents            = {};
	std::optional<ResolvedType>     value              = {};
	const Symbol                   *attachedState      = nullptr;
	bool                            inferResult        = false;
	const Symbol                   *primaryConstructor = nullptr;
	std::optional<LibraryIntrinsic> intrinsic          = {};
	// A secondary constructor can bind its primary call without changing an existing object's ownership.
	bool delegationPreservesOwnership = false;
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

struct ReceiverChecks {
	// A method captures its receiver before arguments. Their effects can require a second check at dispatch.
	bool          beforeArguments = false;
	bool          atAccess        = false;
	bool          activeState     = false;
	const Symbol *knownState      = nullptr;
};

struct ControlFlow {
	bool                         fallsThrough = true;
	bool                         canReturn    = false;
	std::set<const LoopTarget *> breaks, continues;
};

struct IntegerConstant {
	IntegerValue magnitude;
	bool         negative                                  = false;
	bool         operator==(const IntegerConstant &) const = default;
};

using NumericConstant = std::variant<std::monostate, IntegerConstant, double>;

enum class ProviderKind { Type, Format, StorageSize, ArraySize, StringSize, Minimum, Maximum, EnumName, EnumValues, Metadata };
enum class MetadataQuery { Element, Rank, Extent, Width, IsArray, HasExtent, IsUnsigned, IsNumeric };
enum class ArrayMutation { Append, Insert, RemoveAt, Clear, Reserve };

enum class BuiltinOperation {
	Print,
	Println,
	Input,
	Exit,
	Abs,
	Clamp,
	Signum,
	ToStringRadix,
	LeadingZeros,
	TrailingZeros,
	ToInt,
	ToUpperCase,
	ToLowerCase,
	IsWhitespace,
	IsLetter,
	IsDigit,
	IsLetterOrDigit,
	IsUpperCase,
	IsLowerCase,
	IsEmpty,
	Contains,
	StartsWith,
	EndsWith,
	CharAt,
	Substring,
	IndexOf,
	LastIndexOf,
	ToChars,
	Split,
	Replace,
	ReplaceFirst,
	RemovePrefix,
	RemoveSuffix,
	Trim,
	TrimStart,
	TrimEnd,
	IsBlank
};

// Operand expressions retain their source types. These are the types after the selected call's adapters.
struct BuiltinCall {
	BuiltinOperation            operation;
	std::optional<ResolvedType> receiver;
	ResultTypes                 parameters;
};

struct ProviderSelection {
	ProviderKind                 kind;
	ResolvedType                 receiver;
	bool                         typeReceiver;
	std::optional<MetadataQuery> metadata          = {};
	bool                         callsFormatter    = false;
	std::optional<MethodRef>     formatter         = {};
	const GenericInstance       *formatterInstance = nullptr;
};

struct IndexedAccess {
	const Symbol          *operation;
	const Symbol          *validator;
	const GenericInstance *validatorInstance;
	Substitutions          arguments;
};

enum class RangeOperation { Construct, Membership, Equal };

struct OperatorSelection {
	const Symbol          *symbol;
	const GenericInstance *instance;
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
	// The resolved right-hand type of a built-in type test, including symbolic generic parameters.
	std::optional<ResolvedType> testedType = std::nullopt;
	std::optional<CallDispatch> dispatch   = std::nullopt;

	ReceiverChecks receiverChecks = {};
	// A generic intersection can require liveness validation even for an identity comparison.
	bool checkedIdentity = false;
	// Structural array comparison may dispatch to an element's equality operator.
	bool callsElementEquality = false;
	// Array leaves use logical equality even when the outer comparison is inequality.
	std::optional<OperatorSelection> elementEquality    = {};
	bool                             callsClassEquality = false;
	bool                             negateEquality     = false;
	bool                             liftedEquality     = false;
	// Built-in boolean equality compares present payloads by truth value (zero or nonzero).
	bool                             booleanEquality = false;
	std::optional<OperatorSelection> leftContext = {}, rightContext = {};
	NumericConstant                  numericConstant    = {};
	bool                             invalidNumericCast = false;
	std::optional<ProviderSelection> provider           = {};
	std::optional<IndexedAccess>     indexed            = {};
	std::optional<RangeOperation>    range              = {};
	std::optional<ArrayMutation>     arrayMutation      = {};
	std::optional<BuiltinCall>       builtin            = {};
	// Known extents for each result, independent of the declared or overload-selected type.
	ResultTypes                               arrayShapes        = {};
	std::vector<std::optional<ResolvedType>>  representedResults = {};
	[[nodiscard]] std::optional<ResolvedType> representedAt (std::size_t slot) const;

	[[nodiscard]] std::optional<bool> integerTruth () const;
	[[nodiscard]] ResultKind resultKind () const;
	static ExpressionInfo    never ();
};

struct DefaultEffects {
	bool fallsThrough;
	bool mayCallOrMutate;
};

enum class IterationKind { Array, Range, Snapshot };

struct Iteration {
	IterationKind                 kind;
	ResolvedType                  element;
	std::optional<ExpressionInfo> snapshot  = {};
	const Symbol                 *slots     = nullptr;
	const Symbol                 *slotValue = nullptr;
};

using ExpressionFacts = std::unordered_map<const Expression *, ExpressionInfo>;

// Each read evaluates the whole block and selects this binding's result.
struct DeferredBinding {
	const FunctionBlockExpression *block;
	std::size_t                    resultIndex;
};

struct BodyFacts {
	std::unordered_map<const Expression *, ProviderSelection>           textConversions;
	std::unordered_map<const ForStatement *, Iteration>                 iterations;
	std::unordered_map<const Token *, ResolvedType>                     arrayConstraints;
	std::unordered_map<const Symbol *, std::vector<const Expression *>> enumPayloads;
	std::unordered_map<const ParameterDeclaration *, DefaultEffects>    defaultEffects;
	std::unordered_map<const Statement *, ControlFlow>                  controlFlow;
	std::unordered_map<const LoopJumpStatement *, const LoopTarget *>   jumpTargets;
	std::unordered_map<const Statement *, ReceiverChecks>               writeChecks;
	ExpressionFacts                                                     expressions;
	std::unordered_map<const ClassParentClause *, ExpressionInfo>       parentConstructors;
	std::unordered_map<const Token *, ResolvedType>                     localTypes;
	std::unordered_map<const Token *, DeferredBinding>                  deferredBindings;
	// An "is" target can resolve to a value after parsing. Keep that expression alive with its bindings.
	std::unordered_map<const TypeCheckExpression *, std::shared_ptr<const BinaryExpression>> valueTests;
	std::unordered_map<const Statement *, ExpressionInfo>                                    writes;
	std::unordered_map<const Token *, const Token *>                                         references;

	// Immediate uses of self retain receiver access through built-in casts.
	[[nodiscard]] bool isDirectReceiver (const Expression &expression) const;
};

struct InstanceKey {
	const Symbol               *declaration;
	std::optional<ResolvedType> receiver;
	ResultTypes                 arguments;
	unsigned                    operatorWidth = 0;

	std::strong_ordering operator<=>(const InstanceKey &other) const;
};

struct GenericInstance {
	DeclarationInfo                        signature;
	BodyFacts                              body;
	std::map<const Symbol *, ResolvedType> fields;
};

struct StructLayout {
	StorageLayout                           storage;
	std::map<const Symbol *, std::uint64_t> fieldOffsets;
};

struct CheckedProgram : BodyFacts {
	explicit CheckedProgram (SymbolTable entry, TargetLayout layout = {}, std::filesystem::path standardLibraryRoot = {},
	                         ProgramKind kind = ProgramKind::Library);
	const TargetLayout          layout;
	const std::filesystem::path standardLibraryRoot;
	const ProgramKind           kind;
	const Symbol               *entryPoint = nullptr;

	// Own the declarations and stable instances referenced by the body facts.
	std::vector<std::unique_ptr<const SymbolTable>>     units;
	std::deque<Symbol>                                  constructorRequirements;
	std::unordered_map<const Symbol *, DeclarationInfo> declarations;
	std::map<InstanceKey, GenericInstance>              instances;
	std::map<ResolvedType, TypeDispatch>                dispatchTables;
	std::map<ResolvedType, StructLayout>                structLayouts;
	[[nodiscard]] std::optional<StorageLayout>          storageLayout (const ResolvedType &type) const;
	// Contiguous element storage, excluding the array descriptor. One element gives the stride.
	[[nodiscard]] std::optional<StorageLayout> arrayDataLayout (const ResolvedType &array, std::uint64_t count) const;
	// A standalone state keeps its lexical scope. State selections in its body belong to each attaching class.
	std::map<ResolvedType, BodyFacts> attachmentBodies;
};
}
