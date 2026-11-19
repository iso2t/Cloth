// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <format>

#include <ast/decl/class_declaration.hpp>
#include <ast/stmt/statements.hpp>

#include "checked_program_test.hpp"

namespace cloth::test {
namespace {

const ExpressionInfo &callIn (const CheckedProgram &program, std::string_view name) {
	return program.expressions.at(&bodyExpression(program.units.front()->get_globals().at(std::string(name)).front()));
}

TEST (SemanticGenerics, InternsCallsAndKeepsEachBodyAndItsBindings) {
	const CheckedProgram     cp_program   = checkSource(R"(fn<T> identity<T>(T item) = item
fn<int> first() = identity<int>(1)
fn<int> again() = identity<int<32>>(2)
fn<bool> another() = identity(true)
)");
	const Symbol            &sym_identity = cp_program.units.front()->get_globals().at("identity").front();
	const CallableSignature &cs_identity  = *std::get<const CallableSignature *>(sym_identity.declaration);
	const Expression        &e_read       = bodyExpression(sym_identity);
	const Token             &t_item       = cs_identity.parameters.front().name;
	const GenericInstance   *gi_int = callIn(cp_program, "first").instance, *gi_bool = callIn(cp_program, "another").instance;
	ASSERT_NE(gi_int, nullptr);
	ASSERT_NE(gi_bool, nullptr);
	EXPECT_EQ(gi_int, callIn(cp_program, "again").instance);
	EXPECT_EQ(gi_int, callIn(cp_program, "first").contractInstance);
	EXPECT_NE(gi_int, gi_bool);
	EXPECT_EQ(cp_program.instances.size(), 2u);
	EXPECT_EQ(gi_int, &cp_program.instances.at({ &sym_identity, std::nullopt, { ResolvedType(kInteger) } }));
	EXPECT_EQ(gi_int->body.expressions.at(&e_read).types, ResultTypes{ ResolvedType(kInteger) });
	EXPECT_EQ(gi_bool->body.expressions.at(&e_read).types, ResultTypes{ ResolvedType(kBoolean) });
	EXPECT_EQ(gi_int->body.localTypes.at(&t_item), ResolvedType(kInteger));
	EXPECT_EQ(gi_bool->body.localTypes.at(&t_item), ResolvedType(kBoolean));
	EXPECT_EQ(gi_bool->body.references.at(&dynamic_cast<const NameExpression &>(e_read).name), &t_item);
	EXPECT_EQ(cp_program.expressions.at(&e_read).types.front().parameter, cp_program.declarations.at(&sym_identity).parameters.front().parameter);
	EXPECT_NE(cp_program.localTypes.at(&t_item).parameter, nullptr);
}

TEST (SemanticGenerics, KeepsOrderedArgumentsAndOnlyInstantiatesWinningOverloads) {
	const CheckedProgram cp_program = checkSource(R"(fn<A> choose<A, B>(A first, B second) = first
fn<int> pick(int item) = item
fn<T> pick<T>(T item) = item
fn<int> forward() = choose(1, true)
fn<bool> swapped() = choose(true, 1)
fn<int> ordinary() = pick(1)
)");
	const Symbol        &sym_choose = cp_program.units.front()->get_globals().at("choose").front();
	EXPECT_EQ(callIn(cp_program, "forward").instance,
	          &cp_program.instances.at({ &sym_choose, std::nullopt, { ResolvedType(kInteger), ResolvedType(kBoolean) } }));
	EXPECT_EQ(callIn(cp_program, "swapped").instance,
	          &cp_program.instances.at({ &sym_choose, std::nullopt, { ResolvedType(kBoolean), ResolvedType(kInteger) } }));
	EXPECT_EQ(callIn(cp_program, "ordinary").instance, nullptr);
	EXPECT_EQ(cp_program.instances.size(), 2u);
}

TEST (SemanticGenerics, SpecializesFieldsMetadataAndCapturedLocalsOnEachReceiver) {
	const CheckedProgram cp_program = checkSource(R"(typedef<struct> Box<T> {
    T? saved;
    pub mt<typeinfo> metadata() = T::type
    pub mt<T?> copy(T item) { def<T> held = { return item }; return held }
}
fn<typeinfo> first(Box<int> box) = box.metadata()
fn<typeinfo> second(Box<int?> box) = box.metadata()
fn<int?> value(Box<int> box) = box.copy(1)
fn<int?> optional(Box<int?> box) = box.copy(null)
)");
	const Symbol        &sym_box    = cp_program.units.front()->get_globals().at("Box").front();
	const Symbol        &sym_saved  = sym_box.members.at("saved").front();
	const Expression    &e_metadata = bodyExpression(sym_box.members.at("metadata").front());
	ResolvedType         rt_nullable(kInteger);
	rt_nullable.suffixes.emplace_back(to_string(oQuestion));
	const GenericInstance &gi_plain    = cp_program.instances.at({ &sym_box, std::nullopt, { ResolvedType(kInteger) } });
	const GenericInstance &gi_optional = cp_program.instances.at({ &sym_box, std::nullopt, { rt_nullable } });
	EXPECT_EQ(gi_plain.fields.at(&sym_saved), rt_nullable);
	EXPECT_EQ(gi_optional.fields.at(&sym_saved), rt_nullable);
	const GenericInstance *gi_first = callIn(cp_program, "first").instance, *gi_second = callIn(cp_program, "second").instance;
	ASSERT_NE(gi_first, nullptr);
	ASSERT_NE(gi_second, nullptr);
	EXPECT_NE(gi_first, gi_second);
	EXPECT_EQ(gi_first->body.expressions.at(&e_metadata).representedType, ResolvedType(kInteger));
	EXPECT_EQ(gi_second->body.expressions.at(&e_metadata).representedType, rt_nullable);
	const CallableSignature &cs_copy   = *std::get<const CallableSignature *>(sym_box.members.at("copy").front().declaration);
	const ReturnStatement   &rs_return = dynamic_cast<const ReturnStatement &>(*std::get<BlockBody>(cs_copy.executableBody()->value).statements.back());
	const GenericInstance   *gi_value = callIn(cp_program, "value").instance, *gi_maybe = callIn(cp_program, "optional").instance;
	ASSERT_NE(gi_value, nullptr);
	ASSERT_NE(gi_maybe, nullptr);
	EXPECT_EQ(gi_value->body.expressions.at(rs_return.value.get()).types, ResultTypes{ ResolvedType(kInteger) });
	EXPECT_EQ(gi_maybe->body.expressions.at(rs_return.value.get()).types, ResultTypes{ rt_nullable });
}

TEST (SemanticGenerics, RetainsBothTheVirtualContractAndTheOverridingInstance) {
	const CheckedProgram  cp_program = checkSource(R"(typedef<class> Base<T> {
    virtual: pub mt<U> read<U>(U value) = value
}
typedef<class> Child<T> : Base<T> {
    override: pub mt<V> read<V>(V renamed) = renamed
}
fn<bool> use(Child<int> child) = child.read<bool>(value=true)
)");
	const ExpressionInfo &ei_call    = callIn(cp_program, "use");
	ASSERT_NE(ei_call.contractInstance, nullptr);
	ASSERT_NE(ei_call.instance, nullptr);
	EXPECT_NE(ei_call.contractInstance, ei_call.instance);
	const SymbolScope &ss_globals = cp_program.units.front()->get_globals();
	const Symbol      &sym_base = ss_globals.at("Base").front(), &sym_child = ss_globals.at("Child").front();
	EXPECT_EQ(ei_call.argumentContract, &sym_base.members.at("read").front());
	EXPECT_EQ(ei_call.symbol, &sym_child.members.at("read").front());
	EXPECT_EQ(ei_call.contractInstance->body.expressions.at(&bodyExpression(*ei_call.argumentContract)).types, ResultTypes{ ResolvedType(kBoolean) });
	EXPECT_EQ(ei_call.instance->body.expressions.at(&bodyExpression(*ei_call.symbol)).types, ResultTypes{ ResolvedType(kBoolean) });
}

