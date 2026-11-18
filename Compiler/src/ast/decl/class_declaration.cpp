// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <ast/decl/class_declaration.hpp>
#include <ast/decl/declarations.hpp>
#include <ast/expr/class_expression.hpp>
#include <ast/type/type_parameters.hpp>
#include <token/token.hpp>

namespace cloth {

namespace {

std::vector<ParameterDeclaration> collectPrimaryParameters (const ClassDeclaration &owner) {
	std::vector<ParameterDeclaration> v_parameters;
	v_parameters.reserve(owner.parameters.size());
	for (const ClassParameterDeclaration &cpd_parameter : owner.parameters) {
		v_parameters.push_back(cpd_parameter.parameter);
	}
	return v_parameters;
}

}

ConstructorDeclaration::ConstructorDeclaration (const Token &name, std::vector<ParameterDeclaration> parameters, ConstructorDelegation delegation,
                                                BlockBody body) :
        CallableDeclaration(name, name, std::move(parameters), FunctionBody{ std::move(body) }, name.get_span()),
        delegation(std::move(delegation)) {
}

DestructorDeclaration::DestructorDeclaration (const Token &op, const Token &name, BlockBody body) :
        CallableDeclaration(op, name, {}, FunctionBody{ std::move(body) }, op.get_span()) {
}

ClassDeclaration::ClassDeclaration (const Token &keyword, const Token &name, std::optional<Token> visibility, std::optional<Token> finalKeyword,
                                    std::vector<ClassParameterDeclaration> parameters, std::vector<ClassMember> members, TokenSpan span,
                                    std::vector<ClassParentClause> parents, std::optional<TypeParameterList> typeParameters,
                                    std::vector<TypeDeclarationPtr> nestedTypes) :
        TypeDeclaration(name, std::move(visibility), span, std::move(nestedTypes)),
        keyword(keyword),
        finalKeyword(std::move(finalKeyword)),
        parameters(std::move(parameters)),
        members(std::move(members)),
        parents(std::move(parents)),
        typeParameters(std::move(typeParameters)) {
}

PrimaryConstructorDeclaration::PrimaryConstructorDeclaration (std::shared_ptr<const ClassDeclaration> owner) :
        CallableDeclaration(owner->keyword, owner->name, collectPrimaryParameters(*owner), FunctionBody{ BlockBody{ owner->name.get_span() } },
                            owner->name.get_span()),
        owner(std::move(owner)) {
}

}
