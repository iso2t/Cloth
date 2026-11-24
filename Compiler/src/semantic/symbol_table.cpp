// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include <ast/decl/class_declaration.hpp>
#include <ast/decl/declarations.hpp>
#include <ast/decl/enum_declaration.hpp>
#include <ast/decl/function_declaration.hpp>
#include <ast/decl/interface_declaration.hpp>
#include <ast/decl/state_declaration.hpp>
#include <ast/decl/struct_declaration.hpp>
#include <ast/source_unit.hpp>
#include <diagnostics.hpp>
#include <semantic/symbol_table.hpp>
#include <token/token.hpp>

namespace cloth {
namespace {

bool allowsOverloads (SymbolKind kind) {
	return kind == SymbolKind::Function || kind == SymbolKind::Method || kind == SymbolKind::Operator || kind == SymbolKind::Constructor ||
	       kind == SymbolKind::StateMethodContract;
}

Symbol &declare (SymbolScope &scope, SymbolKind kind, const Token &name, const std::optional<Token> &visibility, SymbolDeclaration declaration) {
	std::vector<Symbol> &v_symbols = scope[name.getNameKey()];
	if (!allowsOverloads(kind)) {
		const auto it_previous = std::ranges::find(v_symbols, kind, &Symbol::kind);
		if (it_previous != v_symbols.end()) {
			throw cde::DeclarationErrors::duplicate(it_previous->name, name);
		}
	}
	return v_symbols.emplace_back(kind, visibility ? Visibility::Public : Visibility::Private, name, declaration, SymbolScope{});
}

void collectCallable (SymbolScope &scope, SymbolKind kind, const CallableSignature &callable) {
	declare(scope, kind, callable.getName(), callable.visibility, &callable);
}

void collectMethod (SymbolScope &scope, const MethodDeclaration &method) {
	collectCallable(scope, method.operatorMember ? SymbolKind::Operator : SymbolKind::Method, method);
}

void collectStateMembers (SymbolScope &scope, const StateBodyDeclaration &state) {
	for (const StateMemberPtr &smp_member : state.members) {
		if (const auto *md_method = dynamic_cast<const MethodDeclaration *>(smp_member.get())) {
			collectMethod(scope, *md_method);
		} else {
			const auto &sid_initializer = dynamic_cast<const StateInitializerDeclaration &>(*smp_member);
			declare(scope, SymbolKind::StateInitializer, sid_initializer.keyword, sid_initializer.visibility, &sid_initializer);
		}
	}
}

void collectClassState (SymbolScope &scope, const StateClassMember &member) {
	if (const auto *nsd_state = dynamic_cast<const NestedStateDeclaration *>(&member)) {
		Symbol &sym_state = declare(scope, SymbolKind::State, nsd_state->name, nsd_state->visibility, nsd_state);
		collectStateMembers(sym_state.members, *nsd_state);
	} else if (const auto *sad_attachment = dynamic_cast<const StateAttachmentDeclaration *>(&member)) {
		declare(scope, SymbolKind::StateAttachment, sad_attachment->name, sad_attachment->visibility, sad_attachment);
	} else {
		collectCallable(scope, SymbolKind::StateMethodContract, dynamic_cast<const StateMethodContractDeclaration &>(member));
	}
}

void collectClassMembers (SymbolScope &scope, const ClassDeclaration &type) {
	std::size_t st_fieldIndex = 0;
	for (const ClassParameterDeclaration &cpd_parameter : type.parameters) {
		if (cpd_parameter.fieldKeyword) {
			Symbol &sym_field         = declare(scope, SymbolKind::Field, cpd_parameter.parameter.name, cpd_parameter.visibility, &cpd_parameter);
			sym_field.classFieldIndex = st_fieldIndex++;
		}
	}
	for (const ClassMember &cm_member : type.members) {
		if (const ClassFieldDeclaration *cfd_field = std::get_if<ClassFieldDeclaration>(&cm_member.value)) {
			Symbol &sym_field         = declare(scope, SymbolKind::Field, cfd_field->name, cfd_field->visibility, cfd_field);
			sym_field.classFieldIndex = st_fieldIndex++;
		} else if (const MethodDeclaration *md_method = std::get_if<MethodDeclaration>(&cm_member.value)) {
			collectMethod(scope, *md_method);
		} else if (const ConstructorDeclaration *cd_constructor = std::get_if<ConstructorDeclaration>(&cm_member.value)) {
			collectCallable(scope, SymbolKind::Constructor, *cd_constructor);
		} else if (const DestructorDeclaration *dd_destructor = std::get_if<DestructorDeclaration>(&cm_member.value)) {
			const Token &t_tilde = dd_destructor->getOperator();
			const Token &t_type  = dd_destructor->name;
			const Token  t_name(t_tilde.getLexeme() + t_type.getLexeme(), TokenType::Identifier,
			                    TokenSpan::spanThrough(t_tilde.getSpan(), t_type.getSpan()));
			declare(scope, SymbolKind::Destructor, t_name, dd_destructor->visibility, dd_destructor);
		} else {
			collectClassState(scope, *std::get<StateClassMemberPtr>(cm_member.value));
		}
	}
}

}

SymbolTable::SymbolTable (SourceUnit source) : su_source(std::move(source)) {
	for (const DeclarationPtr &dp_declaration : su_source.declarations) {
		if (const auto *fd_function = dynamic_cast<const FunctionDeclaration *>(dp_declaration.get())) {
			collectCallable(ss_globals, SymbolKind::Function, *fd_function);
		} else if (const TypeDeclarationPtr tdp_type = std::dynamic_pointer_cast<const TypeDeclaration>(dp_declaration)) {
			collectType(ss_globals, tdp_type);
		} else {
			const auto &tsd_state = dynamic_cast<const TopLevelStateDeclaration &>(*dp_declaration);
			Symbol     &sym_state = declare(ss_globals, SymbolKind::State, tsd_state.name, tsd_state.visibility, &tsd_state);
			collectStateMembers(sym_state.members, tsd_state);
			for (const TypeDeclarationPtr &tdp_nested : tsd_state.nestedTypes) {
				collectType(sym_state.members, tdp_nested);
			}
		}
	}
}

void SymbolTable::collectType (SymbolScope &scope, const TypeDeclarationPtr &type) {
	SymbolScope &ss_members = declare(scope, SymbolKind::Type, type->name, type->visibility, type.get()).members;
	if (const std::shared_ptr<const ClassDeclaration> cdp_class = std::dynamic_pointer_cast<const ClassDeclaration>(type)) {
		const PrimaryConstructorDeclaration &pcd_primary =
		        *v_primaryConstructors.emplace_back(std::make_shared<PrimaryConstructorDeclaration>(cdp_class));
		collectCallable(ss_members, SymbolKind::Constructor, pcd_primary);
		collectClassMembers(ss_members, *cdp_class);
	} else if (const auto *sd_struct = dynamic_cast<const StructDeclaration *>(type.get())) {
		for (const StructFieldDeclaration &sfd_field : sd_struct->fields) {
			declare(ss_members, SymbolKind::Field, sfd_field.name, sfd_field.visibility, &sfd_field);
		}
		for (const MethodDeclaration &md_method : sd_struct->methods) {
			collectMethod(ss_members, md_method);
		}
	} else if (const auto *ed_enum = dynamic_cast<const EnumDeclaration *>(type.get())) {
		for (const EnumFieldDeclaration &efd_field : ed_enum->fields) {
			declare(ss_members, SymbolKind::Field, efd_field.name, efd_field.visibility, &efd_field);
		}
		for (std::size_t st_index = 0; st_index < ed_enum->cases.size(); ++st_index) {
			const EnumCaseDeclaration &ecd_case = ed_enum->cases[st_index];
			Symbol                    &sym_case = declare(ss_members, SymbolKind::EnumCase, ecd_case.name, ecd_case.visibility, &ecd_case);
			sym_case.enumCaseOrdinal            = st_index;
		}
		for (const MethodDeclaration &md_operator : ed_enum->operators) {
			collectMethod(ss_members, md_operator);
		}
	} else {
		const auto &id_interface = dynamic_cast<const InterfaceDeclaration &>(*type);
		for (const InterfaceMethodDeclaration &imd_method : id_interface.methods) {
			collectCallable(ss_members, SymbolKind::Method, imd_method);
		}
	}
	for (const TypeDeclarationPtr &tdp_nested : type->nestedTypes) {
		collectType(ss_members, tdp_nested);
	}
}

}
