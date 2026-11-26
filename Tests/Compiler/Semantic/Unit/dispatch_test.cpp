// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "cde/diagnostic_code.hpp"
#include "checked_program_test.hpp"
#include "gtest/gtest.h"
#include "semantic/checked_program.hpp"
#include "semantic/symbol_table.hpp"
#include "token/token.hpp"

namespace cloth::test {
namespace {

using Code = cde::DiagnosticCode;

const Symbol &global (const CheckedProgram &program, const char *name) {
	return program.units.front()->getGlobals().at(name).front();
}

const ExpressionInfo &callIn (const CheckedProgram &program, const char *name) {
	return program.expressions.at(&bodyExpression(global(program, name)));
}

const TypeDispatch &tableFor (const CheckedProgram &program, const char *name) {
	return program.dispatchTables.at(program.declarations.at(&global(program, name)).parameters.front());
}

TEST (SemanticDispatch, SharesOneClosedDefaultAcrossBothRoutesOfADiamond) {
	const CheckedProgram cp_program = checkSource(R"(typedef<interface> Root<T> {
    pub mt<T> pick(T first, opt<T> tail ?: first) = default (tail);
}
typedef<interface> Left<A> : Root<A> {}
typedef<interface> Right<B> : Root<B> {}
typedef<interface> Diamond : Left<int>, Right<int<32>> {}
typedef<class> Holder : Diamond {}
fn<int> root(Root<int> item) = item.pick(first=1);
fn<int> diamond(Diamond item) = item.pick(2);
fn<int> holder(Holder item) = item.pick(3);
)");
	const Symbol        &sym_pick   = global(cp_program, "Root").members.at("pick").front();
	const TypeDispatch  &td_holder  = tableFor(cp_program, "holder");
	ASSERT_EQ(td_holder.methods.size(), 1u);
	ASSERT_EQ(td_holder.interfaceTargets.size(), 4u);
	for (const InterfaceTarget &it_target : td_holder.interfaceTargets) {
		EXPECT_EQ(it_target.contract.symbol, &sym_pick);
		EXPECT_EQ(it_target.contract, it_target.implementation);
		EXPECT_EQ(it_target.contract.owner.arguments(), ResultTypes{ ResolvedType(KInteger) });
	}
	for (const char *s_name : { "root", "diamond", "holder" }) {
		const ExpressionInfo &ei_call = callIn(cp_program, s_name);
		ASSERT_TRUE(ei_call.dispatch);
		EXPECT_EQ(ei_call.dispatch->kind, DispatchKind::Interface);
		EXPECT_EQ(ei_call.dispatch->receiver, cp_program.declarations.at(&global(cp_program, s_name)).parameters.front());
		EXPECT_EQ(ei_call.argumentContract, &sym_pick);
		EXPECT_EQ(ei_call.omittedParameters, (std::vector<std::size_t>{ 1 }));
		EXPECT_EQ(ei_call.types, ResultTypes{ ResolvedType(KInteger) });
	}
}

TEST (SemanticDispatch, UsesEachInterfaceViewsLabelsAndDefaultsWithTheMostSpecificBody) {
	const CheckedProgram  cp_program = checkSource(R"(typedef<interface> Parent {
    pub mt<int> read(opt<int> first ?: 1, opt<int> last ?: first) = default (last);
}
typedef<interface> Child : Parent {
    pub mt<int> read(opt<int> count ?: 5, opt<int> extra ?: count) = default (count);
}
typedef<class> Reader : Child {}
fn<int> parent(Parent item) = item.read(last=2);
fn<int> child(Child item) = item.read(extra=3, count=4);
fn<int> reader(Reader item) = item.read();
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

TEST (SemanticDispatch, RequiresOverrideToReplaceAnInterfaceDefault) {
	const std::string s_abstract = "typedef<interface> Readable { pub mt<int> read(); }\n";
	const std::string s_default  = "typedef<interface> Readable { pub mt<int> read() = default (1); }\n";
	const std::string s_plain    = "typedef<class> Reader : Readable { pub mt<int> read() = 2; }\n";
	const std::string s_override = "typedef<class> Reader : Readable { pub override mt<int> read() = 2; }\n";

	for (const std::string &s_declarations : { s_abstract + s_plain, s_abstract + s_override, s_default + s_override }) {
		SCOPED_TRACE(s_declarations);
		const CheckedProgram  cp_program = checkSource(s_declarations + R"(fn<int> through(Readable item) = item.read();
fn<int> use(Reader item) = through(item);
)");
		const ExpressionInfo &ei_call    = callIn(cp_program, "through");
		ASSERT_TRUE(ei_call.dispatch);
		EXPECT_EQ(ei_call.dispatch->kind, DispatchKind::Interface);

		const TypeDispatch &td_reader = tableFor(cp_program, "use");
		ASSERT_EQ(td_reader.interfaceTargets.size(), 1u);
		const InterfaceTarget &it_read = td_reader.interfaceTargets.front();
		EXPECT_EQ(it_read.contract, ei_call.dispatch->contract);
		EXPECT_EQ(it_read.contract.symbol, &global(cp_program, "Readable").members.at("read").front());
		EXPECT_EQ(it_read.implementation.symbol, &global(cp_program, "Reader").members.at("read").front());
	}
	expectError(s_default + s_plain, Code::InvalidInterfaceOverride);
}

TEST (SemanticDispatch, ResolvesIndependentDefaultsWithAnExplicitClassImplementation) {
	const std::string s_interfaces = R"(typedef<interface> A { pub mt<int> pick() = default (1); }
typedef<interface> B { pub mt<int> pick() = default (2); }
)";
	for (const char *s_parents : { "A, B", "B, A" }) {
		const std::string s_class = s_interfaces + "typedef<class> C : " + s_parents;
		expectError(s_class + " {}", Code::ConflictingInterfaceDefaults);
		expectError(s_class + " { pub mt<int> pick() = 3; }", Code::InvalidInterfaceOverride);
		const CheckedProgram cp_program = checkSource(s_class + " { pub override mt<int> pick() = 3; }\nfn<int> read(C item) = item.pick();");
		const TypeDispatch  &td_class   = tableFor(cp_program, "read");
		ASSERT_EQ(td_class.interfaceTargets.size(), 2u);
		for (const InterfaceTarget &it_target : td_class.interfaceTargets)
			EXPECT_EQ(it_target.implementation.symbol, &global(cp_program, "C").members.at("pick").front());
	}
	expectError("typedef<interface> A { pub mt<int> read(int a); }\ntypedef<interface> B { pub mt<int> read(int b); }\ntypedef<interface> C : A, B {}",
	            Code::IncompatibleInterfaceRequirement);
	expectError("typedef<interface> A { pub mt<int> read() = default (1); }\ntypedef<interface> B : A { pub mt<int> read(); }",
	            Code::IncompatibleInterfaceRequirement);
	expectError("typedef<interface> A { pub mt<int, bool> pair(); }\ntypedef<interface> B : A { pub mt<bool, int> pair(); }",
	            Code::IncompatibleInterfaceRequirement);
	expectError("typedef<interface> A<T> {}\ntypedef<interface> B : A<int> {}\ntypedef<class> C : B, A<bool> {}", Code::ConflictingInterfaceApplication);
}

TEST (SemanticDispatch, SelectsPublicInterfaceImplementationsDeclaredOrInheritedByTheClass) {
	const CheckedProgram cp_program   = checkSource(R"(typedef<interface> Readable { pub mt<int> read(); }
typedef<class> Direct : Readable { pub override mt<int> read() = 1; }
typedef<class> Base(int n) {
    pub Base() : (0) {}
    pub mt<int> read() = 2;
}
typedef<class> Child : Base(), Readable {}
fn<int> direct(Direct item) = item.read();
fn<int> inherited(Child item) = item.read();
)");
	const Symbol        &sym_contract = global(cp_program, "Readable").members.at("read").front();
	const TypeDispatch  &td_direct    = tableFor(cp_program, "direct");
	const TypeDispatch  &td_child     = tableFor(cp_program, "inherited");
	ASSERT_EQ(td_direct.interfaceTargets.size(), 1u);
	ASSERT_EQ(td_child.interfaceTargets.size(), 1u);
	EXPECT_EQ(td_direct.interfaceTargets.front().contract.symbol, &sym_contract);
	EXPECT_EQ(td_child.interfaceTargets.front().contract.symbol, &sym_contract);
	EXPECT_EQ(td_direct.interfaceTargets.front().implementation.symbol, &global(cp_program, "Direct").members.at("read").front());
	EXPECT_EQ(td_child.interfaceTargets.front().implementation.symbol, &global(cp_program, "Base").members.at("read").front());
}

TEST (SemanticDispatch, UsesAnInterfaceDefaultWhenTheInheritedMethodIsPrivate) {
	const CheckedProgram cp_fallback = checkSource(R"(typedef<interface> Readable { pub mt<int> read() = default (7); }
typedef<class> Base(int n) {
    pub Base() : (0) {}
    mt<int> read() = 1;
}
typedef<class> Child : Base(), Readable {}
fn<int> readChild(Child item) = item.read();
)");
	const TypeDispatch  &td_child    = tableFor(cp_fallback, "readChild");
	ASSERT_EQ(td_child.interfaceTargets.size(), 1u);
	const InterfaceTarget &it_read = td_child.interfaceTargets.front();
	EXPECT_EQ(it_read.contract.symbol, &global(cp_fallback, "Readable").members.at("read").front());
	EXPECT_EQ(it_read.implementation, it_read.contract);
}

TEST (SemanticDispatch, PreservesPrivateHelpersAndChecksSharedConstructorContracts) {
	EXPECT_NO_THROW(checkSource(R"(typedef<interface> I {
    mt<int> secret() = default (2);
    pub mt<int> visible() = default (self.secret());
}
typedef<class> C : I {}
fn<int> run(I i) = i.visible();
)"));
	expectError("typedef<interface> I { mt<int> secret() = default (2); }\nfn<int> leak(I i) = i.secret();", Code::PrivateClassMember);
	expectError("typedef<interface> I { pub mt<int> f(); }\ntypedef<interface> J : I { mt<int> f() = default (1); }",
	            Code::IncompatibleInterfaceRequirement);
	expectError(R"(typedef<interface> A { pub mt<T> make<T: new(int x)>(); }
typedef<interface> B { pub mt<U> make<U: new(int y)>(); }
typedef<class> C : A, B { pub override mt<V> make<V>() = V::new(1); }
)",
	            Code::IncompatibleInterfaceRequirement);
}

