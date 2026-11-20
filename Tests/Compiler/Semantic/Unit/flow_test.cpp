// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <ast/decl/class_declaration.hpp>
#include <ast/expr/member_call_expression.hpp>
#include <ast/stmt/do_while_statement.hpp>
#include <ast/stmt/while_statement.hpp>

#include "checked_program_test.hpp"

namespace cloth::test {
namespace {

using Code                 = cde::DiagnosticCode;
const std::string S_ITEM   = "pub typedef<class> Item (pub def<int> value) { pub Item() : (1) {} }\n";
const std::string S_PLAYER = R"(typedef<class> Player {
    pub state<int> ::run(opt<int> n ?: 1)
    default: pub state<void> Ready { mt<int> run(int n) = n }
    pub state<void> Stopped {}
}
fn<int> restore(Player p) { p -> Ready; return 4 }
)";

const BlockBody &body (const Symbol &symbol) {
	return std::get<BlockBody>(std::get<const CallableSignature *>(symbol.declaration)->executableBody()->value);
}

const BlockBody &body (const CheckedProgram &program, const char *name) {
	return body(program.units.front()->get_globals().at(name).front());
}

const MemberCallExpression &firstDestructorCall (const Symbol &type) {
	const Symbol              &sym_destructor = type.members.at(std::string(to_string(oTilde)) + type.name.get_lexeme()).front();
	const ExpressionStatement &es_first       = dynamic_cast<const ExpressionStatement &>(*body(sym_destructor).statements.front());
	return dynamic_cast<const MemberCallExpression &>(*es_first.expression);
}

const Expression &returned (const StatementPtr &stmt) {
	return *dynamic_cast<const ReturnStatement &>(*stmt).value;
}

TEST (SemanticFlow, TracksDeletionThroughAliasesAndContinuingBranches) {
	for (const char *s_body :
	     { "def alias = item; item::delete; return alias.value", "if (flag) { item::delete } else { item::delete }; return item.value",
	       "if (flag) return 0; item::delete; return item.value", "def<Item?> alias = item; item::delete; alias::delete; return 0",
	       "def a, b = item; a::delete; return b.value", "def a, b = item, other; a, b = b, a; item::delete; return b.value",
	       "if (flag) { item::delete } else { item = other; item::delete }; return item.value",
	       "do { item::delete; break } while (true); return item.value", "do { item::delete; continue } while (false); return item.value" }) {
		expectError(S_ITEM + "fn<int> test(Item item, Item other, bool flag) { " + s_body + " }", Code::DELETED_CLASS_ACCESS);
	}
	const CheckedProgram cp_program = checkSource(S_ITEM + R"(fn<int> split(Item item, bool flag) {
    if (flag) item::delete
    return item.value
}
fn<int> fresh() {
    def item = Item::new()
    return item.value
}
fn<int> replace(Item item, Item other) {
    def alias = item
    item::delete
    alias = other
    return alias.value
}
)");
	EXPECT_TRUE(cp_program.expressions.at(&returned(body(cp_program, "split").statements.back())).receiverChecks.atAccess);
	EXPECT_FALSE(cp_program.expressions.at(&returned(body(cp_program, "fresh").statements.back())).receiverChecks.atAccess);
	EXPECT_TRUE(cp_program.expressions.at(&returned(body(cp_program, "replace").statements.back())).receiverChecks.atAccess);
}