TEST (SemanticGenerics, InfersConstructionIndependentlyOfTheEnclosingReceiver) {
	const CheckedProgram   cp_program = checkSource(R"(typedef<class> Box<T>(T value) {
    pub mt<Box<bool>> make() = Box::new(true)
}
fn<Box<bool>> use(Box<int> box) = box.make()
)");
	const Symbol          &sym_box    = cp_program.units.front()->get_globals().at("Box").front();
	const Expression      &e_make     = bodyExpression(sym_box.members.at("make").front());
	const GenericInstance *gi_method  = callIn(cp_program, "use").instance;
	ASSERT_NE(gi_method, nullptr);
	const ExpressionInfo &ei_new = gi_method->body.expressions.at(&e_make);
	ASSERT_NE(ei_new.instance, nullptr);
	ASSERT_EQ(ei_new.types.size(), 1u);
	EXPECT_EQ(ei_new.types.front().arguments(), ResultTypes{ ResolvedType(kBoolean) });
	EXPECT_EQ(ei_new.instance->signature.parameters, ResultTypes{ ResolvedType(kBoolean) });
}

TEST (SemanticGenerics, PreservesTheOverloadProvenByANominalBound) {
	const CheckedProgram   cp_program = checkSource(R"(typedef<class> Base { pub mt<int> pick(Base x) = 1 }
typedef<class> Child : Base { pub mt<bool> pick(Child x) = true }
fn<int> read<T: Base>(T value) = value.pick(value)
fn<int> use(Child item) = read(item)
)");
	const SymbolScope     &ss_globals = cp_program.units.front()->get_globals();
	const Expression      &e_pick     = bodyExpression(ss_globals.at("read").front());
	const GenericInstance *gi_read    = callIn(cp_program, "use").instance;
	ASSERT_NE(gi_read, nullptr);
	EXPECT_EQ(gi_read->body.expressions.at(&e_pick).symbol, &ss_globals.at("Base").front().members.at("pick").front());
	EXPECT_EQ(gi_read->body.expressions.at(&e_pick).argumentContract, cp_program.expressions.at(&e_pick).argumentContract);
	expectError(R"(fn<int> pick(int x) = x
fn<bool> pick<T>(T x) = true
fn<bool> read<T>(T value) = pick(value)
fn<bool> use() = read(1))",
	            cde::DiagnosticCode::RETURN_TYPE);
}

