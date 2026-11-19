// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <filesystem>
#include <format>
#include <functional>

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

TEST (SemanticGenerics, ResolvesClosedMethodTemplatesAtTheCall) {
	const std::string s_box = R"(typedef<class> Box<T> {
    pub mt<U> read<U>(T value, U other) = other
    pub mt<U> read<U>(int value, U other) = other
}
)";
	EXPECT_NO_THROW(checkSource(s_box + "fn use(Box<int> b) {}"));
	expectError(s_box + "fn use(Box<int> b) { b.read<bool>(1, true) }", cde::DiagnosticCode::AMBIGUOUS_CALL);
	expectError(s_box + "fn use(Box<int> b) { b.read(1, true) }", cde::DiagnosticCode::AMBIGUOUS_CALL);
	const CheckedProgram cp_program = checkSource(s_box + "fn<bool> use(Box<bool> b) = b.read(1, true)");
	const Symbol        &sym_method = cp_program.units.front()->get_globals().at("Box").front().members.at("read").back();
	EXPECT_EQ(callIn(cp_program, "use").argumentContract, &sym_method);
	expectError("typedef<class> Box<T> { mt read<U>(T v, U u) {}\n mt read<V>(T v, V u) {} }", cde::DiagnosticCode::DUPLICATE_METHOD);
	expectError("typedef<class> Box<T> { mt read(T v) {}\n mt read(int v) {} }\nfn use(Box<int> b) {}", cde::DiagnosticCode::DUPLICATE_METHOD);
	const std::string s_virtual = R"(typedef<class> Entity<A>(bool tag) {
    pub Entity() : (false) {}
    virtual: pub mt<T> echo<T>(A key, T value) = value
    virtual: pub mt<T> echo<T>(int key, T value) = value
}
)";
	EXPECT_NO_THROW(checkSource(s_virtual + "fn use(Entity<int> e) {}"));
	expectError(s_virtual + "typedef<class> Player : Entity<int> { override: pub mt<U> echo<U>(int key, U value) = value }",
	            cde::DiagnosticCode::INVALID_VIRTUAL_METHOD);
}

TEST (SemanticGenerics, ChecksInterfaceAndStateContractsWhenTheirOwnerCloses) {
	const std::string s_interface = R"(typedef<interface> View<T> {
    pub mt<U> read<U>(T value, U other)
    pub mt<U> read<U>(int value, U other)
}
)";
	EXPECT_NO_THROW(checkSource(s_interface + "fn<bool> read(View<bool> v) = v.read(1, true)"));
	expectError(s_interface + "fn use(View<int> v) {}", cde::DiagnosticCode::DUPLICATE_METHOD);
	expectError(s_interface + "fn use(View<int> v) { v.read(1, true) }", cde::DiagnosticCode::DUPLICATE_METHOD);
	const std::string s_state = R"(typedef<class> Host<T> {
    pub state<int> ::echo<U>(T left, U right)
    pub state<int> ::echo<U>(int head, U other)
    default: pub state<void> Ready { mt<int> echo<V>(T x, V y) = 1 }
}
)";
	EXPECT_NO_THROW(checkSource(s_state + "fn<int> use(Host<bool> h) = h::echo(true, true)"));
	expectError(s_state + "fn use(Host<int> h) {}", cde::DiagnosticCode::INVALID_STATE_CONTRACT);
	expectError(s_state + "fn use(Host<int> h) { h::echo(1, true) }", cde::DiagnosticCode::INVALID_STATE_CONTRACT);
}

