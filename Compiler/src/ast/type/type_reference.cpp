// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <ast/type/type_reference.hpp>
#include <token/token.hpp>

namespace cloth {

bool TypeArgumentList::hasPlaceholders () const {
	return std::ranges::any_of(arguments, [] (const TypeReference &argument) { return argument.is_placeholder(); });
}

TypeReference::TypeReference (const Token &name, bool isUnsigned, std::optional<Token> width, std::optional<TokenSpan> span,
                              std::vector<TokenSpan> arraySuffixes, std::vector<std::optional<Token>> arraySizes, std::optional<TokenSpan> nullable,
                              std::vector<std::optional<TokenSpan>> arrayNullability, std::optional<TypeArgumentList> application) :
        t_name(name),
        b_unsigned(isUnsigned),
        opt_width(std::move(width)),
        t_span(span.value_or(name.get_span())),
        v_arraySuffixes(std::move(arraySuffixes)),
        vopt_arraySizes(std::move(arraySizes)),
        opt_nullable(std::move(nullable)),
        vopt_arrayNullability(std::move(arrayNullability)),
        opt_application(std::move(application)) {
}

bool TypeReference::hasNullability () const {
	return opt_nullable || std::ranges::any_of(vopt_arrayNullability, [] (const std::optional<TokenSpan> &nullability) { return nullability.has_value(); });
}

bool TypeReference::is_placeholder () const {
	return t_name.get_type() == TokenType::Operator && t_name.get_lexeme() == to_string(oUnderscore);
}

bool TypeReference::capturesWidth () const {
	return opt_width && opt_width->get_lexeme() == to_string(kAuto);
}

const std::vector<TypeReference> &TypeReference::get_type_arguments () const {
	static constexpr std::vector<TypeReference> v_empty;
	return opt_application ? opt_application->arguments : v_empty;
}

const Token *TypeReference::get_bare_identifier () const {
	if (t_name.get_type() == TokenType::Identifier && t_name.get_lexeme().find(to_string(oDot)) == std::string::npos && !b_unsigned && !opt_width &&
	    v_arraySuffixes.empty() && !hasNullability() && !opt_application) {
		return &t_name;
	}
	return nullptr;
}

std::string TypeReference::get_spelling () const {
	std::string s_spelling;
	if (b_unsigned) {
		s_spelling += to_string(kUnsigned);
		s_spelling += ' ';
	}
	s_spelling += t_name.get_lexeme();

	if (opt_width) {
		s_spelling += to_string(oLess);
		s_spelling += opt_width->get_lexeme();
		s_spelling += to_string(oGreater);
	}

	if (opt_application) {
		s_spelling += to_string(oLess);
		const std::vector<TypeReference> &v_arguments = opt_application->arguments;
		for (std::size_t st_index = 0; st_index < v_arguments.size(); ++st_index) {
			if (st_index != 0) {
				s_spelling += to_string(oComma);
				s_spelling += ' ';
			}
			s_spelling += v_arguments[st_index].get_spelling();
		}
		s_spelling += to_string(oGreater);
	}

	if (opt_nullable) {
		s_spelling += to_string(oQuestion);
	}

	for (std::size_t st_index = 0; st_index < v_arraySuffixes.size(); ++st_index) {
		s_spelling += to_string(oBracketOpen);
		if (st_index < vopt_arraySizes.size() && vopt_arraySizes[st_index]) {
			s_spelling += vopt_arraySizes[st_index]->get_lexeme();
		}
		s_spelling += to_string(oBracketClose);
		if (st_index < vopt_arrayNullability.size() && vopt_arrayNullability[st_index]) {
			s_spelling += to_string(oQuestion);
		}
	}

	return s_spelling;
}

}