TEST (SemanticFlow, RecordsDistinctLoopExitsAndPresenceAtTheTrailingTest) {
	const CheckedProgram    cp_program = checkSource(R"(fn<int> test(bool leave, bool? flag) {
    do {
        if (leave) { flag = false; break }
        flag = true
        continue
    } while (flag)
    if (flag) return 1
    return 2
}
fn<int> outer() {
    while (true) { do continue while (if (true) { break } else { false }) }
    return 0
}
fn<int> stops(bool again) { do return 3 while (again) }
)");
	const DoWhileStatement &dw_loop    = dynamic_cast<const DoWhileStatement &>(*body(cp_program, "test").statements.front());
	EXPECT_EQ(cp_program.expressions.at(dw_loop.condition.get()).types, ResultTypes{ ResolvedType(kBoolean) });
	const LoopJumpStatement &lj_continue = dynamic_cast<const LoopJumpStatement &>(*dw_loop.body.statements.back());
	EXPECT_EQ(cp_program.jumpTargets.at(&lj_continue), dw_loop.loopTarget.get());
	EXPECT_TRUE(cp_program.controlFlow.at(&lj_continue).continues.contains(dw_loop.loopTarget.get()));
	EXPECT_FALSE(cp_program.controlFlow.at(&lj_continue).fallsThrough);
	EXPECT_TRUE(cp_program.controlFlow.at(&dw_loop).fallsThrough);
	EXPECT_TRUE(cp_program.controlFlow.at(&dw_loop).breaks.empty());
	const WhileStatement &ws_outer = dynamic_cast<const WhileStatement &>(*body(cp_program, "outer").statements.front());
	const ControlFlow    &cf_inner = cp_program.controlFlow.at(ws_outer.body.statements.front().get());
	EXPECT_TRUE(cf_inner.breaks.contains(ws_outer.loopTarget.get()));
	const ControlFlow &cf_return = cp_program.controlFlow.at(body(cp_program, "stops").statements.front().get());
	EXPECT_TRUE(cf_return.canReturn);
	EXPECT_FALSE(cf_return.fallsThrough);
}

TEST (SemanticFlow, JoinsBreakContinueAndZeroTripPathsAtTheCorrectPoint) {
	for (const char *s_source : { "fn<int> test(int? v) { do { v = 1; continue; v = null } while (false); return v }",
	                              "fn<int> test(bool skip, int? v) { do { if (skip) { v = 2; break }; v = 1 } while (false); return v }",
	                              "fn<int> test(int? v) { do {} while (v == null); return v }",
	                              "fn<int> test(int? v) { if (v == null) return 0; do { def n = v + 1; v = null; break } while (true); return 0 }",
	                              "fn<int> test() { do { continue; break } while (!false) }",
	                              "fn<int> test() { while (true) { do break while (if (true) { break } else { false }) } }" }) {
		SCOPED_TRACE(s_source);
		EXPECT_NO_THROW(checkSource(s_source));
	}
	expectError("fn test(bool skip, bool? flag) { do { if (skip) continue; flag = true } while (flag) }", Code::NON_BOOLEAN_CONDITION);
	expectError("fn<int> f(int? v) { while (false) v = 2; return v }", Code::RETURN_TYPE);
	expectError("fn<int> f(bool skip, int? v) { do { if (skip) break; v = 1 } while (v == null); return v }", Code::RETURN_TYPE);
	expectError("fn f(int? v) { if (v != null) { do { def a = v+1; v = null } while (true) } }", Code::NULLABLE_PAYLOAD_ACCESS);
	expectError("fn<int> f() { while (true) { do continue while (if (true) { break } else { false }) } }", Code::MISSING_RETURN);
}

TEST (SemanticFlow, UsesBindingAndFieldIdentityAndInvalidatesResolvedWrites) {
	const std::string s_fields = "typedef<class> Box (pub def<int?> value) {}\nfn touch(Box b) { b.value = null }\n";
	EXPECT_NO_THROW(checkSource(s_fields + "fn<int> f(Box b) { if (b.value == null) return 0; return b.value + 1 }"));
	expectError(s_fields + "fn<int> f(Box b, Box alias) { if (b.value == null) return 0; alias.value = null; return b.value + 1 }",
	            Code::NULLABLE_PAYLOAD_ACCESS);
	expectError(s_fields + "fn<int> f(Box b) { if (b.value == null) return 0; touch(b); return b.value }", Code::RETURN_TYPE);
	EXPECT_NO_THROW(checkSource("fn f(bool flag, int? value) { if (value != null) { if (flag) { def<int?> value = null }; def v = value + 1 } }"));
	expectError("fn<int> f(int? value) { def<int> clear = { value = null; return 1 }; if (value != null) { clear; return value }; return 0 }",
	            Code::RETURN_TYPE);
	EXPECT_NO_THROW(checkSource("typedef<class> Base(int seed) { pub Base() : (0) {} }\ntypedef<class> Child : Base { pub mt<int> read() = 1 }\n"
	                            "fn<int> test(Base? value) { if (value is Child) return value.read(); return 0 }"));
}

