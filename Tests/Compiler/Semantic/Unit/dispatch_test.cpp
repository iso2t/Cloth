// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "checked_program_test.hpp"

namespace cloth::test {
namespace {

using Code = cde::DiagnosticCode;

const Symbol &global (const CheckedProgram &program, const char *name) {
	return program.units.front()->get_globals().at(name).front();
}

const ExpressionInfo &callIn (const CheckedProgram &program, const char *name) {
	return program.expressions.at(&bodyExpression(global(program, name)));
}

const TypeDispatch &tableFor (const CheckedProgram &program, const char *name) {
	return program.dispatchTables.at(program.declarations.at(&global(program, name)).parameters.front());
}

TEST (SemanticDispatch, SharesOneClosedDefaultAcrossBothRoutesOfADiamond) {
	const CheckedProgram cp_program = checkSource(R"(typedef<interface> Root<T> {
    pub mt<T> pick(T first, opt<T> tail ?: first) = default (tail)
}
typedef<interface> Left<A> : Root<A> {}
typedef<interface> Right<B> : Root<B> {}
typedef<interface> Diamond : Left<int>, Right<int<32>> {}
typedef<class> Holder : Diamond {}
fn<int> root(Root<int> item) = item.pick(first=1)
fn<int> diamond(Diamond item) = item.pick(2)
fn<int> holder(Holder item) = item.pick(3)
)");
	const Symbol        &sym_pick   = global(cp_program, "Root").members.at("pick").front();
	const TypeDispatch  &td_holder  = tableFor(cp_program, "holder");
	ASSERT_EQ(td_holder.methods.size(), 1u);
	ASSERT_EQ(td_holder.interfaceTargets.size(), 4u);
	for (const InterfaceTarget &it_target : td_holder.interfaceTargets) {
		EXPECT_EQ(it_target.contract.symbol, &sym_pick);
		EXPECT_EQ(it_target.contract, it_target.implementation);
		EXPECT_EQ(it_target.contract.owner.arguments(), ResultTypes{ ResolvedType(kInteger) });
	}
	for (const char *s_name : { "root", "diamond", "holder" }) {
		const ExpressionInfo &ei_call = callIn(cp_program, s_name);
		ASSERT_TRUE(ei_call.dispatch);
		EXPECT_EQ(ei_call.dispatch->kind, DispatchKind::Interface);
		EXPECT_EQ(ei_call.dispatch->receiver, cp_program.declarations.at(&global(cp_program, s_name)).parameters.front());
		EXPECT_EQ(ei_call.argumentContract, &sym_pick);
		EXPECT_EQ(ei_call.omittedParameters, (std::vector<std::size_t>{ 1 }));
		EXPECT_EQ(ei_call.types, ResultTypes{ ResolvedType(kInteger) });
	}
}

TEST (SemanticDispatch, UsesEachInterfaceViewsLabelsAndDefaultsWithTheMostSpecificBody) {
	const CheckedProgram  cp_program = checkSource(R"(typedef<interface> Parent {
    pub mt<int> read(opt<int> first ?: 1, opt<int> last ?: first) = default (last)
}
typedef<interface> Child : Parent {
    pub mt<int> read(opt<int> count ?: 5, opt<int> extra ?: count) = default (count)
}
typedef<class> Reader : Child {}
fn<int> parent(Parent item) = item.read(last=2)
fn<int> child(Child item) = item.read(extra=3, count=4)
fn<int> reader(Reader item) = item.read()
)");
	const Symbol         &sym_parent = global(cp_program, "Parent").members.at("read").front();
	const Symbol         &sym_child  = global(cp_program, "Child").members.at("read").front();
	const ExpressionInfo &ei_parent = callIn(cp_program, "parent"), &ei_child = callIn(cp_program, "child");
	EXPECT_EQ(ei_parent.argumentContract, &sym_parent);
	EXPECT_EQ(ei_parent.omittedParameters, (std::vector<std::size_t>{ 0 }));
	EXPECT_EQ(ei_child.argumentContract, &sym_child);
	EXPECT_EQ(ei_child.parameterForArgument, (std::vector<std::size_t>{ 1, 0 }));
	EXPECT_EQ(callIn(cp_program, "reader").argumentContract, &sym_child);
	const TypeDispatch &td_reader = tableFor(cp_program, "reader");
	ASSERT_EQ(td_reader.interfaceTargets.size(), 2u);
	for (const InterfaceTarget &it_target : td_reader.interfaceTargets) {
		EXPECT_EQ(it_target.contract.symbol, it_target.view.declaration == &global(cp_program, "Parent") ? &sym_parent : &sym_child);
		EXPECT_EQ(it_target.implementation.symbol, &sym_child);
	}
}