TEST (SemanticGenerics, ValidatesTheOpenReceiverBoundAfterReselectingAnOverload) {
	const std::string s_types = R"(typedef<class> Base(pub def<bool> tag) {
    pub Base() : (false) {}
    pub mt<int> read() = 1
}
typedef<class> Player : Base {}
typedef<class> Other {
    pub mt<int> read() = 2
    pub operator ==(Object other) = false
}
typedef<class> Holder<T: Base>(def<T> value, def<Other> other) {
    mt<T> get(Object arg) = value
    mt<Other> get(Player arg) = other
)";
	const std::string s_use   = "\n}\nfn use(Holder<Player> holder) {}";
	for (const char *s_member : { "mt<int> test() = get(value).read()", "mt<bool> test() = get(value).tag", "mt<bool> test() = get(value) == value" })
		expectError(s_types + s_member + s_use, cde::DiagnosticCode::INVALID_CLASS_MEMBER);
	EXPECT_NO_THROW(checkSource(s_types + "mt<Object> test() = get(value)" + s_use));
}

TEST (SemanticGenerics, InfersExactTypesBeforeContextualizingLiterals) {
	const CheckedProgram   cp_program = checkSource(R"(fn<T> pick<T>(T a, T b) = a
fn<T> first<T>(T[] data, T sample) = sample
fn<int<8>> left(int<8> n) = pick(1, n)
fn<int<8>> right(int<8> n) = pick(n, 1)
fn<int<8>> named(int<8> n) = pick(b=n, a=1+2)
fn<int<8>> array(int<8> n) = first([1, 2], n)
fn<int[]> growable(int[] xs, int n) = pick([n], xs)
fn<float<64>> floating(float<64> n) = pick(1.5, n)
)");
	const GenericInstance *gi_pick    = callIn(cp_program, "left").instance;
	ASSERT_NE(gi_pick, nullptr);
	EXPECT_EQ(gi_pick, callIn(cp_program, "right").instance);
	EXPECT_EQ(gi_pick, callIn(cp_program, "named").instance);
	EXPECT_EQ(gi_pick->signature.returns.front().width, 8u);
	EXPECT_EQ(callIn(cp_program, "array").types, gi_pick->signature.returns);
	EXPECT_EQ(callIn(cp_program, "growable").types.front().spelling(), "int[]");
	EXPECT_EQ(callIn(cp_program, "floating").types.front().width, 64u);
}

TEST (SemanticGenerics, SettlesOrdinaryOverloadsBeforeInferringGenericCandidates) {
	using Code                      = cde::DiagnosticCode;
	const CheckedProgram cp_program = checkSource(R"(typedef<class> Base(int seed) { pub Base() : (0) {} }
typedef<class> Child : Base {}
fn<string> pick(Base value) = "base"
fn<T> pick<T>(T value) = value
fn<string> ordinary(Child item) = pick(item)
fn<int> generic() = pick(1)
typedef<class> Methods {
    pub mt<bool> test(int<8> value) = true
    pub mt<T> test<T>(T value) = value
}
fn<bool> method(Methods methods) = methods.test(1)
)");
	EXPECT_EQ(callIn(cp_program, "ordinary").instance, nullptr);
	EXPECT_NE(callIn(cp_program, "generic").instance, nullptr);
	EXPECT_EQ(callIn(cp_program, "method").instance, nullptr);
	expectError("fn pick(int<8> v) {}\nfn pick(int<64> v) {}\nfn pick<T>(T v) {}\nfn use() { pick(1) }", Code::AMBIGUOUS_CALL);
	expectError("fn pick<T>(T a, int b) {}\nfn pick<T,U>(T a, U b) {}\nfn use() { pick(true,1) }", Code::AMBIGUOUS_CALL);
}

