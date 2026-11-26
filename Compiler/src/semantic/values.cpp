// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/language/expressions/Access.rst

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ast/decl/enum_declaration.hpp>
#include <ast/expr/expression.hpp>
#include <ast/expr/expressions.hpp>
#include <ast/expr/provider_expressions.hpp>
#include <ast/expr/struct_expressions.hpp>
#include <diagnostics.hpp>
#include <semantic/checked_program.hpp>
#include <semantic/checker.hpp>
#include <semantic/symbol_table.hpp>
#include <token/token.hpp>

namespace cloth::semantic {
namespace {
template <typename Visit> void enumConstant (const Expression &expression, const Visit &visit) {
	if (const auto *cast = dynamic_cast<const CastExpression *>(&expression))
		enumConstant(*cast->operand, visit);
	else if (const auto *array = dynamic_cast<const ArrayLiteral *>(&expression)) {
		for (const ExpressionPtr &ep_item : array->elements) enumConstant(*ep_item, visit);
	} else if (const auto *structure = dynamic_cast<const StructLiteral *>(&expression)) {
		for (const ExpressionPtr &ep_item : structure->fields) enumConstant(*ep_item, visit);
	} else if (const auto *unary = dynamic_cast<const UnaryExpression *>(&expression)) {
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
	IntegerConstant result{ .magnitude = { static_cast<std::uint32_t>(value) } };
	if (value >> 32) result.magnitude.push_back(static_cast<std::uint32_t>(value >> 32));
	return result;
}

std::vector<Candidate> dataProviders (const Analyzer &analyzer, const ResolvedType &receiver, std::string_view name) {
	std::vector<Candidate> v_candidates = analyzer.members(receiver, name, SymbolKind::Operator);
	std::erase_if(v_candidates, [] (const Candidate &candidate) {
		return !dynamic_cast<const MethodDeclaration &>(*callable(*candidate.symbol)).operatorMember->provider;
	});
	return v_candidates;
}

ResolvedType formatElement (ResolvedType type) {
	while (type.nullable() || type.array()) type = type.element();
	return type;
}

bool hasStorageSize (const Analyzer &analyzer, const ResolvedType &type) {
	const ResolvedType rt_payload = type.nullable() ? type.element() : type;
	return rt_payload.integer() || rt_payload.floating() || rt_payload == ResolvedType(KBoolean) || rt_payload == ResolvedType(KCharacter) ||
	       (type.nullable() && (analyzer.referenceType(rt_payload) || enumeration(rt_payload) || rt_payload == ResolvedType(KTypeinfo)));
}

[[noreturn]] void invalidDefaultFormatter (const TokenSpan &receiver, const Token &parameter) {
	fail(Code::InvalidParameterDefault, receiver, "A default cannot call a formatter through a read-only receiver", &parameter);
}
}

void Analyzer::collectFormatterClasses () {
	if (!hasOperator(provider::S_STRING)) return;
	for (const Symbol *sym_class : v_order) {
		if (!asClass(sym_class)) continue;
		ResolvedType rt_class = ownerType(*sym_class);
		if (!dataProviders(*this, rt_class, provider::S_STRING).empty()) s_formatterClasses.insert(std::move(rt_class));
	}
}

void Analyzer::useFormatterClass (const ResolvedType &type) {
	if (s_formatterClasses.empty() || !type.declaration || !s_formatterClasses.contains(ownerType(*type.declaration))) return;
	s_formatterClasses.insert(formatElement(type));
}

bool Analyzer::hasFormatterClass (const ResultTypes &bounds) const {
	return std::ranges::any_of(s_formatterClasses, [&] (const ResolvedType &host) {
		return std::ranges::all_of(bounds, [&] (const ResolvedType &bound) { return accepts(bound, host); });
	});
}

void Analyzer::checkFormatterDefault (const ResolvedType &element, const TokenSpan &receiver, const Token &parameter, bool record) {
	ResultTypes v_bounds = element.parameter ? typeBounds(element) : ResultTypes{ element };
	if (v_bounds.empty()) return;
	if (hasFormatterClass(v_bounds)) invalidDefaultFormatter(receiver, parameter);
	// Later bodies and generic instances can introduce another formatter for these bounds.
	if (record) v_formatterDefaults.push_back({ .bounds = std::move(v_bounds), .receiver = receiver, .parameter = &parameter, .followingError = {} });
}

bool Analyzer::checkDefaultExpression (const std::function<bool()> &check) {
	const std::size_t st_before = v_formatterDefaults.size();
	try {
		return check();
	} catch (const cde::ClothDiagnostic &) {
		if (v_formatterDefaults.size() == st_before) throw;
		// Resolve the earlier formatter access before reporting a subsequent error in this default.
		v_formatterDefaults.back().followingError = std::current_exception();
		return false;
	}
}

void Analyzer::checkFormatterDefaults () const {
	for (const FormatterDefault &fd_use : v_formatterDefaults) {
		if (hasFormatterClass(fd_use.bounds)) invalidDefaultFormatter(fd_use.receiver, *fd_use.parameter);
		if (fd_use.followingError) std::rethrow_exception(fd_use.followingError);
	}
}

std::optional<MethodRef> Analyzer::formatterFor (const ResolvedType &type) const {
	for (const Candidate &candidate : dataProviders(*this, type, provider::S_STRING)) {
		const auto &signature = declarations.at(candidate.symbol);
		if (signature.parameters.empty())
			return MethodRef{ .symbol = candidate.symbol, .owner = substitute(ownerType(*signature.owner), candidate.arguments) };
	}
	return {};
}

std::optional<ProviderSelection> BodyChecker::textConversion (const ResolvedType &type) const {
	const ResolvedType           rt_element = formatElement(type);
	ProviderSelection            ps_format{ .kind = ProviderKind::Format, .receiver = type, .typeReceiver = false };
	ps_format.formatter = analyzer.formatterFor(rt_element);
	if (ps_format.formatter) {
		ps_format.callsFormatter = true;
		return ps_format;
	}
	if (analyzer.referenceType(rt_element)) {
		ps_format.callsFormatter = analyzer.anyReceiverClass(rt_element, [&] (const ResolvedType &host) {
			return !dataProviders(analyzer, host, provider::S_STRING).empty();
		});
		return ps_format;
	}
	if (enumeration(rt_element) || analyzer.numeric(rt_element) || rt_element == ResolvedType(KBoolean) || rt_element == ResolvedType(KCharacter) ||
	    rt_element == ResolvedType(KString) || rt_element == ResolvedType(KTypeinfo) || rt_element == ResolvedType(KNull))
		return ps_format;
	return {};
}

bool BodyChecker::formatterMayRun (const Expression &receiver, const ResolvedType &type, bool declared) {
	if (!readOnlyDefaultRoot(receiver)) return true;
	const ResolvedType rt_element = formatElement(type);
	if (declared && !rt_element.parameter) invalidDefaultFormatter(receiver.getSpan(), pd_default->name);
	analyzer.checkFormatterDefault(rt_element, receiver.getSpan(), pd_default->name, record);
	return false;
}

void BodyChecker::applyTextConversion (const Expression &receiver, ProviderSelection &format) {
	if (!format.callsFormatter) return;
	if (!formatterMayRun(receiver, format.receiver, format.formatter.has_value())) {
		format = ProviderSelection{ .kind = ProviderKind::Format, .receiver = format.receiver, .typeReceiver = false };
		return;
	}
	if (record && format.formatter) {
		format.formatterInstance =
		        analyzer.useCallable(*format.formatter->symbol, analyzer.argumentsOf(format.formatter->owner), context, receiver.getSpan());
	}
	invalidateShared();
}

bool BodyChecker::bindText (const Expression &operand, const ResolvedType &type) {
	std::optional<ProviderSelection> opt_format = textConversion(type);
	if (!opt_format) return false;
	applyTextConversion(operand, *opt_format);
	if (record) bf_facts.textConversions.insert_or_assign(&operand, *opt_format);
	return true;
}

void BodyChecker::enumValue (const EnumFieldDeclaration &field, const Expression &value, const Token *caseName) {
	const std::string   subject = std::format("{} for '{}{}{}'", caseName ? "Case '" + caseName->getLexeme() + "'" : "Default", owner->name.getLexeme(),
	                                          toString(ODot), field.name.getLexeme());
	const std::function error   = [&] (const Expression &expression, std::string message, Code code) {
                std::vector<cde::SourceLabel> v_labels{ { .span = expression.getSpan(), .text = message },
			                                  { .span = field.type.getSpan(), .text = "payload field type declared here" } };
                if (caseName) v_labels.push_back({ .span = caseName->getSpan(), .text = "enum case declared here" });
                throw cde::ClothDiagnostic(code, subject + " " + message, std::move(v_labels));
	};
	enumConstant(value, [&] (const Expression &expression, bool valid) {
		if (!valid) error(expression, "requires a constant literal or explicit cast", Code::InvalidEnumMember);
	});
	const ResolvedType                rt_required = bodyType(field.type);
	const std::optional<ResolvedType> opt_actual  = scalar(value, &rt_required);
	if (!opt_actual || !analyzer.accepts(rt_required, *opt_actual))
		error(value, "requires " + rt_required.spelling() + ", found " + (opt_actual ? opt_actual->spelling() : "no value"), Code::InvalidEnumMember);
	if (record)
		enumConstant(value, [&] (const Expression &expression, bool) {
			if (bf_facts.expressions.at(&expression).invalidNumericCast)
				error(expression, "contains a numeric conversion outside the destination type", Code::InvalidCast);
		});
}

ExpressionInfo BodyChecker::providerAccess (const ProviderAccessExpression &node) {
	std::optional<Receiver> opt_receiver = receiver(*node.receiver);
	if (!opt_receiver || !opt_receiver->fallsThrough) return ExpressionInfo::never();
	const std::string  &s_name = node.name.getNameKey();
	const ResolvedType &type   = opt_receiver->type;
	if (type == ResolvedType(KNull))
		throw IntrinsicDiagnostic(Code::ExplicitNullableType, "A provider needs a typed receiver",
		                          { { .span = node.receiver->getSpan(), .text = "no payload type" } });
	const std::function rejectForm = [&] {
		fail(Code::InvalidProviderReceiver, node.name.getSpan(), "Provider '" + node.name.getLexeme() + "' is unavailable on this receiver form");
	};
	const std::function requirePresent = [&] {
		if (type.nullable() && !opt_receiver->isType)
			fail(Code::NullablePayloadAccess, node.name.getSpan(), "Provider '" + node.name.getLexeme() + "' requires a present value");
	};
	const std::function selection = [&] (ProviderKind kind, ResolvedType result) {
		return ExpressionInfo{ .types    = { std::move(result) },
			               .provider = ProviderSelection{ .kind = kind, .receiver = type, .typeReceiver = opt_receiver->isType } };
	};
	const std::vector<Candidate> v_candidates = dataProviders(analyzer, type, s_name);
	if (!v_candidates.empty()) {
		if (opt_receiver->isType) rejectForm();
		if (s_name == provider::S_STRING && !formatterMayRun(*node.receiver, type, true)) {
			// A bound can declare the provider even when no class satisfies the whole intersection.
			bindCall(node.name, node.getSpan(), {}, v_candidates);
			return selection(ProviderKind::Format, ResolvedType(KString));
		}
		ExpressionInfo result = invoke(node.name, node.getSpan(), {}, v_candidates);
		mutatingReceiver(*node.receiver, type);
		return result;
	}
	// Built-in metadata and storage size describe the declared storage even on a narrowed path.
	if (!opt_receiver->isType && (s_name == provider::S_TYPE || s_name == provider::S_SIZE)) {
		const std::optional<TypedPlace> tp_place = locate(*node.receiver);
		if (tp_place && tp_place->type != type &&
		    (s_name == provider::S_TYPE || (tp_place->type.nullable() && hasStorageSize(analyzer, tp_place->type)))) {
			opt_receiver->type = tp_place->type;
			if (record) bf_facts.expressions.at(node.receiver.get()).types.front() = tp_place->type;
		}
	}
	const ResolvedType payload = type.nullable() ? type.element() : type;
	if (s_name == provider::S_TYPE) {
		ExpressionInfo result  = selection(ProviderKind::Type, ResolvedType(KTypeinfo));
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
			ExpressionInfo result = selection(ProviderKind::ArraySize, ResolvedType(KUsize));
			if (const std::optional<std::uint64_t> opt_extent = payload.arrayExtent())
				result.numericConstant = countConstant(*opt_extent);
			else if (opt_receiver->isType)
				rejectForm();
			return result;
		}
		if (payload == ResolvedType(KString)) {
			requirePresent();
			if (opt_receiver->isType) rejectForm();
			return selection(ProviderKind::StringSize, ResolvedType(KUsize));
		}
		if (hasStorageSize(analyzer, type)) {
			ExpressionInfo result = selection(ProviderKind::StorageSize, ResolvedType(KUsize));
			if (!type.nullable()) result.numericConstant = countConstant(analyzer.program.storageLayout(type).value().size);
			return result;
		}
	}
	if (s_name == provider::S_STRING) {
		if (std::optional<ProviderSelection> opt_format = textConversion(type)) {
			if (opt_receiver->isType) rejectForm();
			requirePresent();
			applyTextConversion(*node.receiver, *opt_format);
			return { .types = { ResolvedType(KString) }, .provider = opt_format };
		}
	}
	if (const EnumDeclaration *ed_enum = enumeration(payload); ed_enum && (s_name == provider::S_NAME || s_name == provider::S_VALUES)) {
		requirePresent();
		if (s_name == provider::S_NAME) {
			if (opt_receiver->isType) rejectForm();
			return selection(ProviderKind::EnumName, ResolvedType(KString));
		}
		if (!opt_receiver->isType) rejectForm();
		ResolvedType values = payload;
		values.suffixes.push_back(arraySuffix(std::to_string(ed_enum->cases.size())));
		return selection(ProviderKind::EnumValues, std::move(values));
	}
	if (payload == ResolvedType(KTypeinfo)) {
		using namespace provider;
		static constexpr std::pair<std::string_view, MetadataQuery> V_QUERIES[] = {
			{ S_ELEMENT, MetadataQuery::Element },       { S_EXTENT, MetadataQuery::Extent },
			{ S_WIDTH, MetadataQuery::Width },           { S_RANK, MetadataQuery::Rank },
			{ S_ISARRAY, MetadataQuery::IsArray },       { S_HASEXTENT, MetadataQuery::HasExtent },
			{ S_ISUNSIGNED, MetadataQuery::IsUnsigned }, { S_ISNUMERIC, MetadataQuery::IsNumeric }
		};
		for (const std::pair<std::string_view, MetadataQuery> &mq_entry : V_QUERIES) {
			if (mq_entry.first != s_name) continue;
			requirePresent();
			if (opt_receiver->isType) rejectForm();
			const MetadataQuery mq_query  = mq_entry.second;
			const bool          b_element = mq_query == MetadataQuery::Element;
			const bool          b_count = mq_query == MetadataQuery::Extent || mq_query == MetadataQuery::Width || mq_query == MetadataQuery::Rank;
			ExpressionInfo      ei_result = selection(ProviderKind::Metadata, ResolvedType(b_element ? KTypeinfo : b_count ? KUsize : KBoolean));
			ei_result.provider->metadata  = mq_query;
			if (opt_receiver->represented) {
				const ResolvedType rt_subject = opt_receiver->represented->nullable() ? opt_receiver->represented->element() :
				                                                                        *opt_receiver->represented;
				std::string_view   s_requirement;
				if (b_element && !rt_subject.array())
					s_requirement = "an array type";
				else if (mq_query == MetadataQuery::Extent && (!rt_subject.array() || rt_subject.suffixes.back() == arraySuffix()))
					s_requirement = "an array type with a statically known outer extent";
				else if (mq_query == MetadataQuery::Width && !rt_subject.integer() && !rt_subject.floating())
					s_requirement = "an integer or floating-point type";
				if (!s_requirement.empty())
					fail(Code::InvalidMetadataAccess, node.name.getSpan(),
					     std::format("Type metadata {}{} requires {}, got '{}'", toString(OColonColon), s_name, s_requirement,
					                 rt_subject.spelling()));
				if (b_element) ei_result.representedType = rt_subject.element();
			}
			return ei_result;
		}
	}
	fail(Code::UnknownProvider, node.name.getSpan(), "Unknown provider '" + node.name.getLexeme() + "' on " + type.spelling());
}
}