TEST (SemanticGenerics, BindsGuaranteedConstructionToItsDeclaredLabelsAndDefaults) {
	const CheckedProgram   cp_program = checkSource(R"(typedef<class> Item {
    pub Item(int renamed) : () {}
    pub Item(bool flag) : () {}
}
fn<T> build<T: new(opt<int> count ?: 4)>() = T::new()
fn<T> named<T: new(int count)>() = T::new(count=3)
fn<Item> byDefault() = build<Item>()
fn<Item> byName() = named<Item>()
)");
	const SymbolScope     &ss_globals = cp_program.units.front()->get_globals();
	const Expression      &e_build    = bodyExpression(ss_globals.at("build").front());
	const GenericInstance *gi_build = callIn(cp_program, "byDefault").instance, *gi_named = callIn(cp_program, "byName").instance;
	ASSERT_NE(gi_build, nullptr);
	ASSERT_NE(gi_named, nullptr);
	const ExpressionInfo &ei_ctor = gi_build->body.expressions.at(&e_build);
	ASSERT_NE(ei_ctor.argumentContract, nullptr);
	ASSERT_NE(ei_ctor.symbol, nullptr);
	EXPECT_EQ(ei_ctor.argumentContract, cp_program.expressions.at(&e_build).argumentContract);
	EXPECT_NE(ei_ctor.symbol, ei_ctor.argumentContract);
	EXPECT_EQ(ei_ctor.contractInstance, gi_build);
	EXPECT_EQ(ei_ctor.omittedParameters, (std::vector<std::size_t>{ 0 }));
	const CallableSignature &cs_contract = *std::get<const CallableSignature *>(ei_ctor.argumentContract->declaration);
	EXPECT_EQ(gi_build->body.expressions.at(cs_contract.parameters.front().defaultValue.get()).types, ResultTypes{ ResolvedType(kInteger) });
	const CallableSignature &cs_ctor = *std::get<const CallableSignature *>(ei_ctor.symbol->declaration);
	EXPECT_EQ(cs_ctor.parameters.front().name.get_lexeme(), "renamed");
	const ExpressionInfo &ei_named = gi_named->body.expressions.at(&bodyExpression(ss_globals.at("named").front()));
	EXPECT_EQ(ei_named.symbol, ei_ctor.symbol);
	EXPECT_EQ(ei_named.parameterForArgument, (std::vector<std::size_t>{ 0 }));
	EXPECT_NO_THROW(checkSource(R"(typedef<class> Hidden {
    Hidden(int n) : () {}
    pub mt<Hidden> build() = outer<Hidden>()
}
fn<T> outer<T: new(int value)>() = inner<T>()
fn<T> inner<T: new(int count)>() = T::new(count=1))"));
	expectError("typedef<class> Hidden(int n)\nfn<T> make<T: new(int count)>() = T::new(1)\nfn<Hidden> use() = make<Hidden>()",
	            cde::DiagnosticCode::NO_MATCHING_CALL);
}

