#include <token/Token.hpp>

std::string_view to_string(TokenType type) {
	switch (type) {
	case EndOfFile:
		return "EndOfFile";
	case Unknown:
		return "Unknown";
	case NewLine:
		return "NewLine";
	case Identifier:
		return "Identifier";
	case Keyword:
		return "Keyword";
	case Operator:
		return "Operator";
	case FloatingPointLiteral:
		return "FloatingPointLiteral";
	case IntegerLiteral:
		return "IntegerLiteral";
	case StringLiteral:
		return "StringLiteral";
	case RawStringLiteral:
		return "RawStringLiteral";
	case CharacterLiteral:
		return "CharacterLiteral";
	case InterpolationStart:
		return "InterpolationStart";
	case StringSegment:
		return "StringSegment";
	case InterpolationOpen:
		return "InterpolationOpen";
	case InterpolationClose:
		return "InterpolationClose";
	case InterpolationEnd:
		return "InterpolationEnd";
	}
	return "Invalid";
}
