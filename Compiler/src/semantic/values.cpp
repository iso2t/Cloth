// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <bit>
#include <format>

#include <semantic/checker.hpp>

namespace cloth::semantic {
namespace {
template <typename Visit> void enumConstant (const Expression &expression, const Visit &visit) {
	if (const CastExpression *cast = dynamic_cast<const CastExpression *>(&expression))
		enumConstant(*cast->operand, visit);
	else if (const ArrayLiteral *array = dynamic_cast<const ArrayLiteral *>(&expression)) {
		for (const ExpressionPtr &ep_item : array->elements) enumConstant(*ep_item, visit);
	} else if (const StructLiteral *structure = dynamic_cast<const StructLiteral *>(&expression)) {
		for (const ExpressionPtr &ep_item : structure->fields) enumConstant(*ep_item, visit);
	} else if (const UnaryExpression *unary = dynamic_cast<const UnaryExpression *>(&expression)) {
		if (!dynamic_cast<const NumberLiteral *>(unary->operand.get()) && !dynamic_cast<const BooleanLiteral *>(unary->operand.get()))
			visit(expression, false);
	} else if (!dynamic_cast<const NumberLiteral *>(&expression) && !dynamic_cast<const BooleanLiteral *>(&expression) &&
	           !dynamic_cast<const CharacterLiteral *>(&expression) && !dynamic_cast<const StringLiteral *>(&expression) &&
	           !dynamic_cast<const NullLiteral *>(&expression)) {
		visit(expression, false);
	}
	visit(expression, true);
}

IntegerConstant countConstant (std::uint64_t value) {
	IntegerConstant result{ { static_cast<std::uint32_t>(value) } };
	if (value >> 32) result.magnitude.push_back(static_cast<std::uint32_t>(value >> 32));
	return result;
}

const EnumDeclaration *enumeration (const ResolvedType &type) {
	return type.declaration && type.suffixes.empty() ?
	               dynamic_cast<const EnumDeclaration *>(std::get<const TypeDeclaration *>(type.declaration->declaration)) :
	               nullptr;
}

std::vector<Candidate> dataProviders (const Analyzer &analyzer, const ResolvedType &receiver, std::string_view name) {
	std::vector<Candidate> v_candidates = analyzer.members(receiver, name, SymbolKind::Operator);
	std::erase_if(v_candidates, [] (const Candidate &candidate) {
		return !dynamic_cast<const MethodDeclaration &>(*callable(*candidate.symbol)).operatorMember->provider;
	});
	return v_candidates;
}
}

std::optional<ProviderSelection> BodyChecker::textConversion (const ResolvedType &type, const TokenSpan &use) {
	ResolvedType rt_element = type;
	while (rt_element.nullable() || rt_element.array()) rt_element = rt_element.element();
	ProviderSelection            ps_format{ ProviderKind::Format, type, false };
	const std::vector<Candidate> v_providers = dataProviders(analyzer, rt_element, provider::S_STRING);
	for (const Candidate &c_formatter : v_providers) {
		if (!analyzer.declarations.at(c_formatter.symbol).parameters.empty()) continue;
		const Symbol *sym_owner  = analyzer.declarations.at(c_formatter.symbol).owner;
		ps_format.callsFormatter = true;
		ps_format.formatter      = MethodRef{ c_formatter.symbol, analyzer.substitute(analyzer.ownerType(*sym_owner), c_formatter.arguments) };
		if (record) ps_format.formatterInstance = analyzer.useCallable(*c_formatter.symbol, c_formatter.arguments, context, use);
		return ps_format;
	}
	ps_format.callsFormatter = analyzer.referenceType(rt_element);
	if (ps_format.callsFormatter || enumeration(rt_element) || rt_element.integer() || rt_element.floating() || rt_element == ResolvedType(kBoolean) ||
	    rt_element == ResolvedType(kCharacter) || rt_element == ResolvedType(kString) || rt_element == ResolvedType(kTypeinfo) ||
	    rt_element == ResolvedType(kNull))
		return ps_format;
	return {};
}

void BodyChecker::enumValue (const EnumFieldDeclaration &field, const Expression &value, const Token *caseName) {
	const std::string   subject = std::format("{} for '{}.{}'", caseName ? "Case '" + caseName->get_lexeme() + "'" : "Default", owner->name.get_lexeme(),
	                                          field.name.get_lexeme());
	const std::function error   = [&] (const Expression &expression, std::string message, Code code) {
		std::vector<cde::SourceLabel> v_labels{ { expression.get_span(), message }, { field.type.get_span(), "payload field type declared here" } };
		if (caseName) v_labels.push_back({ caseName->get_span(), "enum case declared here" });
		throw cde::ClothDiagnostic(code, subject + " " + message, std::move(v_labels));
	};
	enumConstant(value, [&] (const Expression &expression, bool valid) {
		if (!valid) error(expression, "requires a constant literal or explicit cast", Code::INVALID_ENUM_MEMBER);
	});
	const ResolvedType                rt_required = bodyType(field.type);
	const std::optional<ResolvedType> opt_actual  = scalar(value, &rt_required);
	if (!opt_actual || !analyzer.accepts(rt_required, *opt_actual))
		error(value, "requires " + rt_required.spelling() + ", found " + (opt_actual ? opt_actual->spelling() : "no value"), Code::INVALID_ENUM_MEMBER);
	if (record)
		enumConstant(value, [&] (const Expression &expression, bool) {
			if (bf_facts.expressions.at(&expression).invalidNumericCast)
				error(expression, "contains a numeric conversion outside the destination type", Code::INVALID_CAST);
		});
}

ExpressionInfo BodyChecker::providerAccess (const ProviderAccessExpression &node) {
	const std::optional<Receiver> opt_receiver = receiver(*node.receiver);
	if (!opt_receiver || !opt_receiver->fallsThrough) return ExpressionInfo::never();
	const ResolvedType &type       = opt_receiver->type;
	const ResolvedType  payload    = type.nullable() ? type.element() : type;
	const std::string  &s_name     = node.name.get_lexeme();
	const std::function rejectForm = [&] {
		fail(Code::INVALID_PROVIDER_RECEIVER, node.name.get_span(), "Provider '" + s_name + "' is unavailable on this receiver form");
	};
	const std::function requirePresent = [&] {
		if (type.nullable() && !opt_receiver->isType)
			fail(Code::NULLABLE_PAYLOAD_ACCESS, node.name.get_span(), "Provider '" + s_name + "' requires a present value");
	};
	const std::function selection = [&] (ProviderKind kind, ResolvedType result) {
		return ExpressionInfo{ .types = { std::move(result) }, .provider = ProviderSelection{ kind, type, opt_receiver->isType } };
	};
	const std::vector<Candidate> v_candidates = dataProviders(analyzer, type, s_name);
	if (!v_candidates.empty()) {
		if (opt_receiver->isType) rejectForm();
		ExpressionInfo result = invoke(node.name, node.get_span(), {}, v_candidates);
		mutatingReceiver(*node.receiver, type);
		return result;
	}
	if (s_name == provider::S_TYPE && type != ResolvedType(kVoid) && type != ResolvedType(kNull)) {
		ExpressionInfo result  = selection(ProviderKind::Type, ResolvedType(kTypeinfo));
		result.representedType = type;
		return result;
	}
	if ((s_name == provider::S_MIN || s_name == provider::S_MAX) && (payload.integer() || payload.floating())) {
		requirePresent();
		const bool     b_maximum = s_name == provider::S_MAX;
		ExpressionInfo result    = selection(b_maximum ? ProviderKind::Maximum : ProviderKind::Minimum, payload);
		result.numericConstant   = numericLimit(payload, analyzer.program.layout, b_maximum);
		return result;
	}
	if (s_name == provider::S_SIZE) {
		if (payload.array()) {
			requirePresent();
			ExpressionInfo     result = selection(ProviderKind::ArraySize, ResolvedType(kUsize));
			const std::string &extent = payload.suffixes.back();
			if (extent == arraySuffix()) {
				if (opt_receiver->isType) rejectForm();
			} else
				result.numericConstant = countConstant(std::stoull(extent.substr(to_string(oBracketOpen).size())));
			return result;
		}
		if (payload == ResolvedType(kString)) {
			requirePresent();
			if (opt_receiver->isType) rejectForm();
			return selection(ProviderKind::StringSize, ResolvedType(kUsize));
		}
		if (payload.integer() || payload.floating() || payload == ResolvedType(kBoolean) || payload == ResolvedType(kCharacter) ||
		    (type.nullable() && (analyzer.referenceType(payload) || enumeration(payload) || payload == ResolvedType(kTypeinfo)))) {
			ExpressionInfo result = selection(ProviderKind::StorageSize, ResolvedType(kUsize));
			const unsigned width  = payload == ResolvedType(kBoolean)   ? 8 :
			                        payload == ResolvedType(kCharacter) ? 32 :
			                        payload.width                       ? payload.width :
			                                                              analyzer.program.layout.sizeBits;
			if (!type.nullable() && width >= 8 && width <= 64 && std::has_single_bit(width)) result.numericConstant = countConstant(width / 8);
			return result;
		}
	}
	if (s_name == provider::S_STRING && type != ResolvedType(kNull)) {
		if (const std::optional<ProviderSelection> opt_format = textConversion(type, node.get_span())) {
			if (opt_receiver->isType) rejectForm();
			requirePresent();
			if (opt_format->callsFormatter) invalidateShared();
			return { .types = { ResolvedType(kString) }, .provider = opt_format };
		}
	}
	if (const EnumDeclaration *ed_enum = enumeration(payload); ed_enum && (s_name == provider::S_NAME || s_name == provider::S_VALUES)) {
		requirePresent();
		if (s_name == provider::S_NAME) {
			if (opt_receiver->isType) rejectForm();
			return selection(ProviderKind::EnumName, ResolvedType(kString));
		}
		if (!opt_receiver->isType) rejectForm();
		ResolvedType values = payload;
		values.suffixes.push_back(arraySuffix(std::to_string(ed_enum->cases.size())));
		return selection(ProviderKind::EnumValues, std::move(values));
	}
	if (payload == ResolvedType(kTypeinfo)) {
		using namespace provider;
		static constexpr std::pair<std::string_view, MetadataQuery> V_QUERIES[] = {
			{ S_ELEMENT, MetadataQuery::Element },       { S_EXTENT, MetadataQuery::Extent },
			{ S_WIDTH, MetadataQuery::Width },           { S_RANK, MetadataQuery::Rank },
			{ S_ISARRAY, MetadataQuery::IsArray },       { S_HASEXTENT, MetadataQuery::HasExtent },
			{ S_ISUNSIGNED, MetadataQuery::IsUnsigned }, { S_ISNUMERIC, MetadataQuery::IsNumeric }
		};
		const bool element = s_name == S_ELEMENT, extent = s_name == S_EXTENT, width = s_name == S_WIDTH;
		for (const std::pair<std::string_view, MetadataQuery> &entry : V_QUERIES) {
			if (entry.first != s_name) continue;
			requirePresent();
			if (opt_receiver->isType) rejectForm();
			ExpressionInfo result     = selection(ProviderKind::Metadata, ResolvedType(element                               ? kTypeinfo :
			                                                                           (extent || width || s_name == S_RANK) ? kUsize :
			                                                                                                                   kBoolean));
			result.provider->metadata = entry.second;
			if (opt_receiver->represented && !opt_receiver->represented->dependent()) {
				const ResolvedType subject = opt_receiver->represented->nullable() ? opt_receiver->represented->element() :
				                                                                     *opt_receiver->represented;
				if (record && ((element && !subject.array()) || (extent && (!subject.array() || subject.suffixes.back() == arraySuffix())) ||
				               (width && !subject.integer() && !subject.floating())))
					fail(Code::INVALID_METADATA_ACCESS, node.name.get_span(),
					     "Cannot query '" + s_name + "' on metadata for " + subject.spelling());
				if (element && subject.array()) result.representedType = subject.element();
			}
			return result;
		}
	}
	fail(Code::UNKNOWN_PROVIDER, node.name.get_span(), "Unknown provider '" + s_name + "' on " + type.spelling());
}
}