TEST (SemanticFlow, JoinsInterfaceContractsAndKeepsTheDeclaredMetadataType) {
	const std::string    s_views    = R"(typedef<interface> Read<T> { pub mt<T> read() }
typedef<interface> Left : Read<int> {}
typedef<interface> Right : Read<int> {}
typedef<interface> Tag { pub mt<int> tag() }
)";
	const CheckedProgram cp_program = checkSource(s_views + R"(fn<int> test(Object? x) {
    if (x is Left || x is Right) {
        def descriptor = x::type
        return x.read()
    }
    return 0
})");
	const IfExpression  &ie_branch =
	        dynamic_cast<const IfExpression &>(*dynamic_cast<const ExpressionStatement &>(*body(cp_program, "test").statements.front()).expression);
	const std::vector<StatementPtr> &v_statements = ie_branch.branches.front().statements;
	const BindingDeclaration        &bd_metadata  = dynamic_cast<const BindingDeclaration &>(*v_statements.front());
	const SymbolScope               &ss_globals   = cp_program.units.front()->get_globals();
	const ResolvedType              &rt_declared  = cp_program.declarations.at(&ss_globals.at("test").front()).parameters.front();
	EXPECT_EQ(cp_program.expressions.at(bd_metadata.initializer.get()).representedType, rt_declared);
	const ExpressionInfo &ei_read = cp_program.expressions.at(&returned(v_statements.back()));
	ASSERT_TRUE(ei_read.dispatch);
	EXPECT_EQ(ei_read.dispatch->kind, DispatchKind::Interface);
	EXPECT_EQ(ei_read.dispatch->receiver.declaration, &ss_globals.at("Read").front());
	EXPECT_EQ(ei_read.dispatch->receiver.arguments(), ResultTypes{ ResolvedType(kInteger) });
	expectError(s_views + "fn f(Object? x) { if (x is Left || x is Tag) x.read() }", Code::UNKNOWN_MEMBER);
	expectError(s_views + "fn f(Object? x, Object? other) { if (x is Left || x is Right) { x = other; x.read() } }", Code::NULLABLE_PAYLOAD_ACCESS);
	EXPECT_NO_THROW(checkSource("typedef<class> C { pub operator ::type() -> int = 3 }\nfn<int> f(Object? x) { if (x is C) return x::type; return 0 }"));
	EXPECT_NO_THROW(checkSource(s_views + R"(typedef<class> Holder {
    pub state<int> ::run()
    default: pub state<void> Idle { mt<int> run() = 0 }
    pub state<Object?> Active { mt<int> run() { if (state is Left || state is Right) return state.read(); return 0 } }
})"));
}