TEST (SemanticDispatch, PreservesTheRootVirtualContractAcrossThreeClassLevels) {
	const CheckedProgram cp_program = checkSource(R"(typedef<class> Leaf : Mid {}
typedef<class> Mid(int seed) : Base {
    pub Mid() : (0) {}
    pub override mt<int, bool> read(int renamed) = renamed, true;
}
typedef<class> Base(int seed) {
    pub Base() : (0) {}
    pub virtual mt<int, bool> read(opt<int> count ?: 2) = count, false;
}
fn<int, bool> base(Base item) = item.read();
fn<int, bool> leaf(Leaf item) = item.read(count=3);
)");
	const Symbol        &sym_base = global(cp_program, "Base").members.at("read").front(), &sym_mid = global(cp_program, "Mid").members.at("read").front();
	EXPECT_EQ(callIn(cp_program, "base").argumentContract, &sym_base);
	const ExpressionInfo &ei_leaf = callIn(cp_program, "leaf");
	ASSERT_TRUE(ei_leaf.dispatch);
	EXPECT_EQ(ei_leaf.dispatch->kind, DispatchKind::Virtual);
	EXPECT_EQ(ei_leaf.argumentContract, &sym_base);
	EXPECT_EQ(ei_leaf.symbol, &sym_mid);
	EXPECT_EQ(ei_leaf.types, (ResultTypes{ ResolvedType(KInteger), ResolvedType(KBoolean) }));
	const TypeDispatch &td_leaf = tableFor(cp_program, "leaf");
	ASSERT_EQ(td_leaf.methods.size(), 1u);
	ASSERT_TRUE(td_leaf.methods.front().implementation);
	EXPECT_EQ(td_leaf.methods.front().implementation->owner.declaration, &global(cp_program, "Mid"));
	EXPECT_EQ(td_leaf.methods.front().contract.owner.declaration, &global(cp_program, "Base"));
	const std::string s_base = "typedef<class> A { pub virtual mt<int, bool> go(int n) = n, true; }\ntypedef<class> B : A { ";
	for (const char *s_bad : { "pub mt<int, bool> go(int n) = n, true", "override mt<int, bool> go(int n) = n, true",
	                           "pub override mt<bool, int> go(int n) = true, n", "pub override mt<int, bool> go(opt<int> n ?: 7) = n, true" }) {
		expectError(s_base + s_bad + "; }", Code::InvalidVirtualMethod);
	}
	expectError(s_base + "pub override mt<int, bool> go(bool n) = 1, n; }", Code::InvalidInterfaceOverride);
	expectError("typedef<class> C { virtual mt f() {} }", Code::InvalidVirtualMethod);
}