TEST (SemanticDispatch, ResolvesIndependentDefaultsWithAnExplicitClassImplementation) {
	const std::string s_interfaces = R"(typedef<interface> A { pub mt<int> pick() = default (1) }
typedef<interface> B { pub mt<int> pick() = default (2) }
)";
	for (const char *s_parents : { "A, B", "B, A" }) {
		const std::string s_class = s_interfaces + "typedef<class> C : " + s_parents;
		expectError(s_class + " {}", Code::CONFLICTING_INTERFACE_DEFAULTS);
		expectError(s_class + " { pub mt<int> pick() = 3 }", Code::INVALID_INTERFACE_OVERRIDE);
		const CheckedProgram cp_program = checkSource(s_class + " { override: pub mt<int> pick() = 3 }\nfn<int> read(C item) = item.pick()");
		const TypeDispatch  &td_class   = tableFor(cp_program, "read");
		ASSERT_EQ(td_class.interfaceTargets.size(), 2u);
		for (const InterfaceTarget &it_target : td_class.interfaceTargets)
			EXPECT_EQ(it_target.implementation.symbol, &global(cp_program, "C").members.at("pick").front());
	}
	expectError("typedef<interface> A { pub mt<int> read(int a) }\ntypedef<interface> B { pub mt<int> read(int b) }\ntypedef<interface> C : A, B {}",
	            Code::INCOMPATIBLE_INTERFACE_REQUIREMENT);
	expectError("typedef<interface> A { pub mt<int> read() = default (1) }\ntypedef<interface> B : A { pub mt<int> read() }",
	            Code::INCOMPATIBLE_INTERFACE_REQUIREMENT);
	expectError("typedef<interface> A { pub mt<int, bool> pair() }\ntypedef<interface> B : A { pub mt<bool, int> pair() }",
	            Code::INCOMPATIBLE_INTERFACE_REQUIREMENT);
	expectError("typedef<interface> A<T> {}\ntypedef<interface> B : A<int> {}\ntypedef<class> C : B, A<bool> {}", Code::CONFLICTING_INTERFACE_APPLICATION);
}

TEST (SemanticDispatch, PreservesPrivateHelpersAndChecksSharedConstructorContracts) {
	EXPECT_NO_THROW(checkSource(R"(typedef<interface> I {
    mt<int> secret() = default (2)
    pub mt<int> visible() = default (self.secret())
}
typedef<class> C : I {}
fn<int> run(I i) = i.visible()
)"));
	expectError("typedef<interface> I { mt<int> secret() = default (2) }\nfn<int> leak(I i) = i.secret()", Code::PRIVATE_CLASS_MEMBER);
	expectError("typedef<interface> I { pub mt<int> f() }\ntypedef<interface> J : I { mt<int> f() = default (1) }",
	            Code::INCOMPATIBLE_INTERFACE_REQUIREMENT);
	expectError(R"(typedef<interface> A { pub mt<T> make<T: new(int x)>() }
typedef<interface> B { pub mt<U> make<U: new(int y)>() }
typedef<class> C : A, B { override: pub mt<V> make<V>() = V::new(1) }
)",
	            Code::INCOMPATIBLE_INTERFACE_REQUIREMENT);
}