TEST (SemanticFlow, KeepsIndexedFieldProofsOnlyForStableStorage) {
	const std::string s_cell  = "typedef<struct> Cell { pub int[]? rows, pub bool flag }\n";
	const std::string s_start = s_cell + "fn f(Cell[][] a, Cell cell, usize i) { if (a[0][0].rows != null) { ";
	const std::string s_read  = "; a[0][0].rows.append(1) } }";
	for (const char *s_write : { "a[0][1] = cell", "a[i][1] = cell", "a[0][i].flag = false", "a.reserve(8)" })
		EXPECT_NO_THROW(checkSource(s_start + s_write + s_read)) << s_write;
	for (const char *s_write : { "a[0][0] = cell", "a[i][0] = cell", "a[0][i].rows = null", "a.clear()" })
		expectError(s_start + s_write + s_read, Code::NULLABLE_PAYLOAD_ACCESS);
	expectError(s_cell + "fn f(Cell[] a, usize i) { if (a[i].rows != null) a[i].rows.append(1) }", Code::NULLABLE_PAYLOAD_ACCESS);
	expectError(s_cell + "fn f(Cell[] a) { a[0].rows = [1]; a[0].rows.append(1) }", Code::NULLABLE_PAYLOAD_ACCESS);
	expectError("fn f(int[]?[] a) { if (a[0] != null) a[0].append(1) }", Code::NULLABLE_PAYLOAD_ACCESS);
}

TEST (SemanticFlow, RevalidatesIndexedReceiversAcrossCallsAndResizes) {
	const std::string s_cell   = "typedef<struct> Cell { pub int[]? rows }\n";
	const std::string s_shared = s_cell + "typedef<class> Box(pub def<Cell[]> cells) {}\nfn f(Box a, Box b) { if (a.cells[0].rows != null) { ";
	const std::string s_read   = "; a.cells[0].rows.append(1) } }";
	EXPECT_NO_THROW(checkSource(s_shared + "b.cells.reserve(8)" + s_read));
	expectError(s_shared + "b.cells.clear()" + s_read, Code::NULLABLE_PAYLOAD_ACCESS);
	const std::string s_deferred = s_cell + "fn f(Cell[] a, Cell replacement, usize i) { def<int> change = { ";
	const std::string s_argument = " = replacement; return 1 }; if (a[0].rows != null) a[0].rows.append(change) }";
	EXPECT_NO_THROW(checkSource(s_deferred + "a[1]" + s_argument));
	expectError(s_deferred + "a[i]" + s_argument, Code::NULLABLE_PAYLOAD_ACCESS);
	const std::string s_getter = s_cell + "typedef<class> Bag { pub operator [](usize index) -> Cell = { null } }\n";
	expectError(s_getter + "fn f(Bag bag) { if (bag[0].rows != null) bag[0].rows.append(1) }", Code::NULLABLE_PAYLOAD_ACCESS);
	const std::string s_struct = s_cell + R"(fn ping() {}
typedef<struct> Bag { pub Cell[] cells;
    mt run() { if (cells[0].rows != null) { ping(); cells[0].rows.append(1) } }
})";
	EXPECT_NO_THROW(checkSource(s_struct));
	expectError(s_struct + "\ntypedef<class> Box {}", Code::NULLABLE_PAYLOAD_ACCESS);
}

TEST (SemanticFlow, CarriesPresenceThroughGuardsAndExitingFallbacks) {
	const std::string s_reader = "typedef<class> Reader (pub def<int> n) { pub mt<int> take(int x) = x }\n";
	EXPECT_NO_THROW(checkSource(s_reader + "fn<int?> f(Reader? r) = r?.take(r.n)"));
	expectError(s_reader + "fn<int> f(Reader? r) { def<int?> n = r?.take(r.n); return r.n }", Code::NULLABLE_PAYLOAD_ACCESS);
	EXPECT_NO_THROW(checkSource("fn<int> f(int? v) { def n = v ?: if (true) { return 0 } else { 1 }; return v }"));
	expectError("fn<int> f(int? v) { def n = v ?: 0; return v }", Code::RETURN_TYPE);
}

TEST (SemanticFlow, InfersBindingsOnlyWhenTheirTypesHaveKnownNullability) {
	expectError("fn f(int? item) { def copy = item }", Code::EXPLICIT_NULLABLE_TYPE);
	expectError("fn f(int? item) { def first, second = item }", Code::EXPLICIT_NULLABLE_TYPE);
	expectError("fn f(int?[] items) { for (def item <- items) {} }", Code::EXPLICIT_NULLABLE_TYPE);
	expectError("fn<T[]> f<T>(T[] items) { def copy = items; return copy }", Code::EXPLICIT_NULLABLE_TYPE);
	EXPECT_NO_THROW(checkSource("fn<int> f(int? item) { if (item == null) return 0; def copy = item; return copy }"));
	EXPECT_NO_THROW(checkSource("typedef<class> Base {}\nfn<T> f<T:Base>(T source) { def copy = source; return copy }"));
}