TEST (SemanticDispatch, ResolvesStateContractsSelectionsAndOverloadsOnTheHost) {
	const CheckedProgram cp_program = checkSource(R"(typedef<class> Host(int seed) {
    pub Host() : (0) {}
    pub state<int, bool> ::score(opt<int> power ?: 1, opt<int> bonus ?: power);
    pub default state<void> Ready {
        mt<int, bool> score(int first, int second) = first + second, true;
        pub mt<bool> score(bool pass) = pass;
    }
    pub mt<int> score(int first, int second) = first - second;
}
typedef<class> Child(int seed) : Host {
    pub Child() : (0) {}
    pub default(1) state<int> Busy {
        mt<int, bool> score(int a, int b) = state, false;
        pub mt<bool> score(bool pass) = pass;
    }
}
typedef<class> Leaf : Child {}
fn<int, bool> omitted(Leaf item) = item::score();
fn<int, bool> named(Host item) = item::score(bonus=2, power=1);
fn<int> ordinary(Host item) = item.score(1, 2);
fn<bool> implicit(Leaf item) = item::score(true);
fn<bool> ready(Leaf item) = item is Ready;
)");
	const Symbol        &sym_host = global(cp_program, "Host"), &sym_ready = sym_host.members.at("Ready").front(),
	             &sym_busy        = global(cp_program, "Child").members.at("Busy").front();
	const ExpressionInfo &ei_call = callIn(cp_program, "omitted");
	ASSERT_TRUE(ei_call.dispatch);
	EXPECT_EQ(ei_call.dispatch->kind, DispatchKind::State);
	EXPECT_EQ(ei_call.argumentContract->kind, SymbolKind::StateMethodContract);
	EXPECT_EQ(ei_call.omittedParameters, (std::vector<std::size_t>{ 0, 1 }));
	EXPECT_EQ(ei_call.types, (ResultTypes{ ResolvedType(KInteger), ResolvedType(KBoolean) }));
	EXPECT_EQ(callIn(cp_program, "named").parameterForArgument, (std::vector<std::size_t>{ 1, 0 }));
	EXPECT_NE(callIn(cp_program, "ordinary").argumentContract, ei_call.argumentContract);
	EXPECT_EQ(callIn(cp_program, "ready").symbol, &sym_ready);
	const TypeDispatch &td_leaf = tableFor(cp_program, "omitted");
	EXPECT_EQ(td_leaf.initialState, &sym_busy);
	EXPECT_EQ(td_leaf.states.at("Busy").payload, ResolvedType(KInteger));
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
	const std::string s_host = "typedef<class> C { pub default state<void> Ready {}\n pub state<int> Full {} }\ntypedef<class> Other {}\n";
	for (const char *s_expr : { "item -> Full", "item -> Full(false)", "item -> Ready(1)" })
		expectError(s_host + "fn move(C item) { " + s_expr + "; }", Code::InvalidStatePayload);
	for (const char *s_expr : { "other is Ready", "other -> Ready()", "other::unknown()" })
		expectError(s_host + "fn test(Other other) { " + s_expr + "; }", Code::InvalidStateTarget);
	expectError(s_host + "fn bad(C? item) { item::read(); }", Code::NullablePayloadAccess);
	for (const char *s_state : { "default(false) state<int> Ready {}", "default(1) state<void> Ready {}", "default state<int> Ready {}" })
		expectError("typedef<class> C { " + std::string(s_state) + " }", Code::InvalidStatePayload);
	expectError("typedef<class> C { default state<void> Ready { mt<int> read() = state; } }", Code::UnknownName);
	expectError("typedef<class> C { default state<void> A { pub mt<int> f(int x) = x; }\n state<void> B { pub mt<int> f(int y) = y; } }",
	            Code::InvalidStateContract);
	expectError("typedef<class> C { pub state<int> ::f();\n default state<void> A { mt<bool> f() = true; } }", Code::InvalidStateContract);
	expectError("typedef<class> C { default state<void> Ready { mt<int> f(opt<int> x ?: 1) = x; } }", Code::InvalidStateContract);
	expectError("typedef<class> C { default state<void> A {}\n default state<void> B {} }", Code::InvalidStateDeclaration);
	expectError("typedef<state<void>> Bad for int {}", Code::InvalidStateAttachment);
}