TEST (SemanticDispatch, PreservesTheRootVirtualContractAcrossThreeClassLevels) {
	const CheckedProgram cp_program = checkSource(R"(typedef<class> Leaf : Mid {}
typedef<class> Mid(int seed) : Base {
    pub Mid() : (0) {}
    override: pub mt<int, bool> read(int renamed) = renamed, true
}
typedef<class> Base(int seed) {
    pub Base() : (0) {}
    virtual: pub mt<int, bool> read(opt<int> count ?: 2) = count, false
}
fn<int, bool> base(Base item) = item.read()
fn<int, bool> leaf(Leaf item) = item.read(count=3)
)");
	const Symbol        &sym_base = global(cp_program, "Base").members.at("read").front(), &sym_mid = global(cp_program, "Mid").members.at("read").front();
	EXPECT_EQ(callIn(cp_program, "base").argumentContract, &sym_base);
	const ExpressionInfo &ei_leaf = callIn(cp_program, "leaf");
	ASSERT_TRUE(ei_leaf.dispatch);
	EXPECT_EQ(ei_leaf.dispatch->kind, DispatchKind::Virtual);
	EXPECT_EQ(ei_leaf.argumentContract, &sym_base);
	EXPECT_EQ(ei_leaf.symbol, &sym_mid);
	EXPECT_EQ(ei_leaf.types, (ResultTypes{ ResolvedType(kInteger), ResolvedType(kBoolean) }));
	const TypeDispatch &td_leaf = tableFor(cp_program, "leaf");
	ASSERT_EQ(td_leaf.methods.size(), 1u);
	ASSERT_TRUE(td_leaf.methods.front().implementation);
	EXPECT_EQ(td_leaf.methods.front().implementation->owner.declaration, &global(cp_program, "Mid"));
	EXPECT_EQ(td_leaf.methods.front().contract.owner.declaration, &global(cp_program, "Base"));
	const std::string s_base = "typedef<class> A { virtual: pub mt<int, bool> go(int n) = n, true }\ntypedef<class> B : A { ";
	for (const char *s_bad : { "pub mt<int, bool> go(int n) = n, true", "override: mt<int, bool> go(int n) = n, true",
	                           "override: pub mt<bool, int> go(int n) = true, n", "override: pub mt<int, bool> go(opt<int> n ?: 7) = n, true" }) {
		expectError(s_base + s_bad + " }", Code::INVALID_VIRTUAL_METHOD);
	}
	expectError(s_base + "override: pub mt<int, bool> go(bool n) = 1, n }", Code::INVALID_INTERFACE_OVERRIDE);
}

