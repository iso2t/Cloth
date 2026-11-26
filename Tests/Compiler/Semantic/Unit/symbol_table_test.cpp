// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <vector>

#include <lexer/lexer.hpp>
#include <parser/parser.hpp>
#include <semantic/symbol_table.hpp>
#include <token/token.hpp>
#include "ast/decl/declarations.hpp"

namespace cloth {
namespace {

SymbolTable collect (const std::string &source) {
	Lexer l_lexer(source);
	return SymbolTable(Parser(l_lexer).parse());
}

void expectSymbol (const SymbolScope &scope, std::string_view name, SymbolKind kind, Visibility visibility = Visibility::Private) {
	SCOPED_TRACE(name);
	const auto it_symbol = scope.find(name);
	ASSERT_NE(it_symbol, scope.end());
	const std::vector<Symbol> &v_symbols = it_symbol->second;
	ASSERT_EQ(v_symbols.size(), 1u);
	EXPECT_EQ(v_symbols.front().kind, kind);
	EXPECT_EQ(v_symbols.front().visibility, visibility);
}

TEST (SemanticSymbols, CollectsForwardDeclarationsAndOverloadsWithoutResolvingBodies) {
	const SymbolTable  st_table   = collect(R"(using shapes: Imported;
fn<Later> private(Later x) = convert(x);
pub fn<Later> convert(Later x) = private(x);
fn<int> convert(int x) = missing(x);
fn work() { def local = missing(); }
typedef<struct> Later { int private; }
)");
	const SymbolScope &ss_globals = st_table.getGlobals();
	EXPECT_EQ(ss_globals.size(), 4u);
	EXPECT_FALSE(ss_globals.contains("local"));
	EXPECT_FALSE(ss_globals.contains("Imported"));
	expectSymbol(ss_globals, "private", SymbolKind::Function);
	expectSymbol(ss_globals, "Later", SymbolKind::Type);
	expectSymbol(ss_globals.at("Later").front().members, "private", SymbolKind::Field);
	const std::vector<Symbol> &v_overloads = ss_globals.at("convert");
	ASSERT_EQ(v_overloads.size(), 2u);
	EXPECT_EQ(v_overloads[0].visibility, Visibility::Public);
	EXPECT_EQ(v_overloads[1].visibility, Visibility::Private);
	const CallableSignature *cs_callable = std::get<const CallableSignature *>(v_overloads[0].declaration);
	EXPECT_EQ(cs_callable, st_table.getSourceUnit().declarations[1].get());
	ASSERT_EQ(cs_callable->parameters.size(), 1u);
	EXPECT_EQ(cs_callable->parameters[0].type.getSpelling(), "Later");
}

TEST (SemanticSymbols, PublicBlocksUseTheEnclosingScopeAndStopAtTypeBodies) {
	const SymbolTable  table   = collect(R"(pub {
    typedef<class> Door {
        def<int> key = 0;
        state Ready;
        pub {
            Door(int key) : () {}
            ~Door {}
            def<int> count = 0;
            default state<void> Idle { mt reset() {} }
            state Hidden;
            operator +(int delta) -> int = delta;
            typedef<class> Part { mt inside() {} }
        }
        mt after() {}
    }
    typedef<state<void>> Ready for Door;
    typedef<interface> View {
        pub { pub { mt show(); } typedef<class> Entry; }
        mt hidden();
    }
}
typedef<state<void>> Hidden for Door;
typedef<class> Internal { pub { mt visible() {} } }
)");
	const SymbolScope &globals = table.getGlobals();
	expectSymbol(globals, "Door", SymbolKind::Type, Visibility::Public);
	expectSymbol(globals, "Ready", SymbolKind::State, Visibility::Public);
	expectSymbol(globals, "Hidden", SymbolKind::State);
	const SymbolScope &door = globals.at("Door").front().members;
	expectSymbol(door, "Ready", SymbolKind::StateAttachment);
	expectSymbol(door, "Hidden", SymbolKind::StateAttachment, Visibility::Public);
	expectSymbol(door, "Idle", SymbolKind::State, Visibility::Public);
	expectSymbol(door.at("Idle").front().members, "reset", SymbolKind::Method);
	expectSymbol(door, "key", SymbolKind::Field);
	expectSymbol(door, "count", SymbolKind::Field, Visibility::Public);
	expectSymbol(door, "after", SymbolKind::Method);
	ASSERT_EQ(door.at("Door").size(), 2u);
	EXPECT_EQ(door.at("Door")[1].visibility, Visibility::Public);
	expectSymbol(door, "~Door", SymbolKind::Destructor, Visibility::Public);
	expectSymbol(door, "+", SymbolKind::Operator, Visibility::Public);
	expectSymbol(door, "Part", SymbolKind::Type, Visibility::Public);
	expectSymbol(door.at("Part").front().members, "inside", SymbolKind::Method);
	const SymbolScope &view = globals.at("View").front().members;
	expectSymbol(view, "Entry", SymbolKind::Type, Visibility::Public);
	expectSymbol(view, "show", SymbolKind::Method, Visibility::Public);
	expectSymbol(view, "hidden", SymbolKind::Method);
	expectSymbol(globals, "Internal", SymbolKind::Type);
	expectSymbol(globals.at("Internal").front().members, "visible", SymbolKind::Method, Visibility::Public);
}

TEST (SemanticSymbols, AcceptsPrivateAndPrivAsFieldTypeNames) {
	const SymbolTable  st_table   = collect(R"(typedef<struct> private {}
pub typedef<struct> priv {}
typedef<struct> Pair { private left, priv right; }
)");
	const SymbolScope &ss_globals = st_table.getGlobals();
	expectSymbol(ss_globals, "private", SymbolKind::Type);
	expectSymbol(ss_globals, "priv", SymbolKind::Type, Visibility::Public);
	const SymbolScope &ss_pair = ss_globals.at("Pair").front().members;
	expectSymbol(ss_pair, "left", SymbolKind::Field);
	expectSymbol(ss_pair, "right", SymbolKind::Field);
}

TEST (SemanticSymbols, KeepsClassAndStateMembersInTheirOwningScopes) {
	const SymbolTable  st_table   = collect(R"(pub typedef<class> Lamp(pub def<int> light, int setting) {
    def<int> power = unknown();
    pub Lamp() : (0, 0) {}
    pub ~Lamp {}
    pub mt<int> level() = power;
    operator +(int value) -> int = value;
    pub operator ::power() -> int = 42;
    state<void> ::switchOn();
    pub state<void> On {
        init {}
        mt switchOn() {}
    }
    state Off;
    typedef<struct> Part { pub int light; }
}
typedef<state<void>> pub Off for Lamp {
    pub init {}
    pub mt switchOn() {}
    typedef<struct> Part {}
}
)");
	const SymbolScope &ss_globals = st_table.getGlobals();
	ASSERT_EQ(ss_globals.size(), 2u);
	expectSymbol(ss_globals, "Lamp", SymbolKind::Type, Visibility::Public);
	const SymbolScope &ss_lamp = ss_globals.at("Lamp").front().members;
	expectSymbol(ss_lamp, "light", SymbolKind::Field, Visibility::Public);
	EXPECT_FALSE(ss_lamp.contains("setting"));
	const std::vector<Symbol> &v_power = ss_lamp.at("power");
	ASSERT_EQ(v_power.size(), 2u);
	EXPECT_EQ(v_power[0].kind, SymbolKind::Field);
	EXPECT_EQ(v_power[0].visibility, Visibility::Private);
	EXPECT_EQ(v_power[1].kind, SymbolKind::Operator);
	EXPECT_EQ(v_power[1].visibility, Visibility::Public);
	expectSymbol(ss_lamp, "level", SymbolKind::Method, Visibility::Public);
	expectSymbol(ss_lamp, toString(OPlus), SymbolKind::Operator);
	const std::vector<Symbol> &v_constructors = ss_lamp.at("Lamp");
	ASSERT_EQ(v_constructors.size(), 2u);
	EXPECT_EQ(v_constructors[0].kind, SymbolKind::Constructor);
	EXPECT_EQ(v_constructors[0].visibility, Visibility::Private);
	EXPECT_EQ(std::get<const CallableSignature *>(v_constructors[0].declaration)->parameters.size(), 2u);
	EXPECT_EQ(v_constructors[1].kind, SymbolKind::Constructor);
	EXPECT_EQ(v_constructors[1].visibility, Visibility::Public);
	expectSymbol(ss_lamp, std::string(toString(OTilde)) + "Lamp", SymbolKind::Destructor, Visibility::Public);
	expectSymbol(ss_lamp, "Part", SymbolKind::Type);
	expectSymbol(ss_lamp.at("Part").front().members, "light", SymbolKind::Field, Visibility::Public);
	expectSymbol(ss_lamp, "switchOn", SymbolKind::StateMethodContract);
	expectSymbol(ss_lamp, "Off", SymbolKind::StateAttachment);
	expectSymbol(ss_lamp, "On", SymbolKind::State, Visibility::Public);
	const SymbolScope &ss_on = ss_lamp.at("On").front().members;
	expectSymbol(ss_on, "switchOn", SymbolKind::Method);
	expectSymbol(ss_on, toString(KInit), SymbolKind::StateInitializer);
	expectSymbol(ss_globals, "Off", SymbolKind::State, Visibility::Public);
	const SymbolScope &ss_off = ss_globals.at("Off").front().members;
	expectSymbol(ss_off, toString(KInit), SymbolKind::StateInitializer, Visibility::Public);
	expectSymbol(ss_off, "switchOn", SymbolKind::Method, Visibility::Public);
	expectSymbol(ss_off, "Part", SymbolKind::Type);
}

