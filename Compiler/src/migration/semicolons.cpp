// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/Flags.rst

#include <algorithm>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ast/source_unit.hpp>
#include <cde/cloth_diagnostic.hpp>
#include <file/source_file.hpp>
#include <lexer/lexer.hpp>
#include <migration/semicolons.hpp>
#include <migration/structure.hpp>
#include <parser/parser.hpp>
#include <token/token.hpp>

namespace cloth::migration {
namespace {

struct Attempt {
	std::optional<ParsedSource> parsed;
	std::string                 error;
};

Attempt parse (const std::vector<Token> &tokens, SourceGrammar grammar, bool capture = false) {
	try {
		Parser parser(tokens, grammar);
		return { .parsed = capture ? parser.parseWithRanges() : ParsedSource{ .unit = parser.parse(), .ranges = {} }, .error = {} };
	} catch (const cde::ClothDiagnostic &error) {
		const auto &labels = error.getSourceLabels();
		return { .parsed = std::nullopt, .error = labels.empty() ? error.getMessage() : labels.front().span.toString() + " " + error.getMessage() };
	}
}

enum class Wrapper { None, Braces, Parentheses };

struct Ending {
	std::size_t begin;
	Wrapper     wrapper;
	bool        terminateInside;
	bool        terminateAfter;
};

struct Edits {
	std::vector<Ending>                          endings;
	std::vector<std::pair<std::size_t, Wrapper>> openings;
	bool                                         removeSemicolon = false;
};

class Rewrite {
    public:
	Rewrite (const std::string &original, const std::vector<Token> &tokens, const std::vector<SourceRange> &ranges) : original(original) {
		for (std::size_t i = 0; i < original.size(); ++i) {
			if (original[i] == '\n') lines.push_back(i + 1);
		}
		for (const Token &token : tokens) {
			if (token.getType() != TokenType::NewLine) significant.emplace(begin(token.getSpan()), &token);
		}
		for (const auto &range : ranges) {
			if (range.construct == SourceConstruct::ForSeparator) protectedSeparators.insert(begin(range.span));
			if (range.construct == SourceConstruct::Conditional) conditionals.emplace(begin(range.span), end(range.span));
		}
		// The opening keyword precedes all its arms, including an unbraced nested if.
		for (const auto &range : ranges) plan(range);
	}

	std::string render () {
		for (const auto &[pos, token] : significant) {
			if (isSemicolon(*token) && !protectedSeparators.contains(pos)) edits[pos].removeSemicolon = true;
		}
		std::size_t copied = 0;
		std::string result;
		for (auto &[pos, edit] : edits) {
			result.append(original, copied, pos - copied);
			// At a shared endpoint, finish an inner arm before closing its parent.
			std::ranges::sort(edit.endings, [] (const Ending &a, const Ending &b) {
				if (a.begin != b.begin) return a.begin > b.begin;
				return a.wrapper > b.wrapper;
			});
			std::string suffix;
			for (const auto &ending : edit.endings) {
				if (ending.terminateInside && !suffix.ends_with(';')) suffix += ';';
				if (ending.wrapper == Wrapper::Braces) suffix += " }";
				if (ending.wrapper == Wrapper::Parentheses) suffix += ')';
				if (ending.terminateAfter && !suffix.ends_with(';')) suffix += ';';
			}
			const auto next = significant.lower_bound(pos);
			if (suffix.ends_with(';') && next != significant.end() && isSemicolon(*next->second) && !protectedSeparators.contains(next->first) &&
			    std::string_view(original).substr(pos, next->first - pos).find_first_not_of(" \t") == std::string_view::npos) {
				suffix.pop_back();
				edits[next->first].removeSemicolon = false;
			}
			result += suffix;
			std::ranges::sort(edit.openings, std::greater{});
			for (const auto &[close, wrapper] : edit.openings) result += wrapper == Wrapper::Braces ? "{ " : "(";
			copied = pos + static_cast<std::size_t>(edit.removeSemicolon);
		}
		result.append(original, copied);
		return result;
	}

    private:
	const std::string                   &original;
	std::vector<std::size_t>             lines{ 0 };
	std::map<std::size_t, const Token *> significant;
	std::set<std::size_t>                protectedSeparators;
	std::map<std::size_t, std::size_t>   conditionals;
	std::set<std::size_t>                statementConditionals;
	std::map<std::size_t, Edits>         edits;