TEST (SemanticGenerics, ChecksUnusedDeclarationsAndReusesRecursiveInstances) {
	const CheckedProgram   cp_program = checkSource(R"(fn<T> left<T>(T x) = right(x)
fn<T> right<T>(T x) = left(x)
fn<int> start() = left(1)
)");
	const SymbolScope     &ss_globals = cp_program.units.front()->get_globals();
	const GenericInstance *gi_left    = callIn(cp_program, "start").instance;
	ASSERT_NE(gi_left, nullptr);
	const GenericInstance *gi_right = gi_left->body.expressions.at(&bodyExpression(ss_globals.at("left").front())).instance;
	ASSERT_NE(gi_right, nullptr);
	EXPECT_EQ(gi_right->body.expressions.at(&bodyExpression(ss_globals.at("right").front())).instance, gi_left);
	EXPECT_EQ(cp_program.instances.size(), 2u);
	using Code = cde::DiagnosticCode;
	expectError("fn<T> unused<T>(T x) = missing", Code::UNKNOWN_NAME);
	expectError("fn<bool> unused<T>(T? x, T? y) = x == y", Code::INCOMPARABLE_TYPES);
	expectError("fn<T> unused<T>(T x) = x + 1", Code::BINARY_OPERANDS);
	expectError("fn<T> unused<T>() = T::new()", Code::INVALID_CLASS_LIFECYCLE);
	expectError("fn<bool> eq<T>(T[] a, T[] b) = a == b", Code::INCOMPARABLE_TYPES);
	expectError("fn<usize> bytes<T>(T? item) = item::size", Code::UNKNOWN_PROVIDER);
	expectError("typedef<class> Base {}\nfn unused<T: Base = int>() {}", Code::UNSATISFIED_GENERIC_BOUND);
	expectError("fn unused<T: int>() {}", Code::INVALID_GENERIC_BOUND);
	EXPECT_NO_THROW(checkSource("fn<bool> eq<T: new()>(T a, T b) = a == b\nfn drop<T: new()>(T item) = item::delete"));
	EXPECT_NO_THROW(checkSource("typedef<class> Base { pub mt<int> count() = 1 }\nfn<int> read<T: Base>(T item) = item.count()"));
}

TEST (SemanticGenerics, DistinguishesInlineStorageFromReferenceCycles) {
	using Code = cde::DiagnosticCode;
	expectError("typedef<struct> Loop { Loop next }", Code::RECURSIVE_STRUCT);
	expectError("typedef<struct> Loop<T> { Loop<T>?[]? next }", Code::RECURSIVE_STRUCT);
	expectError("typedef<struct> A<T> { B<T> next }\ntypedef<struct> B<T> { A<T>? preceding }", Code::RECURSIVE_STRUCT);
	expectError("typedef<struct> Cell<T> { T value }\ntypedef<struct> Loop { Cell<Loop> link }", Code::RECURSIVE_STRUCT);
	EXPECT_NO_THROW(checkSource("typedef<struct> Cell<T> { T value }\nfn finite(Cell<Cell<int>> item) {}"));
	EXPECT_NO_THROW(checkSource(R"(typedef<struct> Slot<T> { Holder<T> owner }
typedef<class> Holder<T>(def<Slot<T>?> slot ?: null)
fn use(Holder<int> item) {})"));
}

TEST (SemanticGenerics, StopsGrowingDependenciesEvenWithoutAClosedCall) {
	using Code = cde::DiagnosticCode;
	expectError("fn grow<T>() = grow<T[]>()", Code::GENERIC_TEMPLATE_DEPENDENCY_LIMIT);
	expectError("typedef<class> Pair<A,B> {}\nfn grow<T>() = grow<Pair<T,T>>()", Code::GENERIC_TEMPLATE_DEPENDENCY_LIMIT);
	expectError("typedef<class> Expands<T>(def<Expands<T[]>?> next ?: null)", Code::GENERIC_TEMPLATE_DEPENDENCY_LIMIT);
	expectError("typedef<class> Expands<T: Expands<Expands<T>>> {}", Code::GENERIC_TEMPLATE_DEPENDENCY_LIMIT);
}

TEST (SemanticGenerics, ChecksForMemberSignaturesThatCoincideAfterSubstitution) {
	const std::string s_methods = R"(typedef<class> Box<T> {
    mt put(T item) {}
    mt put(int item) {}
}
)";
	EXPECT_NO_THROW(checkSource(s_methods + "fn use(Box<bool> box) {}"));
	expectError(s_methods + "fn use(Box<int> box) {}", cde::DiagnosticCode::DUPLICATE_METHOD);
	expectError(s_methods + "fn unused<T>(Box<int> box) {}", cde::DiagnosticCode::DUPLICATE_METHOD);
	expectError(R"(typedef<class> Box<T> {
    Box(T item) : () {}
    Box(int item) : () {}
}
fn use(Box<int> box) {})",
	            cde::DiagnosticCode::INVALID_CLASS_MEMBER);
}