TEST (SemanticDispatch, RequiresADefaultWhereStatesAreIntroduced) {
	expectError("typedef<class> Host { state<void> Ready {} }", Code::InvalidStateDeclaration);
	expectError("typedef<state<void>> Ready for Host {}\ntypedef<class> Host { state Ready; }", Code::InvalidStateDeclaration);
	const std::string s_parent = "typedef<class> Base(int seed) { pub Base() : (0) {}\n default state<void> Ready {} }\n";
	expectError(s_parent + "typedef<class> Child : Base { state<void> Busy {} }", Code::InvalidStateDeclaration);
	expectError(s_parent + "typedef<state<void>> Busy for Base {}\ntypedef<class> Child : Base { state Busy; }", Code::InvalidStateDeclaration);
	EXPECT_NO_THROW(checkSource(s_parent + "typedef<class> Child : Base {}"));
	EXPECT_NO_THROW(checkSource(s_parent + "typedef<state<void>> Busy for Base {}\ntypedef<class> Child : Base { default state Busy; }"));
}

TEST (SemanticDispatch, MaterializesGenericImplementationsSelectedThroughStaticContracts) {
	const CheckedProgram                                            cp_program = checkSource(R"(typedef<interface> I { pub mt<V> echo<V>(V label); }
typedef<class> Base(int seed) {
    pub Base() : (0) {}
    pub virtual mt<X> get<X>(X value) = value;
    pub state<T> ::read<T>(T item);
    default state<void> Ready { mt<U> read<U>(U renamed) = renamed; }
}
typedef<class> Child<A> : Base, I {
    pub override mt<B> echo<B>(B own) = own;
    pub override mt<Y> get<Y>(Y name) = name;
    default state<void> Busy { mt<W> read<W>(W other) = other; }
}
fn<bool> face(I item) = item.echo<bool>(label=true);
fn<int> virtualCall(Base item) = item.get<int>(value=1);
fn<string> stateCall(Base item) = item::read<string>(item="ok");
fn<Base> up(Child<int> item) = item;
)");
	const ResolvedType  &rt_child   = cp_program.declarations.at(&global(cp_program, "up")).parameters.front();
	const Symbol        &sym_child = global(cp_program, "Child"), &sym_busy = sym_child.members.at("Busy").front();
	const std::function<void(const Symbol &, const ResolvedType &)> check = [&] (const Symbol &body, const ResolvedType &argument) {
		const GenericInstance &gi_instance = cp_program.instances.at({ .declaration=&body, .receiver=rt_child, .arguments={ argument } });
		EXPECT_EQ(gi_instance.signature.returns, ResultTypes{ argument });
		EXPECT_EQ(gi_instance.body.expressions.at(&bodyExpression(body)).types, ResultTypes{ argument });
	};
	check(sym_child.members.at("echo").front(), ResolvedType(KBoolean));
	check(sym_child.members.at("get").front(), ResolvedType(KInteger));
	check(sym_busy.members.at("read").front(), ResolvedType(KString));
}