TEST (SemanticFlow, ResolvesStateDispatchAfterItsOperandsAndDefaults) {
	for (const char *s_body :
	     { "if (p is Stopped) return p::run(); return 0", "if (!(p is Stopped)) return 0; def alias = p; return alias::run(1)",
	       "if (flag) { p -> Stopped } else { p -> Stopped }; return p::run(1)",
	       "if (p is Stopped) return p::run(if (flag) { p = other; 1 } else { 2 }); return 0", "while (p is Stopped) { return p::run() }; return 0" }) {
		expectError(S_PLAYER + "fn<int> test(Player p, Player other, bool flag) { " + s_body + " }", Code::INVALID_STATE_DISPATCH);
	}
	const CheckedProgram cp_program = checkSource(S_PLAYER + R"(fn<int> exact(Player p) {
    p -> Ready
    return p::run()
}
fn<int> changed(Player p) {
    p -> Stopped
    return p::run(if (true) { p -> Ready; 1 } else { 2 })
}
fn<int> unknown(Player p) {
    p -> Stopped
    return p::run(restore(p))
}
)");
	const Symbol        *sym_ready  = &cp_program.units.front()->get_globals().at("Player").front().members.at("Ready").front();
	for (const char *s_name : { "exact", "changed" }) {
		const ExpressionInfo &ei = cp_program.expressions.at(&returned(body(cp_program, s_name).statements.back()));
		EXPECT_EQ(ei.receiverChecks.knownState, sym_ready);
		EXPECT_FALSE(ei.receiverChecks.activeState);
	}
	const ExpressionInfo &ei_unknown = cp_program.expressions.at(&returned(body(cp_program, "unknown").statements.back()));
	EXPECT_TRUE(ei_unknown.receiverChecks.activeState);
	EXPECT_TRUE(ei_unknown.receiverChecks.atAccess);
	EXPECT_NO_THROW(checkSource(R"(typedef<class> C {
    pub state<int> ::run(C item, opt<int> n ?: restore(item))
    default: pub state<void> Ready { mt<int> run(C item, int n) = n }
    pub state<void> Other {}
}
fn<int> restore(C c) { c -> Ready; return 1 }
fn<int> test(C c) { c -> Other; return c::run(c) }
)"));
}

TEST (SemanticFlow, CapturesReceiversAndRestrictsDestructorPrivileges) {
	const std::string s_class = "typedef<class> C (pub def<int> n) { pub C() : (1) {}\n pub mt<int> read(int x) = x }\n";
	expectError(s_class + "fn<int> f(C c) = c.read(if (true) { c::delete; 1 } else { 2 })", Code::DELETED_CLASS_ACCESS);
	expectError(s_class + "fn f(C c) { c.n = if (true) { c::delete; 1 } else { 2 } }", Code::DELETED_CLASS_ACCESS);
	const CheckedProgram cp_program =
	        checkSource(s_class + "fn<int> destroy(C c) { c::delete; return 1 }\nfn<int> f() { def c = C::new(); return c.read(destroy(c)) }");
	const ReceiverChecks &checks = cp_program.expressions.at(&returned(body(cp_program, "f").statements.back())).receiverChecks;
	EXPECT_FALSE(checks.beforeArguments);
	EXPECT_TRUE(checks.atAccess);
	EXPECT_NO_THROW(checkSource("typedef<class> C (pub def<int> n) { ~C { def copy = self.n } }"));
	expectError("typedef<class> C { ~C { self::delete } }", Code::DELETED_CLASS_ACCESS);
	expectError("typedef<class> C (pub def<int> n) { ~C { def alias = self; def copy = alias.n } }", Code::DELETED_CLASS_ACCESS);
	EXPECT_NO_THROW(checkSource("fn f() { def<C?> empty = null; empty::delete; empty::delete }\ntypedef<class> C {}"));
}

