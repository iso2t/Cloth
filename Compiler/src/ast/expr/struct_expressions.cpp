// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <ast/expr/expression.hpp>
#include <ast/expr/expressions.hpp>
#include <ast/expr/struct_expressions.hpp>
#include <token/token.hpp>

namespace cloth {

std::optional<Token> typeNameCandidate (const Expression &expression, const std::function<bool(const Token &)> &acceptRoot) {
	const Expression          *e_target = &expression;
	std::vector<const Token *> v_suffixes;
	while (const FieldAccessExpression *fa_field = dynamic_cast<const FieldAccessExpression *>(e_target)) {
		v_suffixes.push_back(&fa_field->name);
		e_target = fa_field->receiver.get();
	}

	const NameExpression *ne_root = dynamic_cast<const NameExpression *>(e_target);
	if (!ne_root || ne_root->name.get_type() != TokenType::Identifier || !acceptRoot(ne_root->name)) {
		return std::nullopt;
	}
	if (v_suffixes.empty()) {
		return ne_root->name;
	}

	std::string s_spelling = ne_root->name.get_lexeme();
	for (std::size_t st_remaining = v_suffixes.size(); st_remaining != 0; --st_remaining) {
		s_spelling += cloth::to_string(Operator::o_DOT);
		s_spelling += v_suffixes[st_remaining - 1]->get_lexeme();
	}
	return Token(std::move(s_spelling), TokenType::Identifier, expression.get_span());
}

bool hasFieldTarget (const Expression &expression) {
	const Expression *e_target = &expression;
	while (const IndexExpression *ie_index = dynamic_cast<const IndexExpression *>(e_target)) {
		e_target = ie_index->array.get();
	}
	return dynamic_cast<const FieldAccessExpression *>(e_target) != nullptr;
}

}