TEST (SemanticDispatch, ChecksAnAttachedBodyAgainstEachHostAndKeepsItsLexicalReceiver) {
	const std::string    s_decls    = R"(typedef<interface> Host { pub mt<int> id(); }
typedef<state<void>> Shared for Host {
    pub mt<bool> ready() = self is Ready;
    pub mt<int> idFromHost() = self.id();
}
typedef<class> A : Host {
    pub override mt<int> id() = 1;
    default state Shared;
    pub state<void> Ready {}
}
)";
	const CheckedProgram cp_program = checkSource(s_decls + R"(typedef<class> B<T> : Host {
    pub override mt<int> id() = 2;
    default state Shared;
    pub state<int> Ready {}
}
fn<bool> a(A item) = item::ready();
fn<bool> b(B<int> item) = item::ready();
)");
	const Symbol        &sym_shared = global(cp_program, "Shared"), &sym_query = sym_shared.members.at("ready").front(),
	             &sym_read   = sym_shared.members.at("idFromHost").front();
	const ResolvedType &rt_a = cp_program.declarations.at(&global(cp_program, "a")).parameters.front(),
	                   &rt_b = cp_program.declarations.at(&global(cp_program, "b")).parameters.front();
	const BodyFacts &bf_a    = cp_program.attachmentBodies.at(rt_a);
	EXPECT_EQ(bf_a.expressions.at(&bodyExpression(sym_query)).symbol, &global(cp_program, "A").members.at("Ready").front());
	EXPECT_EQ(bf_a.expressions.at(&bodyExpression(sym_read)).argumentContract, &global(cp_program, "Host").members.at("id").front());
	const GenericInstance &gi_b = cp_program.instances.at({ .declaration=&sym_query, .receiver=rt_b, .arguments={} });
	EXPECT_EQ(gi_b.body.expressions.at(&bodyExpression(sym_query)).symbol, &global(cp_program, "B").members.at("Ready").front());
	EXPECT_EQ(cp_program.dispatchTables.at(rt_b).states.at("Shared").definition, &sym_shared);
	expectError(s_decls + "typedef<class> Bad : Host { pub override mt<int> id() = 3; default state Shared; }", Code::InvalidStateTarget);
	expectError(
	        "typedef<class> Host { def<int> hidden = 1; default state Shared; }\ntypedef<state<void>> Shared for Host { pub mt<int> bad() = self.hidden; }",
	        Code::PrivateClassMember);
}