TEST (SemanticFlow, KeepsDestructorReceiverAccessThroughImmediateInterfaceCasts) {
	const std::string           s_views    = R"(typedef<interface> Read {
    pub mt<int> read()
    pub mt<int> report() = default (self.read())
}
typedef<interface> More : Read {}
fn<int> throughAlias(Read view) = view.report()
typedef<class> Item : More {
    override: pub mt<int> read() = 7
    ~Item { )";
	const CheckedProgram        cp_program = checkSource(s_views + "((self -> More) -> Read).report(); throughAlias(self) } }");
	const SymbolScope          &ss_globals = cp_program.units.front()->get_globals();
	const Symbol               &sym_report = ss_globals.at("Read").front().members.at("report").front();
	const MemberCallExpression &mc_call    = firstDestructorCall(ss_globals.at("Item").front());
	const ExpressionInfo       &ei_call    = cp_program.expressions.at(&mc_call);
	ASSERT_TRUE(ei_call.dispatch);
	EXPECT_EQ(ei_call.dispatch->kind, DispatchKind::Interface);
	EXPECT_EQ(ei_call.dispatch->contract.symbol, &sym_report);
	EXPECT_TRUE(cp_program.isDirectReceiver(*mc_call.receiver));
	EXPECT_FALSE(ei_call.receiverChecks.beforeArguments);
	EXPECT_FALSE(ei_call.receiverChecks.atAccess);

	const MemberCallExpression &mc_default = dynamic_cast<const MemberCallExpression &>(bodyExpression(sym_report));
	EXPECT_TRUE(cp_program.isDirectReceiver(*mc_default.receiver));
	const MemberCallExpression &mc_alias = dynamic_cast<const MemberCallExpression &>(bodyExpression(ss_globals.at("throughAlias").front()));
	EXPECT_FALSE(cp_program.isDirectReceiver(*mc_alias.receiver));
	EXPECT_TRUE(cp_program.expressions.at(&mc_alias).receiverChecks.beforeArguments);
	EXPECT_TRUE(cp_program.expressions.at(&mc_alias).receiverChecks.atAccess);

	for (const char *s_use : { "def alias = self -> More; (alias -> Read).report()", "(self -> Read)::delete", "def alias = self -> Read; println(alias)" })
		expectError(s_views + s_use + " } }", Code::DELETED_CLASS_ACCESS);
	EXPECT_NO_THROW(checkSource(s_views + "println((self -> Read)::str) } }"));
}

TEST (SemanticFlow, ChecksTheObjectReturnedByACastOperatorDuringDestruction) {
	const CheckedProgram        cp_program = checkSource(R"(typedef<interface> Read { pub mt<int> read() }
typedef<class> Proxy(pub def<Read> target) {
    pub operator<Read> ->() = target
    ~Proxy { (self -> Read).read() }
})");
	const MemberCallExpression &mc_call    = firstDestructorCall(cp_program.units.front()->get_globals().at("Proxy").front());
	ASSERT_NE(cp_program.expressions.at(mc_call.receiver.get()).argumentContract, nullptr);
	EXPECT_FALSE(cp_program.isDirectReceiver(*mc_call.receiver));
	const ReceiverChecks &rc_result = cp_program.expressions.at(&mc_call).receiverChecks;
	EXPECT_TRUE(rc_result.beforeArguments);
	EXPECT_TRUE(rc_result.atAccess);
}