TEST (SemanticGenerics, EnforcesTheClosedInstanceLimitAfterDuplicateLookup) {
	std::string s_source = "typedef<class> Box<T> {}\nfn main() {\n";
	for (unsigned u_width = 1; u_width <= 4096; ++u_width) s_source += std::format("Box<int<{}>>::type\n", u_width);
	const CheckedProgram cp_program = checkSource(s_source + "Box<int<1>>::type\n}");
	const Symbol        &sym_box    = cp_program.units.front()->get_globals().at("Box").front();
	EXPECT_EQ(std::ranges::count_if(cp_program.instances,
	                                [&] (const std::pair<const InstanceKey, GenericInstance> &entry) { return entry.first.declaration == &sym_box; }),
	          4096);
	expectError(s_source + "Box<int<4097>>::type\n}", cde::DiagnosticCode::GENERIC_INSTANCE_LIMIT);
}

TEST (SemanticGenerics, CountsNestingAfterSubstitution) {
	std::string s_argument = "int";
	for (unsigned u_layer = 0; u_layer < 255; ++u_layer) s_argument = "Box<" + s_argument + ">";
	const std::string s_types = "typedef<class> Box<T> {}\ntypedef<class> Uses<T>(def<";
	const std::string s_use   = "?> item ?: null)\nfn take(Uses<" + s_argument + "> value) {}";
	EXPECT_NO_THROW(checkSource(s_types + "Box<T>" + s_use));
	expectError(s_types + "Box<Box<T>>" + s_use, cde::DiagnosticCode::GENERIC_SUBSTITUTION_NESTING);
}

TEST (SemanticGenerics, SharesNestedArgumentsAndBoundsTheirDiagnosticSpelling) {
	std::string s_source = "typedef<class> Pair<A, B> {}\n";
	for (unsigned u_level = 0; u_level < 20; ++u_level) {
		s_source += std::format("fn<typeinfo> step{}<T>() = step{}<Pair<T, T>>()\n", u_level, u_level + 1);
	}
	const CheckedProgram   cp_program = checkSource(s_source + "fn<typeinfo> step20<T>() = T::type\nfn<typeinfo> use() = step0<int>()");
	const SymbolScope     &ss_globals = cp_program.units.front()->get_globals();
	const GenericInstance *gi_step    = callIn(cp_program, "use").instance;
	for (unsigned u_level = 0; u_level < 20; ++u_level) {
		ASSERT_NE(gi_step, nullptr);
		gi_step = gi_step->body.expressions.at(&bodyExpression(ss_globals.at(std::format("step{}", u_level)).front())).instance;
	}
	ASSERT_NE(gi_step, nullptr);
	const std::optional<ResolvedType> &opt_type = gi_step->body.expressions.at(&bodyExpression(ss_globals.at("step20").front())).representedType;
	ASSERT_TRUE(opt_type);
	EXPECT_EQ(opt_type->argumentDepth(), 20u);
	EXPECT_FALSE(opt_type->dependent());
	ASSERT_EQ(opt_type->arguments().size(), 2u);
	EXPECT_EQ(opt_type->arguments()[0].application, opt_type->arguments()[1].application);
	const std::string s_display = opt_type->spelling();
	EXPECT_LE(s_display.size(), 4096u);
	EXPECT_TRUE(s_display.ends_with("…"));
}

}
}