TEST (SemanticDispatch, ChecksEveryPublicSignatureIncludingBoundsAndInferredStateMethods) {
	const std::string s_types = "typedef<class> Secret {}\npub typedef<class> Box<T> {}\n";
	for (const char *s_decl : { "pub fn f<T: Secret>(T x) {}", "pub fn f<T: new(Box<Secret> value)>(T x) {}", "pub typedef<class> Api<T = Secret> {}",
	                            "pub typedef<interface> Api { pub mt<Box<Secret>[]> leak(); }", "pub typedef<class> Api { pub def<Secret?> leaked = null; }",
	                            "pub typedef<class> Api { default state<void> Hidden { pub mt<Secret> leak(Secret item) = item; } }" }) {
		expectError(s_types + s_decl, Code::PrivateGenericSignature);
	}
	EXPECT_NO_THROW(checkSource(s_types + "pub typedef<class> Api { mt<Secret> internal(Secret item) = item; }"));
}

TEST (SemanticDispatch, InheritsGenericMethodBoundsAndConstructorGuarantees) {
	const CheckedProgram   cp_program = checkSource(R"(typedef<interface> Label { pub mt<int> read(); }
typedef<class> Value : Label { pub override mt<int> read() = 5; }
typedef<class> Base(int seed) {
    pub Base() : (0) {}
    pub virtual mt<int> describe<T: Label>(T value) = value.read();
    pub virtual mt<T> make<T: new(int count)>(int value) = T::new(count=value);
}
typedef<class> Child : Base {
    pub override mt<int> describe<U>(U item) = item.read();
    pub override mt<R> make<R>(int item) = R::new(count=item);
}
typedef<class> Product { pub Product(int renamed) : () {} }
fn<int> describe(Base item, Value value) = item.describe(value);
fn<Product> make(Base item) = item.make<Product>(1);
fn<Base> up(Child item) = item;
)");
	const ResolvedType    &rt_child   = cp_program.declarations.at(&global(cp_program, "up")).parameters.front();
	const ResolvedType    &rt_product = cp_program.declarations.at(&global(cp_program, "make")).returns.front();
	const Symbol          &sym_make   = global(cp_program, "Child").members.at("make").front();
	const GenericInstance &gi_make    = cp_program.instances.at({ .declaration=&sym_make, .receiver=rt_child, .arguments={ rt_product } });
	const ExpressionInfo  &ei_create  = gi_make.body.expressions.at(&bodyExpression(sym_make));
	EXPECT_EQ(ei_create.types, ResultTypes{ rt_product });
	ASSERT_NE(ei_create.argumentContract, nullptr);
	EXPECT_NE(ei_create.argumentContract, ei_create.symbol);
	EXPECT_EQ(cp_program.declarations.at(ei_create.argumentContract).owner, &global(cp_program, "Base").members.at("make").front());
}

}
}