TEST (SemanticDispatch, ResolvesStateContractsSelectionsAndOverloadsOnTheHost) {
	const CheckedProgram cp_program = checkSource(R"(typedef<class> Host(int seed) {
    pub Host() : (0) {}
    pub state<int, bool> ::score(opt<int> power ?: 1, opt<int> bonus ?: power)
    default: pub state<void> Ready {
        mt<int, bool> score(int first, int second) = first + second, true
        pub mt<bool> score(bool pass) = pass
    }
    pub mt<int> score(int first, int second) = first - second
}
typedef<class> Child(int seed) : Host {
    pub Child() : (0) {}
    default: (1) pub state<int> Busy {
        mt<int, bool> score(int a, int b) = state, false
        pub mt<bool> score(bool pass) = pass
    }
}
typedef<class> Leaf : Child {}
fn<int, bool> omitted(Leaf item) = item::score()
fn<int, bool> named(Host item) = item::score(bonus=2, power=1)
fn<int> ordinary(Host item) = item.score(1, 2)
fn<bool> implicit(Leaf item) = item::score(true)
fn<bool> ready(Leaf item) = item is Ready
)");
	const Symbol        &sym_host = global(cp_program, "Host"), &sym_ready = sym_host.members.at("Ready").front(),
	             &sym_busy        = global(cp_program, "Child").members.at("Busy").front();
	const ExpressionInfo &ei_call = callIn(cp_program, "omitted");
	ASSERT_TRUE(ei_call.dispatch);
	EXPECT_EQ(ei_call.dispatch->kind, DispatchKind::State);
	EXPECT_EQ(ei_call.argumentContract->kind, SymbolKind::StateMethodContract);
	EXPECT_EQ(ei_call.omittedParameters, (std::vector<std::size_t>{ 0, 1 }));
	EXPECT_EQ(ei_call.types, (ResultTypes{ ResolvedType(kInteger), ResolvedType(kBoolean) }));
	EXPECT_EQ(callIn(cp_program, "named").parameterForArgument, (std::vector<std::size_t>{ 1, 0 }));
	EXPECT_NE(callIn(cp_program, "ordinary").argumentContract, ei_call.argumentContract);
	EXPECT_EQ(callIn(cp_program, "ready").symbol, &sym_ready);
	const TypeDispatch &td_leaf = tableFor(cp_program, "omitted");
	EXPECT_EQ(td_leaf.initialState, &sym_busy);
	EXPECT_EQ(td_leaf.states.at("Busy").payload, ResolvedType(kInteger));
	ASSERT_EQ(td_leaf.stateMethods.size(), 2u);
	for (const StateMethod &sm_method : td_leaf.stateMethods) {
		EXPECT_EQ(sm_method.contract.owner.declaration, &sym_host);
		ASSERT_EQ(sm_method.implementations.size(), 2u);
		EXPECT_EQ(sm_method.implementations.at(&sym_ready).owner.declaration, &sym_host);
		EXPECT_EQ(sm_method.implementations.at(&sym_busy).owner.declaration, &global(cp_program, "Child"));
		if (sm_method.contract.symbol->kind == SymbolKind::Method) {
			EXPECT_EQ(sm_method.contract.symbol, callIn(cp_program, "implicit").argumentContract);
		}
	}
}

TEST (SemanticDispatch, RejectsMismatchedStateContractsAndPayloads) {
	const std::string s_host = "typedef<class> C { default: pub state<void> Ready {}\n pub state<int> Full {} }\ntypedef<class> Other {}\n";
	for (const char *s_expr : { "item -> Full", "item -> Full(false)", "item -> Ready(1)" })
		expectError(s_host + "fn move(C item) { " + s_expr + " }", Code::INVALID_STATE_PAYLOAD);
	for (const char *s_expr : { "other is Ready", "other -> Ready()", "other::unknown()" })
		expectError(s_host + "fn test(Other other) { " + s_expr + " }", Code::INVALID_STATE_TARGET);
	expectError(s_host + "fn bad(C? item) { item::read() }", Code::NULLABLE_PAYLOAD_ACCESS);
	for (const char *s_state : { "default: (false) state<int> Ready {}", "default: (1) state<void> Ready {}", "default: state<int> Ready {}" })
		expectError("typedef<class> C { " + std::string(s_state) + " }", Code::INVALID_STATE_PAYLOAD);
	expectError("typedef<class> C { default: state<void> Ready { mt<int> read() = state } }", Code::UNKNOWN_NAME);
	expectError("typedef<class> C { default: state<void> A { pub mt<int> f(int x) = x }\n state<void> B { pub mt<int> f(int y) = y } }",
	            Code::INVALID_STATE_CONTRACT);
	expectError("typedef<class> C { pub state<int> ::f()\n default: state<void> A { mt<bool> f() = true } }", Code::INVALID_STATE_CONTRACT);
	expectError("typedef<class> C { default: state<void> Ready { mt<int> f(opt<int> x ?: 1) = x } }", Code::INVALID_STATE_CONTRACT);
	expectError("typedef<class> C { default: state<void> A {}\n default: state<void> B {} }", Code::INVALID_STATE_DECLARATION);
	expectError("typedef<state<void>> Bad for int {}", Code::INVALID_STATE_ATTACHMENT);
}