TEST (SemanticFlow, PreservesIndependentStorageAndChecksTheCapturedOwner) {
	const std::string s_struct = R"(typedef<struct> Cell {
    pub int? value;
    pub mt clear() { value = null }
}
typedef<class> Box (pub def<Cell> cell)
)";
	EXPECT_NO_THROW(checkSource(s_struct + "fn<int> f(Cell a, Cell b) { if (a.value == null) return 0; b.value = null; return a.value }"));
	expectError(s_struct + "fn<int> f(Cell a) { if (a.value == null) return 0; a.clear(); return a.value }", Code::RETURN_TYPE);
	expectError(s_struct + "fn f(Box a) { a.cell.value = if (true) { a::delete; 1 } else { 2 } }", Code::DELETED_CLASS_ACCESS);
	expectError(s_struct + "fn<int> f(Box a, Box b) { a.cell.value = if (true) { a = b; 1 } else { 2 }; return a.cell.value }", Code::RETURN_TYPE);
	const std::string s_counter = "typedef<class> Counter { pub operator +=(int n) {} }\n";
	expectError(s_counter + "fn f(Counter a) { a += if (true) { a::delete; 1 } else { 2 } }", Code::DELETED_CLASS_ACCESS);
	expectError(s_counter + "fn f(Counter a) { a::delete; a += 1 }", Code::DELETED_CLASS_ACCESS);
}

TEST (SemanticFlow, RequiresStoredArraysToStayPresentThroughMutationArguments) {
	const std::string s_setup = "fn f() { def<int[]?> items = [1]; def<int> clear = { items = null; return 0 }; if (items != null) ";
	for (const char *s_call : { "items.append(clear)", "items.insert(0, clear)", "items.removeAt(clear -> usize)", "items.reserve(clear -> usize)" })
		expectError(s_setup + s_call + " }", Code::NULLABLE_PAYLOAD_ACCESS);
	expectError("typedef<class> Box { pub def<int[]?> items = [1] }\nfn f(Box box, Box alias) { def<int> arg = { alias.items = null; return 1 }; "
	            "if (box.items != null) box.items.append(arg) }",
	            Code::NULLABLE_PAYLOAD_ACCESS);
	expectError("typedef<struct> Row { pub int[] items }\nfn f(Row? row) { def<int> arg = { row = null; return 1 }; "
	            "if (row != null) row.items.append(arg) }",
	            Code::NULLABLE_PAYLOAD_ACCESS);
	EXPECT_NO_THROW(checkSource(s_setup + "items.append(2) }"));
	EXPECT_NO_THROW(checkSource("fn f() { def<int[]?> a = [1]; def<int[]?> b = [2]; def<int> arg = { b = null; return 1 }; "
	                            "if (a != null) a.append(arg) }"));
	EXPECT_NO_THROW(checkSource("fn f(int[]? a) { if (a != null) a.append(if (true) { return } else { 1 }) }"));
	expectError("typedef<struct> Cell { pub mt set(int n) {} }\nfn f(Cell? cell) { def<int> arg = { cell = null; return 1 }; "
	            "if (cell != null) cell.set(arg) }",
	            Code::NULLABLE_PAYLOAD_ACCESS);
	EXPECT_NO_THROW(checkSource("typedef<class> Cell { pub mt set(int n) {} }\nfn f(Cell? cell) { def<int> arg = { cell = null; return 1 }; "
	                            "if (cell != null) cell.set(arg) }"));
}