TEST (SemanticGenerics, ReportsConflictingEvidenceAtBothArgumentsAndTheDeclaration) {
	using Code = cde::DiagnosticCode;
	try {
		checkSource("fn pick<T>(T a, T b) {}\nfn use(int a,bool b) { pick(\n a,\n b) }");
		FAIL() << "Expected incompatible type evidence";
	} catch (const cde::ClothDiagnostic &error) {
		EXPECT_EQ(error.get_code(), Code::GENERIC_INFERENCE_FAILED);
		const std::vector<cde::SourceLabel> &v_labels = error.get_source_labels();
		ASSERT_EQ(v_labels.size(), 3u);
		EXPECT_EQ(v_labels[0].span.startLine, 3u);
		EXPECT_EQ(v_labels[1].span.startLine, 4u);
		EXPECT_EQ(v_labels[2].span.startLine, 1u);
	}
	expectError("fn f<T>(T v) {}\nfn use() { f(null) }", Code::GENERIC_INFERENCE_FAILED);
	expectError("fn f<T>(T[] v) {}\nfn use() { f([]) }", Code::GENERIC_INFERENCE_FAILED);
	expectError("fn f<T>(T v) {}\nfn use() { f(2147483648) }", Code::NO_MATCHING_CALL);
	expectError("fn f<T>(T v) {}\nfn use() { f([2147483648]) }", Code::NO_MATCHING_CALL);
	expectError("fn f<T>(T a,T b) {}\nfn use(int<8> n) { f(128,n) }", Code::NO_MATCHING_CALL);
}

TEST (SemanticGenerics, UsesNominalViewsAndOnlyInfersWrittenPlaceholders) {
	const CheckedProgram cp_program = checkSource(R"(typedef<interface> View<T> {}
typedef<struct> Slot<T> { T value }
typedef<class> Entry : View<Slot<string>> {}
fn read<T>(View<Slot<T>> item) {}
fn forward<U: View<Slot<string>>>(U item) { read(item) }
fn byInterface(Entry item) = read(item)
fn use(Entry item) { forward(item) }
fn<U> select<T,U = T>(T a,U b) = b
fn<int<8>> placeholder(int<8> n) = select<bool,_>(true,n)
fn<bool> defaulted() = select<bool>(true,true)
typedef<class> Pair<A,B=int>(def<A> left, def<B> right) {
    pub mt<Pair<bool,int<8>>> narrow(int<8> n) = Pair<bool,_>::new(true,n)
    pub mt<Pair<bool,int>> ordinary() = Pair<bool>::new(true,1)
}
fn<Pair<bool,int<8>>> construct(Pair<bool,int> pair, int<8> n) = pair.narrow(n)
)");
	ASSERT_NE(callIn(cp_program, "byInterface").instance, nullptr);
	const DeclarationInfo &di_read = callIn(cp_program, "byInterface").instance->signature;
	EXPECT_EQ(di_read.parameters.front().spelling(), "View<Slot<string>>");
	EXPECT_EQ(callIn(cp_program, "placeholder").types.front().width, 8u);
	EXPECT_EQ(callIn(cp_program, "defaulted").types.front(), ResolvedType(kBoolean));
	EXPECT_EQ(callIn(cp_program, "construct").types.front().arguments().back().width, 8u);
	expectError("fn pick<T,U=int>(T a,U b) {}\nfn use(int<8> n) { pick<bool>(true,n) }", cde::DiagnosticCode::NO_MATCHING_CALL);
	expectError("fn pick<T,U>(T a,U b) {}\nfn use() { pick<bool>(true,1) }", cde::DiagnosticCode::GENERIC_ARITY);
	expectError("typedef<class> Box<T> {}\nfn<Box<int>> use() = Box::new()", cde::DiagnosticCode::GENERIC_INFERENCE_FAILED);
}