TEST (SemanticDispatch, RequiresADefaultWhereStatesAreIntroduced) {
	expectError("typedef<class> Host { state<void> Ready {} }", Code::INVALID_STATE_DECLARATION);
	expectError("typedef<state<void>> Ready for Host {}\ntypedef<class> Host { state Ready }", Code::INVALID_STATE_DECLARATION);
	const std::string s_parent = "typedef<class> Base(int seed) { pub Base() : (0) {}\n default: state<void> Ready {} }\n";
	expectError(s_parent + "typedef<class> Child : Base { state<void> Busy {} }", Code::INVALID_STATE_DECLARATION);
	expectError(s_parent + "typedef<state<void>> Busy for Base {}\ntypedef<class> Child : Base { state Busy }", Code::INVALID_STATE_DECLARATION);
	EXPECT_NO_THROW(checkSource(s_parent + "typedef<class> Child : Base {}"));
	EXPECT_NO_THROW(checkSource(s_parent + "typedef<state<void>> Busy for Base {}\ntypedef<class> Child : Base { default: state Busy }"));
}

TEST (SemanticDispatch, MaterializesGenericImplementationsSelectedThroughStaticContracts) {
	const CheckedProgram                                            cp_program = checkSource(R"(typedef<interface> I { pub mt<V> echo<V>(V label) }
typedef<class> Base(int seed) {
    pub Base() : (0) {}
    virtual: pub mt<X> get<X>(X value) = value
    pub state<T> ::read<T>(T item)
    default: state<void> Ready { mt<U> read<U>(U renamed) = renamed }
}
typedef<class> Child<A> : Base, I {
    override: pub mt<B> echo<B>(B own) = own
    override: pub mt<Y> get<Y>(Y name) = name
    default: state<void> Busy { mt<W> read<W>(W other) = other }
}
fn<bool> face(I item) = item.echo<bool>(label=true)
fn<int> virtualCall(Base item) = item.get<int>(value=1)
fn<string> stateCall(Base item) = item::read<string>(item="ok")
fn<Base> up(Child<int> item) = item
)");
	const ResolvedType  &rt_child   = cp_program.declarations.at(&global(cp_program, "up")).parameters.front();
	const Symbol        &sym_child = global(cp_program, "Child"), &sym_busy = sym_child.members.at("Busy").front();
	const std::function<void(const Symbol &, const ResolvedType &)> check = [&] (const Symbol &body, const ResolvedType &argument) {
		const GenericInstance &gi_instance = cp_program.instances.at({ &body, rt_child, { argument } });
		EXPECT_EQ(gi_instance.signature.returns, ResultTypes{ argument });
		EXPECT_EQ(gi_instance.body.expressions.at(&bodyExpression(body)).types, ResultTypes{ argument });
	};
	check(sym_child.members.at("echo").front(), ResolvedType(kBoolean));
	check(sym_child.members.at("get").front(), ResolvedType(kInteger));
	check(sym_busy.members.at("read").front(), ResolvedType(kString));
}

TEST (SemanticDispatch, ChecksAnAttachedBodyAgainstEachHostAndKeepsItsLexicalReceiver) {
	const std::string    s_decls    = R"(typedef<interface> Host { pub mt<int> id() }
typedef<state<void>> Shared for Host {
    pub mt<bool> ready() = self is Ready
    pub mt<int> idFromHost() = self.id()
}
typedef<class> A : Host {
    override: pub mt<int> id() = 1
    default: state Shared
    pub state<void> Ready {}
}
)";
	const CheckedProgram cp_program = checkSource(s_decls + R"(typedef<class> B<T> : Host {
    override: pub mt<int> id() = 2
    default: state Shared
    pub state<int> Ready {}
}
fn<bool> a(A item) = item::ready()
fn<bool> b(B<int> item) = item::ready()
)");
	const Symbol        &sym_shared = global(cp_program, "Shared"), &sym_query = sym_shared.members.at("ready").front(),
	             &sym_read   = sym_shared.members.at("idFromHost").front();
	const ResolvedType &rt_a = cp_program.declarations.at(&global(cp_program, "a")).parameters.front(),
	                   &rt_b = cp_program.declarations.at(&global(cp_program, "b")).parameters.front();
	const BodyFacts &bf_a    = cp_program.attachmentBodies.at(rt_a);
	EXPECT_EQ(bf_a.expressions.at(&bodyExpression(sym_query)).symbol, &global(cp_program, "A").members.at("Ready").front());
	EXPECT_EQ(bf_a.expressions.at(&bodyExpression(sym_read)).argumentContract, &global(cp_program, "Host").members.at("id").front());
	const GenericInstance &gi_b = cp_program.instances.at({ &sym_query, rt_b, {} });
	EXPECT_EQ(gi_b.body.expressions.at(&bodyExpression(sym_query)).symbol, &global(cp_program, "B").members.at("Ready").front());
	EXPECT_EQ(cp_program.dispatchTables.at(rt_b).states.at("Shared").definition, &sym_shared);
	expectError(s_decls + "typedef<class> Bad : Host { override: pub mt<int> id() = 3; default: state Shared }", Code::INVALID_STATE_TARGET);
	expectError(
	        "typedef<class> Host { def<int> hidden = 1; default: state Shared }\ntypedef<state<void>> Shared for Host { pub mt<int> bad() = self.hidden }",
	        Code::PRIVATE_CLASS_MEMBER);
}