	std::size_t begin (const TokenSpan &span) const {
		return lines.at(span.startLine - 1) + span.startCol - 1;
	}

	std::size_t end (const TokenSpan &span) const {
		return lines.at(span.endLine - 1) + span.endCol - 1;
	}

	static bool isSemicolon (const Token &token) {
		return token.getType() == TokenType::Operator && token.getOperator() == OSemicolon;
	}

	void enclose (std::size_t start, std::size_t last, Wrapper wrapper, bool inside = false, bool after = false) {
		if (wrapper != Wrapper::None) edits[start].openings.emplace_back(last, wrapper);
		edits[last].endings.push_back({ .begin = start, .wrapper = wrapper, .terminateInside = inside, .terminateAfter = after });
	}

	bool followingOperator (std::size_t last) const {
		auto it = significant.lower_bound(last);
		while (it != significant.end() && isSemicolon(*it->second)) ++it;
		if (it == significant.end() || it->second->getType() != TokenType::Operator) return false;
		const Operator op = it->second->getOperator();
		return op != OBraceClose && op != OBracketClose && op != OParenClose && op != OComma;
	}

	void plan (const SourceRange &range) {
		const auto start = begin(range.span), last = end(range.span);
		const bool arm       = range.construct == SourceConstruct::ConditionalArm;
		const bool statement = range.construct == SourceConstruct::Statement ||
		                       (range.conditional && statementConditionals.contains(begin(*range.conditional)));
		if (arm && !statement) {
			// Single value arms take their delimiter from the enclosing expression. Other
			// arms need their own scope so that a comma or return stays inside the branch.
			if (range.form == WrittenForm::Results || range.form == WrittenForm::Simple)
				enclose(start, last, Wrapper::Braces, true);
			else if (range.form == WrittenForm::SelfTerminated)
				enclose(start, last, Wrapper::Braces);
			return;
		}
		if (statement && conditionals.contains(start)) {
			if (range.form == WrittenForm::BareConditional && !followingOperator(last)) {
				statementConditionals.insert(start);
				return;
			}
			// Parentheses keep a leading conditional in value context, or separate a braced
			// statement from a following prefix operator that strict parsing could absorb.
			enclose(start, conditionals.at(start), Wrapper::Parentheses);
		}
		if (range.form != WrittenForm::SelfTerminated) enclose(start, last, Wrapper::None, false, true);
	}
};
}

std::optional<std::string> migrateSemicolons (const SourceFile &source, const InputDialect dialect) {
	const auto tokens = Lexer(source).scanTokens();
	const auto legacy = parse(tokens, SourceGrammar::Legacy, true);
	const auto strict = parse(tokens, SourceGrammar::ExplicitSemicolons);
	if (!legacy.parsed) {
		if (strict.parsed) return std::nullopt;
		throw std::runtime_error("Cannot parse source. Legacy: " + legacy.error + ". Strict: " + strict.error);
	}
	const SourceUnit &reference = legacy.parsed->unit;
	if (strict.parsed) {
		const auto difference = firstParseDifference(reference, strict.parsed->unit);
		if (!difference) return std::nullopt;
		if (dialect == InputDialect::Unknown)
			throw std::runtime_error(difference->toString() +
			                         " Ambiguous source: legacy and strict parses differ. Use --from-legacy for known legacy input.");
	}
	std::string replacement = Rewrite(source.getContents(), tokens, legacy.parsed->ranges).render();
	const auto  proposed    = Lexer(SourceFile(source.getPath(), replacement, source.getExtension())).scanTokens();
	const auto  verified    = parse(proposed, SourceGrammar::ExplicitSemicolons);
	if (!verified.parsed) throw std::runtime_error("Proposed rewrite failed strict parsing: " + verified.error);
	if (const auto difference = firstParseDifference(reference, verified.parsed->unit))
		throw std::runtime_error(difference->toString() + " Proposed rewrite changes the legacy parse");
	// A second known-legacy run must also accept the bytes without attempting a new rewrite.
	const auto repeated = parse(proposed, SourceGrammar::Legacy);
	if (repeated.parsed) {
		if (const auto difference = firstParseDifference(reference, repeated.parsed->unit))
			throw std::runtime_error(difference->toString() + " Rewritten source would be ambiguous on a second run");
	}
	return replacement;
}
}