TEST (SemanticGenerics, ChecksDeclaredAndSubstitutedBoundsBeforeUse) {
	using Code = cde::DiagnosticCode;
	expectError("typedef<interface> I {}\nfn f<T:I & I>() {}", Code::INVALID_GENERIC_BOUND);
	expectError("fn f<T:new(int a) & new(int<32> b)>() {}", Code::INVALID_GENERIC_BOUND);
	expectError("typedef<class> Box<T> { mt f<T>(T x) {} }", Code::INVALID_GENERIC_PARAMETER);
	expectError("fn f<T=T>() {}", Code::INVALID_GENERIC_ARGUMENT);
	expectError("typedef<struct> Box<T> { T value }\nfn f<T=Box<U>,U=int>() {}", Code::INVALID_GENERIC_ARGUMENT);
	expectError("typedef<class> Box<T=Box<>> {}", Code::INVALID_GENERIC_ARGUMENT);
	expectError("typedef<class> Box<T> {}\nfn f<T:Box<T>[]>() {}", Code::INVALID_GENERIC_BOUND);
	expectError("typedef<interface> I<T> {}\ntypedef<interface> J : I<int> {}\nfn f<T:J & I<bool>>() {}", Code::CONFLICTING_INTERFACE_APPLICATION);
	const std::string s_source = "typedef<interface> I<N> {}\ntypedef<class> P : I<int> {}\nfn f<A,B,T: I<A> & I<B>>(T item) {}\n";
	EXPECT_NO_THROW(checkSource(s_source + "fn use(P item) { f<int,int,_>(item) }"));
	expectError(s_source + "fn use(P item) { f<int,bool,_>(item) }", Code::CONFLICTING_INTERFACE_APPLICATION);
	expectError("typedef<interface> I {}\nfn f<T:I>(T a) {}\nfn f<U:I>(U a,opt<int> b ?: 0) {}\nfn use() { f(1) }", Code::UNSATISFIED_GENERIC_BOUND);
}