TEST (SemanticDispatch, ChecksEveryPublicSignatureIncludingBoundsAndInferredStateMethods) {
	const std::string s_types = "typedef<class> Secret {}\npub typedef<class> Box<T> {}\n";
	for (const char *s_decl : { "pub fn f<T: Secret>(T x) {}", "pub fn f<T: new(Box<Secret> value)>(T x) {}", "pub typedef<class> Api<T = Secret> {}",
	                            "pub typedef<interface> Api { pub mt<Box<Secret>[]> leak() }", "pub typedef<class> Api { pub def<Secret?> leaked = null }",
	                            "pub typedef<class> Api { default: state<void> Hidden { pub mt<Secret> leak(Secret item) = item } }" }) {
		expectError(s_types + s_decl, Code::PRIVATE_GENERIC_SIGNATURE);
	}
	EXPECT_NO_THROW(checkSource(s_types + "pub typedef<class> Api { mt<Secret> internal(Secret item) = item }"));
}

TEST (SemanticDispatch, InheritsGenericMethodBoundsAndConstructorGuarantees) {
	const CheckedProgram   cp_program = checkSource(R"(typedef<interface> Label { pub mt<int> read() }
typedef<class> Value : Label { override: pub mt<int> read() = 5 }
typedef<class> Base(int seed) {
    pub Base() : (0) {}
    virtual: pub mt<int> describe<T: Label>(T value) = value.read()
    virtual: pub mt<T> make<T: new(int count)>(int value) = T::new(count=value)
}
typedef<class> Child : Base {
    override: pub mt<int> describe<U>(U item) = item.read()
    override: pub mt<R> make<R>(int item) = R::new(count=item)
}
typedef<class> Product { pub Product(int renamed) : () {} }
fn<int> describe(Base item, Value value) = item.describe(value)
fn<Product> make(Base item) = item.make<Product>(1)
fn<Base> up(Child item) = item
)");
	const ResolvedType    &rt_child   = cp_program.declarations.at(&global(cp_program, "up")).parameters.front();
	const ResolvedType    &rt_product = cp_program.declarations.at(&global(cp_program, "make")).returns.front();
	const Symbol          &sym_make   = global(cp_program, "Child").members.at("make").front();
	const GenericInstance &gi_make    = cp_program.instances.at({ &sym_make, rt_child, { rt_product } });
	const ExpressionInfo  &ei_create  = gi_make.body.expressions.at(&bodyExpression(sym_make));
	EXPECT_EQ(ei_create.types, ResultTypes{ rt_product });
	ASSERT_NE(ei_create.argumentContract, nullptr);
	EXPECT_NE(ei_create.argumentContract, ei_create.symbol);
	EXPECT_EQ(cp_program.declarations.at(ei_create.argumentContract).owner, &global(cp_program, "Base").members.at("make").front());
}

}
}