TEST (SemanticFlow, InvalidatesSharedPresenceWhenArrayElementEqualityCanCallUserCode) {
	const std::string s_box = R"(typedef<class> Box { pub def<string?> name = "live" }
typedef<struct> Probe { Box box; pub operator ==(Probe other) { self.box.name = null; return true } }
typedef<class> Base(int seed) { pub Base() : (0) {} }
typedef<class> Child : Base { pub operator ==(Object other) = true }
typedef<interface> View {}
typedef<class> Viewed : View { pub operator ==(Object other) = true }
typedef<class> Plain {}
)";
	for (const char *s_array : { "Probe[]", "Probe?[][]", "Base[]", "View[]" }) {
		for (const char *s_op : { " == ", " != " })
			expectError(s_box + "fn<usize> f(Box box, " + s_array + " a) { if (box.name != null && a" + s_op +
			                    "a) return box.name::size; return 0 }",
			            Code::NULLABLE_PAYLOAD_ACCESS);
	}
	const CheckedProgram cp_program = checkSource(s_box + R"(fn<bool> compared(Probe[] values) = values == values
fn<bool> primitive(int[][] values) = values != values
fn<usize> use(Box box, int[][] values) { if (box.name != null && values == values) return box.name::size; return 0 }
fn<usize> local(string? name, Probe[] values) { if (name != null && values == values) return name::size; return 0 }
fn<usize> bounded<T: Plain>(Box box, T[] values) { if (box.name != null && values == values) return box.name::size; return 0 }
)");
	const SymbolScope   &ss_globals = cp_program.units.front()->get_globals();
	EXPECT_TRUE(cp_program.expressions.at(&bodyExpression(ss_globals.at("compared").front())).callsElementEquality);
	EXPECT_FALSE(cp_program.expressions.at(&bodyExpression(ss_globals.at("primitive").front())).callsElementEquality);
}

TEST (SemanticFlow, CarriesFactsIntoSpecializationsAndSkipsExitedOperands) {
	const std::string s_interface = R"(typedef<interface> Reader { pub mt<int> read() }
typedef<class> Box : Reader { pub mt<int> read() = 1 }
)";
	expectError(s_interface + "fn<int> f(Box b) { def<Reader> alias = b; b::delete; return alias.read() }", Code::DELETED_CLASS_ACCESS);
	const CheckedProgram cp_program  = checkSource(s_interface + R"(fn<int> maybe<T:Reader>(T value, bool drop) {
    if (drop) value::delete
    return value.read()
}
fn<int> closed(Box box) = maybe<Box>(box, false)
)");
	const Symbol        &sym_generic = cp_program.units.front()->get_globals().at("maybe").front();
	const ResolvedType   rt_box      = cp_program.declarations.at(&cp_program.units.front()->get_globals().at("closed").front()).parameters.front();
	const BodyFacts     &bf_closed   = cp_program.instances.at({ &sym_generic, std::nullopt, { rt_box } }).body;
	const StatementPtr  &sp_branch   = body(cp_program, "maybe").statements.front();
	EXPECT_TRUE(bf_closed.controlFlow.at(sp_branch.get()).fallsThrough);
	const Expression &e_call = returned(body(cp_program, "maybe").statements.back());
	EXPECT_TRUE(bf_closed.expressions.at(&e_call).receiverChecks.atAccess);
	EXPECT_NO_THROW(checkSource(S_ITEM + "fn<bool> f(Item a) { a::delete; return a === a }"));
	expectError(S_ITEM + "fn<bool> f(Item a) { a::delete; return a == a }", Code::DELETED_CLASS_ACCESS);
	EXPECT_NO_THROW(checkSource(S_ITEM + "fn<int> f(Item a) { a::delete; def<int?> n = 1; return n ?: a.value }"));
	EXPECT_NO_THROW(checkSource(S_ITEM + "fn<int> f(Item? a) { if (a == null) { a::delete; return 0 }; return a.value }"));
	EXPECT_NO_THROW(checkSource("fn<int> f() { while (true) { def x = (if (true) { return 1 } else { 0 }) + (if (true) { break } else { 0 }) } }"));
	const CheckedProgram cp_guarded =
	        checkSource("typedef<class> C(int n) { pub C() : (0) { self::delete }\n pub mt<int> n() = 1 }\nfn<int> f() = C::new().n()");
	const Symbol &sym_f = cp_guarded.units.front()->get_globals().at("f").front();
	EXPECT_TRUE(cp_guarded.expressions.at(&bodyExpression(sym_f)).receiverChecks.atAccess);
}

}
}