TEST (SemanticSymbols, RequiresExplicitPublicVisibilityOnStructEnumAndInterfaceMembers) {
	const SymbolTable  st_table   = collect(R"(pub typedef<struct> Pair {
    pub int left, int right;
    pub mt<int> first() = left;
    mt<int> second() = right;
    pub operator +(int delta) -> int = delta;
}
pub typedef<enum> Mode(pub def<int> code) {
    pub On(1), Off(0);
    pub operator +(int delta) -> int = delta;
}
typedef<interface> Sized {
    pub mt<int> size();
    mt<bool> empty() = default (unknown());
}
)");
	const SymbolScope &ss_globals = st_table.getGlobals();
	const SymbolScope &ss_pair    = ss_globals.at("Pair").front().members;
	expectSymbol(ss_pair, "left", SymbolKind::Field, Visibility::Public);
	expectSymbol(ss_pair, "right", SymbolKind::Field);
	expectSymbol(ss_pair, "first", SymbolKind::Method, Visibility::Public);
	expectSymbol(ss_pair, "second", SymbolKind::Method);
	expectSymbol(ss_pair, toString(OPlus), SymbolKind::Operator, Visibility::Public);
	const SymbolScope &ss_mode = ss_globals.at("Mode").front().members;
	expectSymbol(ss_mode, "code", SymbolKind::Field, Visibility::Public);
	expectSymbol(ss_mode, "On", SymbolKind::EnumCase, Visibility::Public);
	expectSymbol(ss_mode, "Off", SymbolKind::EnumCase);
	expectSymbol(ss_mode, toString(OPlus), SymbolKind::Operator, Visibility::Public);
	const SymbolScope &ss_sized = ss_globals.at("Sized").front().members;
	expectSymbol(ss_sized, "size", SymbolKind::Method, Visibility::Public);
	expectSymbol(ss_sized, "empty", SymbolKind::Method);
}

}
}
