// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "checked_program_test.hpp"

#include <ast/decl/class_declaration.hpp>
#include <ast/stmt/statements.hpp>

namespace cloth::test {
namespace {

TEST (SemanticChecking, RetainsWrittenArgumentsAndDeclarationSiteDefaults) {
	const CheckedProgram     cp_program = checkSource(R"(fn<int, bool> use(int helper) = pick(ok = true, n = helper)
fn<int> helper(int n) = n
fn<int, bool> pick(opt<int> start ?: 2, int n, opt<int> extra ?: helper(n), bool ok) = extra, ok
fn<int, bool> positional() = pick(4, false)
fn<int?> maybe(opt<int?> n ?: 7) = n
fn<int?> supplied() = maybe(null)
fn<int?> omitted() = maybe()
)");
	const SymbolScope       &ss_globals = cp_program.units.front()->get_globals();
	const Symbol            &sym_pick   = ss_globals.at("pick").front();
	const CallableSignature &cs_pick    = *std::get<const CallableSignature *>(sym_pick.declaration);
	const CallExpression    &ce_call    = dynamic_cast<const CallExpression &>(bodyExpression(ss_globals.at("use").front()));
	const ExpressionInfo    &ei_call    = cp_program.expressions.at(&ce_call);
	EXPECT_EQ(ei_call.symbol, &sym_pick);
	ASSERT_EQ(ei_call.argumentContract, &sym_pick);
	EXPECT_EQ(ei_call.parameterForArgument, (std::vector<std::size_t>{ 3, 1 }));
	EXPECT_EQ(ei_call.omittedParameters, (std::vector<std::size_t>{ 0, 2 }));
	EXPECT_EQ(ei_call.types, (ResultTypes{ ResolvedType(kInteger), ResolvedType(kBoolean) }));
	const CallableSignature &cs_use = *std::get<const CallableSignature *>(ss_globals.at("use").front().declaration);
	EXPECT_EQ(cp_program.expressions.at(ce_call.suppliedArguments[1].expression.get()).binding, &cs_use.parameters.front().name);
	const CallExpression &ce_default = dynamic_cast<const CallExpression &>(*cs_pick.parameters[2].defaultValue);
	EXPECT_EQ(cp_program.expressions.at(&ce_default).symbol, &ss_globals.at("helper").front());
	EXPECT_EQ(cp_program.expressions.at(ce_default.suppliedArguments.front().expression.get()).binding, &cs_pick.parameters[1].name);
	const ExpressionInfo &ei_positional = cp_program.expressions.at(&bodyExpression(ss_globals.at("positional").front()));
	EXPECT_EQ(ei_positional.parameterForArgument, (std::vector<std::size_t>{ 1, 3 }));
	EXPECT_EQ(ei_positional.omittedParameters, ei_call.omittedParameters);
	EXPECT_TRUE(cp_program.expressions.at(&bodyExpression(ss_globals.at("supplied").front())).omittedParameters.empty());
	EXPECT_EQ(cp_program.expressions.at(&bodyExpression(ss_globals.at("omitted").front())).omittedParameters, (std::vector<std::size_t>{ 0 }));
}

TEST (SemanticChecking, UsesTheStaticParameterContractForMethodsAndConstruction) {
	const CheckedProgram  cp_program = checkSource(R"(typedef<class> Base {
    pub Base(opt<int> n ?: 1) : () {}
    virtual: pub mt<int> read(opt<int> count ?: 2) = count
}
typedef<class> Child : Base() {
    override: pub mt<int> read(int renamed) = renamed
}
fn<int> derived(Child child) = child.read()
fn<int> labeled(Child child) = child.read(count = 7)
fn<int> base(Base item) = item.read()
)");
	const SymbolScope    &ss_globals = cp_program.units.front()->get_globals();
	const Symbol         &sym_base = ss_globals.at("Base").front(), &sym_child = ss_globals.at("Child").front();
	const Symbol         &sym_contract = sym_base.members.at("read").front(), &sym_override = sym_child.members.at("read").front();
	const ExpressionInfo &ei_derived = cp_program.expressions.at(&bodyExpression(ss_globals.at("derived").front()));
	EXPECT_EQ(ei_derived.symbol, &sym_override);
	EXPECT_EQ(ei_derived.argumentContract, &sym_contract);
	EXPECT_EQ(ei_derived.omittedParameters, (std::vector<std::size_t>{ 0 }));
	const ExpressionInfo &ei_labeled = cp_program.expressions.at(&bodyExpression(ss_globals.at("labeled").front()));
	EXPECT_EQ(ei_labeled.argumentContract, &sym_contract);
	EXPECT_EQ(ei_labeled.parameterForArgument, (std::vector<std::size_t>{ 0 }));
	EXPECT_TRUE(ei_labeled.omittedParameters.empty());
	EXPECT_EQ(cp_program.expressions.at(&bodyExpression(ss_globals.at("base").front())).symbol, &sym_contract);
	const ClassDeclaration &cd_child  = dynamic_cast<const ClassDeclaration &>(*std::get<const TypeDeclaration *>(sym_child.declaration));
	const ExpressionInfo   &ei_parent = cp_program.parentConstructors.at(&cd_child.parents.front());
	ASSERT_NE(ei_parent.argumentContract, nullptr);
	EXPECT_EQ(ei_parent.symbol, ei_parent.argumentContract);
	EXPECT_EQ(ei_parent.omittedParameters, (std::vector<std::size_t>{ 0 }));
	EXPECT_EQ(cp_program.declarations.at(ei_parent.argumentContract).owner, &sym_base);
}

TEST (SemanticChecking, ChecksDefaultsEvenWhenEveryCallSuppliesTheArgument) {
	using Code = cde::DiagnosticCode;
	expectError("fn f(opt<int> n ?: n) {}\nfn test() = f(2)", Code::INVALID_PARAMETER_DEFAULT);
	expectError("fn f(opt<int> n ?: later, int later) {}", Code::INVALID_PARAMETER_DEFAULT);
	expectError("fn f(opt<int> n ?: caller) {}\nfn test(int caller) = f()", Code::UNKNOWN_NAME);
	expectError("fn f(opt<int> n ?: if (true) { return 1 } else { 2 }) {}", Code::INVALID_PARAMETER_DEFAULT);
	expectError("fn f(int[] n, opt<int> x ?: if (true) { n[0] = 1\n n[0] } else { 0 }) {}", Code::INVALID_PARAMETER_DEFAULT);
	expectError("fn f(int n, opt<int> x ?: if (true) { def<int> edit = { n++\n return n }\n edit } else { 0 }) {}", Code::INVALID_PARAMETER_DEFAULT);
	EXPECT_NO_THROW(checkSource("fn f(int n, opt<int> x ?: if (true) { def<int> read = { return n }\n read } else { 0 }) { n++ }"));
	expectError("fn f(opt<int> n ?: 1, int required) {}\nfn use() = f()", Code::INVALID_ARGUMENT_BINDING);
	expectError("fn<int> f(int<8> x) = 1\nfn<int, bool> f(int<64> x, opt<bool> flag ?: true) = 2, flag\nfn<int> use() = f(1)", Code::AMBIGUOUS_CALL);
}

TEST (SemanticChecking, BindsOmittedBaseArgumentsInOpenAndClosedClasses) {
	const CheckedProgram     cp_program = checkSource(R"(typedef<interface> View {}
typedef<class> Base<T> { pub Base(opt<int> tag ?: 1) : () {} }
typedef<class> Child<T> : View, Base<T> {}
typedef<class> Explicit : Base<int>() {}
fn use(Child<int> child) {}
)");
	const SymbolScope       &ss_globals = cp_program.units.front()->get_globals();
	const Symbol            &sym_child = ss_globals.at("Child").front(), &sym_explicit = ss_globals.at("Explicit").front();
	const ClassDeclaration  &cd_child    = dynamic_cast<const ClassDeclaration &>(*std::get<const TypeDeclaration *>(sym_child.declaration));
	const ClassDeclaration  &cd_explicit = dynamic_cast<const ClassDeclaration &>(*std::get<const TypeDeclaration *>(sym_explicit.declaration));
	const ClassParentClause &cp_base     = cd_child.parents.at(1);
	const ExpressionInfo    &ei_open     = cp_program.parentConstructors.at(&cp_base);
	const ExpressionInfo    &ei_explicit = cp_program.parentConstructors.at(&cd_explicit.parents.front());
	const GenericInstance   &gi_child    = cp_program.instances.at({ &sym_child, std::nullopt, { ResolvedType(kInteger) } });
	const ExpressionInfo    &ei_closed   = gi_child.body.parentConstructors.at(&cp_base);
	EXPECT_FALSE(cp_program.parentConstructors.contains(&cd_child.parents.front()));
	EXPECT_EQ(ei_open.symbol, ei_explicit.symbol);
	EXPECT_EQ(ei_open.argumentContract, ei_open.symbol);
	EXPECT_EQ(ei_open.omittedParameters, (std::vector<std::size_t>{ 0 }));
	EXPECT_TRUE(ei_open.parameterForArgument.empty());
	ASSERT_NE(ei_closed.instance, nullptr);
	EXPECT_EQ(ei_closed.instance, ei_explicit.instance);
	EXPECT_EQ(ei_closed.omittedParameters, ei_open.omittedParameters);

	using Code = cde::DiagnosticCode;
	for (const std::string s_base : { "Base", "Base()" }) {
		expectError("typedef<class> Base(int required)\ntypedef<class> Child : " + s_base, Code::CALL_ARITY);
		expectError("typedef<class> Base {}\ntypedef<class> Child : " + s_base, Code::PRIVATE_CLASS_MEMBER);
		expectError("typedef<class> Base { pub Base(opt<int> x ?: 1) : () {}\n pub Base(opt<bool> x ?: true) : () {} }\n"
		            "typedef<class> Child : " +
		                    s_base,
		            Code::AMBIGUOUS_CALL);
	}
	expectError("typedef<interface> View {}\ntypedef<class> Child : View()", Code::INVALID_INTERFACE_HEADER);
}

TEST (SemanticChecking, DistinguishesOrderedResultsEmptyAndNoncontinuingExpressions) {
	const CheckedProgram  cp_program = checkSource(R"(fn<int<8>, bool> choose(bool flag) = if (flag) { return 1, true } else { 2, false }
fn<int<8>, bool> exit(bool flag) = if (flag) { return choose(flag) } else { return 3, true }
fn<int<8>, bool> list(bool flag) = if (flag) { return 1, true } else { return 2, false }, false
fn noop() {}
fn empty() = noop()
fn<int> single() = 1
fn<int<8>, bool> receive(bool flag) {
    def n, ok = choose(flag)
    n, ok = if (flag) { return n, ok } else { 4, false }
    return n, ok
}
)");
	const SymbolScope    &ss_globals = cp_program.units.front()->get_globals();
	const IfExpression   &ie_choose  = dynamic_cast<const IfExpression &>(bodyExpression(ss_globals.at("choose").front()));
	const ResultTypes    &v_expected = cp_program.declarations.at(&ss_globals.at("choose").front()).returns;
	const ExpressionInfo &ei_choose  = cp_program.expressions.at(&ie_choose);
	ASSERT_EQ(ei_choose.resultKind(), ResultKind::Multiple);
	EXPECT_EQ(ei_choose.types, v_expected);
	ASSERT_EQ(v_expected.size(), 2u);
	EXPECT_EQ(v_expected[0].spelling(), "int<8>");
	EXPECT_EQ(v_expected[1], ResolvedType(kBoolean));
	EXPECT_EQ(cp_program.expressions.at(&bodyExpression(ss_globals.at("empty").front())).resultKind(), ResultKind::Empty);
	EXPECT_EQ(cp_program.expressions.at(&bodyExpression(ss_globals.at("single").front())).resultKind(), ResultKind::Single);
	for (const std::string_view s_name : { "exit", "list" }) {
		const ExpressionInfo &ei_exit = cp_program.expressions.at(&bodyExpression(ss_globals.find(s_name)->second.front()));
		EXPECT_EQ(ei_exit.resultKind(), ResultKind::Never);
		EXPECT_TRUE(ei_exit.types.empty());
	}
	const CallableSignature       &cs_receive = *std::get<const CallableSignature *>(ss_globals.at("receive").front().declaration);
	const BlockBody               &bb_receive = std::get<BlockBody>(cs_receive.executableBody()->value);
	const BindingGroupDeclaration &bg_group   = dynamic_cast<const BindingGroupDeclaration &>(*bb_receive.statements[0]);
	EXPECT_EQ(cp_program.localTypes.at(&bg_group.targets[0].get_token()), v_expected[0]);
	EXPECT_EQ(cp_program.localTypes.at(&bg_group.targets[1].get_token()), v_expected[1]);
	const MultiAssignmentStatement &ma_assign = dynamic_cast<const MultiAssignmentStatement &>(*bb_receive.statements[1]);
	EXPECT_EQ(cp_program.expressions.at(ma_assign.value.get()).types, v_expected);
}

TEST (SemanticChecking, ChecksArityAndEveryResultSlotEvenAfterAnExit) {
	using Code = cde::DiagnosticCode;
	expectError("fn<int, bool> f() = 1", Code::RESULT_ARITY);
	expectError("fn<int, bool> f(bool b) = if (b) { 1, true } else { 2 }", Code::BRANCH_RESULT_ARITY);
	expectError("fn<int, bool> f() = if (true) { 1, true } else { 2, 3 }", Code::BRANCH_TYPE_MISMATCH);
	expectError("fn<int, bool> f() { return 1, 2 }", Code::RETURN_TYPE);
	expectError("fn<int, int<8>> f() = if (true) { return 1, 2 } else { return 3, 4 }, 128", Code::INTEGER_RANGE);
	expectError("fn f(bool b) = if (b) { 1, true } else { 2, false }", Code::VALUE_IN_VOID);
	expectError("fn noop() {}\nfn f() { def a, b = noop() }", Code::VOID_VALUE);
	expectError("fn<int, bool> pair() = 1, true\nfn f() { def x = pair() }", Code::MULTIPLE_RESULTS_IN_SCALAR);
	EXPECT_NO_THROW(checkSource("fn<int> f(bool flag) { def<int, int> a, b = if (flag) { return 1 } else { return 2 }\n return a + b }"));
	EXPECT_NO_THROW(checkSource("fn<int> f(bool flag, int x) { x = if (flag) { return 1 } else { return 2 } }"));
}

TEST (SemanticChecking, CommitsOnlyTheSelectedOverloadInWrittenOrder) {
	const CheckedProgram       cp_program = checkSource(R"(fn consume(int<8> n) {}
fn consume(int<64> n) {}
fn<int, bool> pair() = 1, true
fn<int, bool> run(bool flag) {
    consume(if (flag) { return pair() } else { 1000 })
    return pair()
}
)");
	const SymbolScope         &ss_globals = cp_program.units.front()->get_globals();
	const CallableSignature   &cs_run     = *std::get<const CallableSignature *>(ss_globals.at("run").front().declaration);
	const BlockBody           &bb_run     = std::get<BlockBody>(cs_run.executableBody()->value);
	const ExpressionStatement &es_call    = dynamic_cast<const ExpressionStatement &>(*bb_run.statements.front());
	const CallExpression      &ce_call    = dynamic_cast<const CallExpression &>(*es_call.expression);
	const ExpressionInfo      &ei_call    = cp_program.expressions.at(&ce_call);
	EXPECT_EQ(ei_call.symbol, &ss_globals.at("consume")[1]);
	const IfExpression &ie_arg = dynamic_cast<const IfExpression &>(*ce_call.suppliedArguments.front().expression);
	EXPECT_EQ(cp_program.expressions.at(&ie_arg).types.front().width, 64u);
	const ReturnStatement &rs_return    = dynamic_cast<const ReturnStatement &>(*ie_arg.branches.front().statements.front());
	const ExpressionInfo  &ei_forwarded = cp_program.expressions.at(rs_return.value.get());
	EXPECT_EQ(ei_forwarded.symbol, &ss_globals.at("pair").front());
	EXPECT_EQ(ei_forwarded.resultKind(), ResultKind::Multiple);
	using Code = cde::DiagnosticCode;
	expectError("fn use(int x, int<8> n) {}\nfn f(int? x) { if (x != null) { use(n = if (true) { x = null\n 1 } else { 2 }, x = x) } }",
	            Code::NO_MATCHING_CALL);
	expectError("fn use(int<8> x) {}\nfn use(int<64> x) {}\nfn<int<8>> f(bool b) { use(if (b) { return 128 } else { 1 })\n return 2 }",
	            Code::INTEGER_RANGE);
	std::string s_nested = "0";
	for (std::size_t st_level = 0; st_level < 24; ++st_level) s_nested = "sum(1, " + s_nested + ")";
	EXPECT_NO_THROW(checkSource("fn<int> sum(int<8> n, int x) = x\nfn<int> sum(int<8> n, bool flag) = 0\nfn<int> use() = " + s_nested));
}

TEST (SemanticChecking, KeepsGuardedAndExitedArgumentPathsDistinct) {
	const CheckedProgram           cp_program = checkSource(R"(typedef<class> Counter { pub mt<int> count(int n) = n }
fn<int?> read(Counter? value, bool flag) = value?.count(if (flag) { return 1 } else { return 2 })
)");
	const GuardedAccessExpression &ga_read =
	        dynamic_cast<const GuardedAccessExpression &>(bodyExpression(cp_program.units.front()->get_globals().at("read").front()));
	const ExpressionInfo &ei_access = cp_program.expressions.at(&ga_read);
	EXPECT_EQ(ei_access.resultKind(), ResultKind::Single);
	ASSERT_EQ(ei_access.types.size(), 1u);
	EXPECT_EQ(ei_access.types.front().spelling(), "int?");
	const ExpressionInfo &ei_present = cp_program.expressions.at(ga_read.continuation.get());
	EXPECT_EQ(ei_present.resultKind(), ResultKind::Never);
	EXPECT_TRUE(ei_present.types.empty());
	ASSERT_NE(ei_present.argumentContract, nullptr);
	EXPECT_EQ(ei_present.argumentContract->name.get_lexeme(), "count");
	using Code = cde::DiagnosticCode;
	expectError("typedef<class> C { pub mt<int, bool> pair() = 1, true }\nfn f(C? c) { c?.pair() }", Code::MULTIPLE_RESULTS_IN_SCALAR);
	expectError("fn sink(int x) {}\nfn<int> f(bool b) { while (true) { sink(if (b) { break } else { 3 }) } }", Code::MISSING_RETURN);
	EXPECT_NO_THROW(checkSource(
	        "fn sink(int a, int b) {}\nfn<int> f(bool b) { while (true) { sink(if (b) { return 1 } else { return 2 }, if (b) { break } else { 3 }) } }"));
	EXPECT_NO_THROW(checkSource(
	        "fn<int, bool> f(bool b) { while (true) { (if (b) { return 1, true } else { return 2, false }), if (b) { break } else { true } } }"));
}

TEST (SemanticChecking, RetainsSelectedOverloadAndLexicalBinding) {
	const CheckedProgram     program = checkSource(R"(fn<int> entry(int x) = choose(second = x, first = 1)
fn<bool> choose(bool first, bool second) = first
fn<int> choose(int first, int second) = entry(second)
)");
	const SymbolScope       &globals = program.units.front()->get_globals();
	const CallableSignature &entry   = *std::get<const CallableSignature *>(globals.at("entry")[0].declaration);
	const CallExpression    &call    = dynamic_cast<const CallExpression &>(*std::get<ExpressionBody>(entry.executableBody()->value).expression);
	const ExpressionInfo    &result  = program.expressions.at(&call);
	EXPECT_EQ(result.symbol, &globals.at("choose")[1]);
	EXPECT_EQ(result.types, ResultTypes{ ResolvedType(kInteger) });
	EXPECT_EQ(result.parameterForArgument, (std::vector<std::size_t>{ 1, 0 }));
	EXPECT_EQ(program.expressions.at(call.suppliedArguments[0].expression.get()).binding, &entry.parameters[0].name);
}

TEST (SemanticChecking, PreservesCompoundTypeSpellingsDuringResolution) {
	const CheckedProgram   cp_program = checkSource(R"(fn<unsigned int<8>> first(unsigned int<8>?[] values) = values[0] ?: 0
fn<unsigned int<8>> use() = first([42 -> unsigned int<8>, null])
)");
	const Symbol          &sym_first  = cp_program.units.front()->get_globals().at("first").front();
	const DeclarationInfo &di_first   = cp_program.declarations.at(&sym_first);
	ASSERT_EQ(di_first.parameters.size(), 1u);
	ASSERT_EQ(di_first.returns.size(), 1u);
	EXPECT_EQ(di_first.parameters.front().spelling(), "unsigned int<8>?[]");
	EXPECT_EQ(di_first.returns.front().spelling(), "unsigned int<8>");
}

TEST (SemanticChecking, MatchesExactContextualOptionalAndGenericArguments) {
	EXPECT_NO_THROW(checkSource(R"(fn<int> take(int x) = x
fn<bool> take(int<8> x) = true
fn<int> withDefault(opt<int> step ?: 1, int x) = step + x
fn<T> first<T>(T[] values) = values[0]
fn<int> entry() = withDefault(take(first([42])))
fn<bool> narrow() = take(1 -> int<8>)
)"));
	using Code = cde::DiagnosticCode;
	expectError("fn f(int x) {}\nfn<int> f(int y) = 1", Code::DUPLICATE_FUNCTION);
	expectError("fn f(int<8> x) {}\nfn f(int<16> x) {}\nfn run() = f(1)", Code::AMBIGUOUS_CALL);
	expectError("fn f(int x) {}\nfn run() = f(true)", Code::NO_MATCHING_CALL);
	expectError("fn f(int x) {}\nfn run() = f()", Code::CALL_ARITY);
	expectError("fn f(int x,int y) {}\nfn run() = f(y=1,2)", Code::INVALID_ARGUMENT_BINDING);
	expectError("fn<int> run() = missing()", Code::UNKNOWN_NAME);
}

TEST (SemanticChecking, ChecksBodiesScopesAssignmentsAndAllReturnPaths) {
	using Code = cde::DiagnosticCode;
	EXPECT_NO_THROW(checkSource(R"(fn<int,int> pair(int x) = x,x
fn<int> run(int n) {
    def<int,int> a,b = pair(n)
    if (a > 0) { return b } else { return a }
}
)"));
	expectError("fn<int> run(bool condition) { if (condition) { return 1 } }", Code::MISSING_RETURN);
	expectError("fn<int> run() = true", Code::RETURN_TYPE);
	expectError("fn run() { def local = later\ndef later = 1 }", Code::UNKNOWN_NAME);
	expectError("fn run() { if (true) { def x = 1 }\ndef y = x }", Code::UNKNOWN_NAME);
	expectError("fn run(final int x) { x = 2 }", Code::IMMUTABLE_BINDING);
	expectError("fn run() { def x := 1\nx++ }", Code::IMMUTABLE_BINDING);
	expectError("fn run() { def<int> x = true }", Code::TYPE_MISMATCH);
	expectError("fn run() { if (1) {} }", Code::NON_BOOLEAN_CONDITION);
	expectError("fn run() { break }", Code::INVALID_LOOP_JUMP);
	expectError("fn<Missing> run() = 1", Code::UNSUPPORTED_TYPE);
}

TEST (SemanticChecking, ChecksDeferredReturnsAndProducesNoValueForVoidReads) {
	const CheckedProgram       cp_program = checkSource(R"(fn cleanup() {}
fn use(bool early) {
    def<void> later := { if (early) return; return cleanup() }
    later
    return later
}
fn<int> typed() { def<int> later = { return 3 }; return later }
fn empty() { def<void> first, second = {}; first; second }
)");
	const Symbol              &sym_use    = cp_program.units.front()->get_globals().at("use").front();
	const CallableSignature   &cs_use     = *std::get<const CallableSignature *>(sym_use.declaration);
	const BlockBody           &bb_body    = std::get<BlockBody>(cs_use.executableBody()->value);
	const BindingDeclaration  &bd_later   = dynamic_cast<const BindingDeclaration &>(*bb_body.statements.at(0));
	const ExpressionStatement &es_read    = dynamic_cast<const ExpressionStatement &>(*bb_body.statements.at(1));
	const ReturnStatement     &rs_return  = dynamic_cast<const ReturnStatement &>(*bb_body.statements.at(2));
	EXPECT_EQ(cp_program.localTypes.at(&bd_later.name), ResolvedType(kVoid));
	EXPECT_EQ(cp_program.expressions.at(bd_later.initializer.get()).resultKind(), ResultKind::Empty);
	for (const Expression *e_read : { es_read.expression.get(), rs_return.value.get() }) {
		const ExpressionInfo &ei_read = cp_program.expressions.at(e_read);
		EXPECT_EQ(ei_read.binding, &bd_later.name);
		EXPECT_EQ(ei_read.resultKind(), ResultKind::Empty);
	}

	using Code = cde::DiagnosticCode;
	expectError("fn use() { def later = { return 1 } }", Code::BLOCK_RETURN_TYPE_REQUIRED);
	expectError("fn use() { def<void> later = { return 1 } }", Code::VALUE_IN_VOID);
	expectError("fn use() { def<int> later = { return } }", Code::RETURN_VALUE_REQUIRED);
	expectError("fn use() { def<int> later = {} }", Code::MISSING_RETURN);
	expectError("fn<int> use() { def<void> later = { return }; later }", Code::MISSING_RETURN);
	expectError("fn use() { def<void> later = {}; def value = later }", Code::VOID_VALUE);
	expectError("fn take(int n) {}\nfn use() { def<void> later = {}; take(later) }", Code::NO_MATCHING_CALL);
	for (const std::string s_use : { "later()", "later(1)", "later<int>()" })
		expectError("fn use() { def<int> later = { return 1 }; " + s_use + " }", Code::NOT_CALLABLE);
	for (const std::string s_write : { "later = 1", "later += 1", "later++", "other, later = 2, 1" })
		expectError("fn use(int other) { def<int> later = { return 1 }; " + s_write + " }", Code::BLOCK_ASSIGNMENT);
}

TEST (SemanticChecking, AppliesDeferredWritesOnlyWhenTheBindingIsRead) {
	const std::string s_source = "fn<usize> use(string? text) { if (text == null) return 0; def<void> clear = { text = null }; ";
	EXPECT_NO_THROW(checkSource(s_source + "return text::size }"));
	expectError(s_source + "clear; return text::size }", cde::DiagnosticCode::NULLABLE_PAYLOAD_ACCESS);
	const std::string s_delete = "typedef<class> Item { pub def<int> value = 1 }\n"
	                             "fn<int> use(Item item) { def<void> destroy = { item::delete }; ";
	EXPECT_NO_THROW(checkSource(s_delete + "return item.value }"));
	expectError(s_delete + "destroy; return item.value }", cde::DiagnosticCode::DELETED_CLASS_ACCESS);
}

TEST (SemanticChecking, EnforcesMemberVisibilityAndSpecializesReceiverTypes) {
	EXPECT_NO_THROW(checkSource(R"(pub typedef<class> Box<T>(def<T> item) {
    pub mt<T> get() = self.item
    mt<T> other(Box<T> box) = box.item
}
fn<int> use(Box<int> box) = box.get()
)"));
	using Code = cde::DiagnosticCode;
	expectError("pub typedef<struct> S { int x }\nfn<int> leak(S s) = s.x", Code::PRIVATE_CLASS_MEMBER);
	expectError("typedef<class> C { mt<int> read() = 1 }\nfn<int> leak(C c) = c.read()", Code::PRIVATE_CLASS_MEMBER);
	expectError("pub typedef<enum> E { pub A, B }\nfn<E> leak() = E.B", Code::PRIVATE_CLASS_MEMBER);
	expectError("pub typedef<class> C { typedef<struct> Hidden {} }\nfn use(C.Hidden h) {}", Code::PRIVATE_NESTED_TYPE);
	expectError("typedef<class> C { pub mt<int> pick(bool x) = 1\nmt<int> pick(int x) = x }\nfn<int> leak(C c) = c.pick(2)", Code::PRIVATE_CLASS_MEMBER);
}

TEST (SemanticChecking, ChecksFieldWritesNullableGuardsLoopsAndMultiResultAssignments) {
	EXPECT_NO_THROW(checkSource(R"(typedef<struct> Point { pub int x }
fn<int,int> pair(int n) = n, n
fn<int> sum(Point? point, int[] items) {
    if (point == null) return 0
    def total = point.x
    for (def n <- items) total += n
    def a,b = pair(total)
    a,b = pair(b)
    while (a > 0) { a-- }
    return b
}
)"));
	using Code = cde::DiagnosticCode;
	expectError("fn run(final int[] items) { items[0] = 1 }", Code::IMMUTABLE_BINDING);
	expectError("typedef<enum> E { pub First }\nfn run() { E.First = E.First }", Code::INVALID_FIELD_TARGET);
	expectError("fn<int> bad(int? n) { if (n != null) { n = null\nreturn n }\nreturn 0 }", Code::RETURN_TYPE);
	expectError("fn<int,int> pair() = 1, 2\nfn run() { def a,b,c = pair() }", Code::RECEPTION_ARITY);
}

TEST (SemanticChecking, ChecksInheritedOverloadsAndGenericBounds) {
	EXPECT_NO_THROW(checkSource(R"(typedef<interface> Readable { pub mt<int> read() }
typedef<class> Base(int seed) : Readable {
    pub Base() : (0) {}
    virtual: pub mt<int> read() = 1
    pub mt<int> read(int x) = x
}
typedef<class> Derived : Base { override: pub mt<int> read() = 2 }
fn<int> through<T: Readable>(T x) = x.read()
fn<int> check(Derived d) = through(d) + d.read(3) + d.read()
)"));
	using Code = cde::DiagnosticCode;
	expectError("fn<T> choose<T>(T x) = x\nfn<U> choose<U>(U y) = y", Code::DUPLICATE_FUNCTION);
	expectError("typedef<interface> I {}\nfn<T> constrained<T:I>(T x) = x\nfn<int> wrong() = constrained(1)", Code::UNSATISFIED_GENERIC_BOUND);
	expectError("typedef<class> A : B\ntypedef<class> B : A", Code::INTERFACE_INHERITANCE_CYCLE);
	expectError("typedef<interface> I { pub mt<int> get() }\ntypedef<class> C : I", Code::MISSING_INTERFACE_IMPLEMENTATION);
	expectError("typedef<interface> I { pub mt<int> get() }\ntypedef<class> C : I { mt<int> get() = 0 }", Code::INCOMPATIBLE_INTERFACE_REQUIREMENT);
	expectError("typedef<interface> I { pub mt<int> get() }\ntypedef<class> C : I { pub mt<bool> get() = true }", Code::INCOMPATIBLE_INTERFACE_REQUIREMENT);
}

TEST (SemanticChecking, ChecksOperatorsConstructorsAndStatePayloads) {
	EXPECT_NO_THROW(checkSource(R"(typedef<class> Counter(def<int> n) {
    pub Counter() : (0) {}
    pub operator +(int delta) = self.n + delta
    operator []?(int i) = true
    pub operator []=(int i, int value) { self.n = value }
    default: (0) pub state On
    pub state<void> ::clear()
}
typedef<state<int>> On for Counter { pub mt clear() { self -> On(1) } }
fn<int> work() {
    def c = Counter::new()
    c[0] = 3
    c -> On(2)
    c::clear()
    return c + 2
}
)"));
	using Code = cde::DiagnosticCode;
	expectError("typedef<class> Box(int x)\nfn<Box> leak() = Box::new(1)", Code::PRIVATE_CLASS_MEMBER);
	expectError("typedef<class> Lamp { state Missing }", Code::INVALID_STATE_ATTACHMENT);
	expectError("typedef<class> C { default: (0) pub state<int> On {} }\nfn bad(C c) { c -> On() }", Code::INVALID_STATE_PAYLOAD);
}

TEST (SemanticChecking, DelegatesToThePrimaryAndKeepsOrdinaryConstructorOverloads) {
	const CheckedProgram          cp_program    = checkSource(R"(typedef<class> Counter(def<int<8>> seed) {
    pub Counter(bool flag) : (1) { self.seed = 2 }
    Counter(int number) : (number -> int<8>) {}
}
fn<Counter> make() = Counter::new(true)
)");
	const SymbolScope            &ss_globals    = cp_program.units.front()->get_globals();
	const Symbol                 &sym_counter   = ss_globals.at("Counter").front();
	const ClassDeclaration       &cd_counter    = dynamic_cast<const ClassDeclaration &>(*std::get<const TypeDeclaration *>(sym_counter.declaration));
	const ConstructorDeclaration &cd_secondary  = std::get<ConstructorDeclaration>(cd_counter.members.front().value);
	const ExpressionInfo         &ei_delegation = cp_program.expressions.at(&cd_secondary.delegation);
	ASSERT_NE(ei_delegation.symbol, nullptr);
	EXPECT_EQ(ei_delegation.symbol, cp_program.declarations.at(&sym_counter).primaryConstructor);
	const PrimaryConstructorDeclaration *pcd_primary =
	        dynamic_cast<const PrimaryConstructorDeclaration *>(std::get<const CallableSignature *>(ei_delegation.symbol->declaration));
	ASSERT_NE(pcd_primary, nullptr);
	EXPECT_EQ(pcd_primary->owner.get(), &cd_counter);
	EXPECT_EQ(ei_delegation.parameterForArgument, (std::vector<std::size_t>{ 0 }));
	const ExpressionInfo &ei_argument = cp_program.expressions.at(cd_secondary.delegation.suppliedArguments.front().expression.get());
	ASSERT_EQ(ei_argument.types.size(), 1u);
	EXPECT_EQ(ei_argument.types.front().width, 8u);

	const CallableSignature &cs_make     = *std::get<const CallableSignature *>(ss_globals.at("make").front().declaration);
	const Expression        &e_creation  = *std::get<ExpressionBody>(cs_make.executableBody()->value).expression;
	const ExpressionInfo    &ei_creation = cp_program.expressions.at(&e_creation);
	ASSERT_NE(ei_creation.symbol, nullptr);
	EXPECT_EQ(std::get<const CallableSignature *>(ei_creation.symbol->declaration), &cd_secondary);
}

TEST (SemanticChecking, ReadsFieldsInDeclarationOrderAndKeepsOtherInstancesIndependent) {
	const CheckedProgram         cp_program = checkSource(R"(typedef<class> Base(pub def<int> inherited) { pub Base() : (1) {} }
typedef<class> Sample(int seed, def<int> offset ?: seed, def<Sample> other) : Base {
    def<int> z = seed + self.offset
    def<int> a = z + self.z
    def<int> total = self.inherited + other.last + self.other.read()
    def<int> last = 1
    mt<int> read() = self.last
}
)");
	const Symbol                &sym_sample = cp_program.units.front()->get_globals().at("Sample").front();
	const Symbol                &sym_z      = sym_sample.members.at("z").front();
	const ClassFieldDeclaration &cfd_a      = *std::get<const ClassFieldDeclaration *>(sym_sample.members.at("a").front().declaration);
	const BinaryExpression      &be_sum     = dynamic_cast<const BinaryExpression &>(*cfd_a.initializer);
	EXPECT_EQ(cp_program.expressions.at(be_sum.left.get()).symbol, &sym_z);
	EXPECT_EQ(cp_program.expressions.at(be_sum.right.get()).symbol, &sym_z);
}

TEST (SemanticChecking, ChecksTheReceiverDuringConstructorDefaultsAndDelegation) {
	using Code = cde::DiagnosticCode;
	expectError("typedef<class> C(def<int> x ?: self.x)", Code::PARTIAL_CLASS_INSTANCE);
	expectError("typedef<class> C(def<int> x) { C(float y) : (self.x) {} }", Code::PARTIAL_CLASS_INSTANCE);
	expectError("typedef<class> Base(int x)\ntypedef<class> C(def<int> x) : Base(self.x)", Code::PARTIAL_CLASS_INSTANCE);
	expectError("typedef<class> C(def<int> x ?: if (true) { return } else { 1 })", Code::INVALID_PARAMETER_DEFAULT);
	EXPECT_NO_THROW(checkSource("fn<int> seed() = 1\ntypedef<class> C(def<int> x ?: seed()) { mt<bool> seed() = false }"));
}

TEST (SemanticChecking, RestrictsUsesOfTheInstanceDuringFieldInitialization) {
	using Code = cde::DiagnosticCode;
	expectError("typedef<class> C { def<int> a = b\ndef<int> b = 1 }", Code::PARTIAL_CLASS_INSTANCE);
	expectError("typedef<class> C { def<int> a = self.a }", Code::PARTIAL_CLASS_INSTANCE);
	expectError("fn<int> take(C c) = 1\ntypedef<class> C { def<int> x = take(self) }", Code::PARTIAL_CLASS_INSTANCE);
	expectError("typedef<class> C { def<int> a = read()\nmt<int> read() = 1 }", Code::PARTIAL_CLASS_INSTANCE);
	expectError("typedef<class> C { def<int> a = self.read()\nmt<int> read() = 1 }", Code::PARTIAL_CLASS_INSTANCE);
	expectError("typedef<class> C { def<int> a = 1\ndef<int> b = if (true) { a = 2; a } else { 0 } }", Code::PARTIAL_CLASS_INSTANCE);
	expectError("typedef<class> C(def<int[1]> a) { def<int> x = if (true) { self.a[0] = 9; self.a[0] } else { 0 } }", Code::PARTIAL_CLASS_INSTANCE);
	expectError("typedef<class> Other(pub def<int> n)\ntypedef<class> C(def<Other> other) { def<int> x = if (true) { self.other.n = 2; 1 } else { 0 } }",
	            Code::PARTIAL_CLASS_INSTANCE);
	expectError("typedef<struct> S { pub mt<int> read() = 1 }\ntypedef<class> C(def<S> s) { def<int> x = self.s.read() }", Code::PARTIAL_CLASS_INSTANCE);
	expectError("typedef<class> C { def<int> x = if (true) { return } else { 0 } }", Code::PARTIAL_CLASS_INSTANCE);
	EXPECT_NO_THROW(checkSource(R"(typedef<class> C {
    default: state<void> Ready {}
    def<bool> ready = self is Ready
    def<int> first = if (true) { def<int> local := { return 1 }; local } else { 0 }
}
)"));
}

TEST (SemanticChecking, ResolvesUpdatesAndDistinguishesStateTypeAndValueTests) {
	const CheckedProgram checked = checkSource(R"(typedef<class> Meter(def<int> n) {
    pub operator +=(int amount) { self.n += amount }
    pub operator ++() { self.n++ }
    default: pub state<void> Ready {}
}
fn<bool> test(Meter m) {
    m += 3
    m++
    m -> Ready
    def range = 1 .. 3
    return m is Ready && m is Meter && 2 is range
}
)");
	EXPECT_EQ(checked.writes.size(), 2u);
	EXPECT_EQ(checked.valueTests.size(), 1u);
	using Code = cde::DiagnosticCode;
	expectError("typedef<class> C { default: state<void> Hidden {} }\nfn<bool> bad(C c) = c is Hidden", Code::PRIVATE_CLASS_MEMBER);
	expectError("fn<bool> bad(int n) = 3 is n", Code::INVALID_RANGE_MEMBERSHIP);
	expectError("fn<int> bad(int? n) = n -> int", Code::NULLABLE_PAYLOAD_ACCESS);
}

TEST (SemanticChecking, InfersClassArgumentsAndResolvesBoundConstructors) {
	EXPECT_NO_THROW(checkSource(R"(typedef<class> Cell<T>(def<T> item, int unused) {
    pub Cell(T value) : (value, 0) {}
}
fn<T> construct<T: new(int value)>(int value) = T::new(value)
fn<Cell<int>> inferred() = Cell::new(5)
fn<Cell<int>> factory() = construct<Cell<int>>(5)
)"));
	expectError("typedef<class> Hidden(int value)\nfn<T> make<T: new(int value)>() = T::new(1)\nfn<Hidden> run() = make<Hidden>()",
	            cde::DiagnosticCode::UNSATISFIED_GENERIC_BOUND);
}

TEST (SemanticChecking, ChecksGroupedBindingsAndTheirDeclaredTypes) {
	EXPECT_NO_THROW(checkSource(R"(fn<int> private() {
    def<int<8>> a,b = 1
    a,b = 2
    def x, y = a, b
    while (true) {
        return (x + y) -> int
        break
    }
}
)"));
	expectError("fn run() { def<int,int> a,b = 1 }", cde::DiagnosticCode::RECEPTION_ANNOTATIONS);
	expectError("fn run() { def<int> a,b = 1, 2 }", cde::DiagnosticCode::RECEPTION_ANNOTATIONS);
}

}
}