TEST (SemanticGenerics, OnlyTheStandardLibraryNumberGrantsPrimitiveConformance) {
	using Code                            = cde::DiagnosticCode;
	const std::filesystem::path p_library = std::filesystem::path(__FILE__).parent_path() / "../../../../Stdlib";
	const std::function         check     = [&] (const std::string &source, Code code) {
		SCOPED_TRACE(source);
		Lexer l_lexer{ SourceFile("numeric-client.co", "using cloth.lang.num.Number: Number\n" + source) };
		try {
			CheckedProgram cp_program(SymbolTable(Parser(l_lexer).parse()), {}, p_library);
			FAIL() << "Expected a semantic diagnostic";
		} catch (const cde::ClothDiagnostic &error) {
			EXPECT_EQ(error.get_code(), code) << error.what();
			ASSERT_FALSE(error.get_source_labels().empty());
		}
	};
	for (const char *s_argument : { "bool", "char", "string", "float?", "int[]" })
		check(std::format("fn f<T: Number>() {{}}\nfn use() {{ f<{}>() }}", s_argument), Code::UNSATISFIED_GENERIC_BOUND);
	check("fn f<T: Number>() {}\nfn unused<U>() { f<U>() }", Code::UNSATISFIED_GENERIC_BOUND);
	check("fn f<T: Number?>() {}\nfn use() { f<int>() }", Code::UNSATISFIED_GENERIC_BOUND);
	check("typedef<class> Local { pub typedef<interface> Number {} }\nfn f<T: Local.Number>() {}\nfn use() { f<int>() }", Code::UNSATISFIED_GENERIC_BOUND);
	check("fn f<T: Number[]>() {}\nfn use() { f<int[2]>() }", Code::UNSATISFIED_GENERIC_BOUND);
	check("fn<bool> same<T: Number>(T a,T b) = a === b", Code::INCOMPARABLE_TYPES);
	check("typedef<class> Empty : Number {}\nfn<T> sum<T:Number>(T v) = v+v\nfn<Empty> use(Empty v) = sum(v)", Code::BINARY_OPERANDS);
	expectError("fn f<T:Number>() {}", Code::UNSUPPORTED_TYPE);
	expectError("typedef<interface> Number {}\nfn f<T:Number>() {}\nfn use() { f<int>() }", Code::UNSATISFIED_GENERIC_BOUND);
	expectError("typedef<interface> Number {}\nfn<T> f<T:Number>(T v) = v+v", Code::BINARY_OPERANDS);
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
	const CheckedProgram  cp_program = checkSource(R"(typedef<class> Base<T>(int seed) {
    pub Base() : (0) {}
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
	const CheckedProgram   cp_program = checkSource(R"(typedef<class> Base(int seed) {
    pub Base() : (0) {}
    pub mt<int> pick(Base x) = 1
}
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

TEST (SemanticGenerics, ConstructorsAndTypeTestsResolveShadowedTypeNames) {
	using Code                        = cde::DiagnosticCode;
	const CheckedProgram  cp_program  = checkSource(R"(typedef<class> Item(int ignored) { pub Item() : (0) {} }
fn<Item> concrete(int Item) = Item::new()
fn<T> create<T: new()>(int T) = T::new()
fn<Item> closed() = create<Item>(4)
fn<bool> matches<T>(T value, int T) = value is T
fn<bool> check(Item value) = matches<Item>(value, 3)
fn<bool> stopped() = exit(0) is Item
typedef<class> Holder(def<int> Item) { pub mt<Item> copy(Item item) = item }
)");
	const SymbolScope    &ss_globals  = cp_program.units.front()->get_globals();
	const Symbol         &sym_item    = ss_globals.at("Item").front();
	const ExpressionInfo &ei_concrete = callIn(cp_program, "concrete");
	ASSERT_NE(ei_concrete.symbol, nullptr);
	EXPECT_EQ(ei_concrete.symbol->kind, SymbolKind::Constructor);
	EXPECT_EQ(ei_concrete.symbol->visibility, Visibility::Public);
	EXPECT_EQ(ei_concrete.types.front().declaration, &sym_item);
	const ExpressionInfo &ei_stopped = callIn(cp_program, "stopped");
	EXPECT_FALSE(ei_stopped.fallsThrough);
	EXPECT_EQ(ei_stopped.testedType, ei_concrete.types.front());
	const GenericInstance *gi_closed = callIn(cp_program, "closed").instance;
	ASSERT_NE(gi_closed, nullptr);
	const ExpressionInfo &ei_construction = gi_closed->body.expressions.at(&bodyExpression(ss_globals.at("create").front()));
	EXPECT_EQ(ei_construction.symbol, ei_concrete.symbol);
	EXPECT_EQ(ei_construction.argumentContract, callIn(cp_program, "create").argumentContract);
	const Expression     &e_test  = bodyExpression(ss_globals.at("matches").front());
	const ExpressionInfo &ei_open = cp_program.expressions.at(&e_test);
	ASSERT_TRUE(ei_open.testedType);
	EXPECT_EQ(ei_open.testedType->parameter, cp_program.declarations.at(&ss_globals.at("matches").front()).parameters.front().parameter);
	EXPECT_TRUE(cp_program.valueTests.empty());
	const GenericInstance *gi_check = callIn(cp_program, "check").instance;
	ASSERT_NE(gi_check, nullptr);
	EXPECT_EQ(gi_check->body.expressions.at(&e_test).testedType, ei_concrete.types.front());
	expectError("fn f(int n) { n::new() }", Code::INVALID_CLASS_LIFECYCLE);
	expectError("typedef<class> Item(int seed) {}\nfn<Item> f(int Item) = Item::new(1)", Code::PRIVATE_CLASS_MEMBER);
}

TEST (SemanticGenerics, ConstructorOnlyBoundsSupplyTheObjectView) {
	using Code = cde::DiagnosticCode;
	EXPECT_NO_THROW(checkSource(R"(fn<Object> forward<U: Object>(U obj) = obj
fn<Object> erase<T: new()>(T item) = item
fn<Object> next<T: new(int n)>(T item) = forward(item)
fn<Object?> nullable<T: new()>(T? item) = item
fn<Object> constructed<T: new()>() = T::new()
)"));
	expectError("fn<Object> erase<T>(T item) = item", Code::RETURN_TYPE);
	expectError("fn<Object> erase<T: new()>(T? item) = item", Code::RETURN_TYPE);
	expectError("fn make<T: new()>() {}\nfn run() { make<int>() }", Code::UNSATISFIED_GENERIC_BOUND);
	expectError("typedef<struct> S { int x }\nfn make<T: new()>() {}\nfn run() { make<S>() }", Code::UNSATISFIED_GENERIC_BOUND);
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
	            cde::DiagnosticCode::UNSATISFIED_GENERIC_BOUND);
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
