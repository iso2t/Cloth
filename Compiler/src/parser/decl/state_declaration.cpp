// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include <parser/decl/declarations.hpp>
#include <parser/decl/state_declaration.hpp>
#include <parser/expr/expression.hpp>
#include <parser/type/type_parameters.hpp>
#include <parser/type/type_reference.hpp>
#include <token/token.hpp>

namespace cloth {

StateSelectionDeclaration::StateSelectionDeclaration (std::optional<InitialStatePrefix> initial) : initial(std::move(initial)) {
}

StateBodyDeclaration::StateBodyDeclaration (const Token &keyword, std::optional<Token> visibility, const Token &name, TypeReference payloadType,
                                            std::vector<StateMemberPtr> members) :
        keyword(keyword),
        visibility(std::move(visibility)),
        name(name),
        payloadType(std::move(payloadType)),
        members(std::move(members)) {
}

NestedStateDeclaration::NestedStateDeclaration (const Token &keyword, std::optional<Token> visibility, const Token &name, TypeReference payloadType,
                                                std::optional<InitialStatePrefix> initial, std::vector<StateMemberPtr> members, TokenSpan span) :
        StateSelectionDeclaration(std::move(initial)),
        StateBodyDeclaration(keyword, std::move(visibility), name, std::move(payloadType), std::move(members)),
        span(span) {
}

StateAttachmentDeclaration::StateAttachmentDeclaration (const Token &keyword, const Token &name, std::optional<InitialStatePrefix> initial, TokenSpan span) :
        StateSelectionDeclaration(std::move(initial)),
        keyword(keyword),
        name(name),
        span(span) {
}

TopLevelStateDeclaration::TopLevelStateDeclaration (const Token &keyword, std::optional<Token> visibility, const Token &name, TypeReference payloadType,
                                                    TypeReference host, std::vector<StateMemberPtr> members, TokenSpan span,
                                                    std::vector<TypeDeclarationPtr> nestedTypes) :
        Declaration{ span },
        StateBodyDeclaration(keyword, std::move(visibility), name, std::move(payloadType), std::move(members)),
        host(std::move(host)),
        nestedTypes(std::move(nestedTypes)) {
}

StateMethodContractDeclaration::StateMethodContractDeclaration (const Token &keyword, std::optional<Token> visibility, const Token &name,
                                                                std::vector<TypeReference> returnTypes, std::vector<ParameterDeclaration> parameters,
                                                                TokenSpan span, std::optional<TypeParameterList> typeParameters) :
        CallableSignature(keyword, std::move(parameters), span, std::move(returnTypes)),
        visibility(std::move(visibility)),
        name(name),
        typeParameters(std::move(typeParameters)) {
}

StateInitializerDeclaration::StateInitializerDeclaration (const Token &keyword, BlockBody body) : keyword(keyword), body{ std::move(body) } {
}

StateActivationDeclaration::StateActivationDeclaration (const Token &name, StateInitializerDeclaration initializer) :
        CallableSignature(initializer.keyword, {}, initializer.get_span()),
        name(name),
        initializer(std::move(initializer)) {
}

StateInitialPayloadDeclaration::StateInitialPayloadDeclaration (const Token &name, InitialStatePrefix prefix) :
        CallableSignature(prefix.keyword, {}, prefix.span),
        name(name),
        prefix(std::move(prefix)) {
}

const ExpressionPtr &StateInitialPayloadDeclaration::get_expression () const {
	if (!prefix.payload) {
		throw std::logic_error("Initial state payload is missing.");
	}
	return prefix.payload;
}

}
