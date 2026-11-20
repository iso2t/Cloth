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
	return rt_payload.integer() || rt_payload.floating() || rt_payload == ResolvedType(kBoolean) || rt_payload == ResolvedType(kCharacter) ||
	       (type.nullable() && (analyzer.referenceType(rt_payload) || enumeration(rt_payload) || rt_payload == ResolvedType(kTypeinfo)));
}

[[noreturn]] void invalidDefaultFormatter (const TokenSpan &receiver, const Token &parameter) {
	fail(Code::INVALID_PARAMETER_DEFAULT, receiver, "A default cannot call a formatter through a read-only receiver", &parameter);
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
	if (record) v_formatterDefaults.push_back({ std::move(v_bounds), receiver, &parameter, {} });
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

std::optional<ProviderSelection> BodyChecker::textConversion (const ResolvedType &type) const {
	const ResolvedType           rt_element = formatElement(type);
	ProviderSelection            ps_format{ ProviderKind::Format, type, false };
	const std::vector<Candidate> v_providers = dataProviders(analyzer, rt_element, provider::S_STRING);
	for (const Candidate &c_formatter : v_providers) {
		if (!analyzer.declarations.at(c_formatter.symbol).parameters.empty()) continue;
		const Symbol *sym_owner  = analyzer.declarations.at(c_formatter.symbol).owner;
		ps_format.callsFormatter = true;
		ps_format.formatter      = MethodRef{ c_formatter.symbol, analyzer.substitute(analyzer.ownerType(*sym_owner), c_formatter.arguments) };
		return ps_format;
	}
	if (analyzer.referenceType(rt_element)) {
		ps_format.callsFormatter = analyzer.anyReceiverClass(rt_element, [&] (const ResolvedType &host) {
			return !dataProviders(analyzer, host, provider::S_STRING).empty();
		});
		return ps_format;
	}
	if (enumeration(rt_element) || analyzer.numeric(rt_element) || rt_element == ResolvedType(kBoolean) || rt_element == ResolvedType(kCharacter) ||
	    rt_element == ResolvedType(kString) || rt_element == ResolvedType(kTypeinfo) || rt_element == ResolvedType(kNull))
		return ps_format;
	return {};
}

bool BodyChecker::formatterMayRun (const Expression &receiver, const ResolvedType &type, bool declared) {
	if (!readOnlyDefaultRoot(receiver)) return true;
	const ResolvedType rt_element = formatElement(type);
	if (declared && !rt_element.parameter) invalidDefaultFormatter(receiver.get_span(), pd_default->name);
	analyzer.checkFormatterDefault(rt_element, receiver.get_span(), pd_default->name, record);
	return false;
}

void BodyChecker::applyTextConversion (const Expression &receiver, ProviderSelection &format) {
	if (!format.callsFormatter) return;
	if (!formatterMayRun(receiver, format.receiver, format.formatter.has_value())) {
		format = ProviderSelection{ ProviderKind::Format, format.receiver, false };
		return;
	}
	if (record && format.formatter) {
		format.formatterInstance =
		        analyzer.useCallable(*format.formatter->symbol, analyzer.argumentsOf(format.formatter->owner), context, receiver.get_span());
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
	std::optional<Receiver> opt_receiver = receiver(*node.receiver);
	if (!opt_receiver || !opt_receiver->fallsThrough) return ExpressionInfo::never();
	const std::string  &s_name = node.name.get_name_key();
	const ResolvedType &type   = opt_receiver->type;
	if (type == ResolvedType(kNull))
		throw IntrinsicDiagnostic(Code::EXPLICIT_NULLABLE_TYPE, "A provider needs a typed receiver",
		                          { { node.receiver->get_span(), "no payload type" } });
	const std::function rejectForm = [&] {
		fail(Code::INVALID_PROVIDER_RECEIVER, node.name.get_span(), "Provider '" + node.name.get_lexeme() + "' is unavailable on this receiver form");
	};
	const std::function requirePresent = [&] {
		if (type.nullable() && !opt_receiver->isType)
			fail(Code::NULLABLE_PAYLOAD_ACCESS, node.name.get_span(), "Provider '" + node.name.get_lexeme() + "' requires a present value");
	};
	const std::function selection = [&] (ProviderKind kind, ResolvedType result) {
		return ExpressionInfo{ .types = { std::move(result) }, .provider = ProviderSelection{ kind, type, opt_receiver->isType } };
	};
	const std::vector<Candidate> v_candidates = dataProviders(analyzer, type, s_name);
	if (!v_candidates.empty()) {
		if (opt_receiver->isType) rejectForm();
		if (s_name == provider::S_STRING && !formatterMayRun(*node.receiver, type, true)) {
			// A bound can declare the provider even when no class satisfies the whole intersection.
			bindCall(node.name, node.get_span(), {}, v_candidates);
			return selection(ProviderKind::Format, ResolvedType(kString));
		}
		ExpressionInfo result = invoke(node.name, node.get_span(), {}, v_candidates);
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
		if (hasStorageSize(analyzer, type)) {
			ExpressionInfo result = selection(ProviderKind::StorageSize, ResolvedType(kUsize));
			const unsigned width  = payload == ResolvedType(kBoolean)   ? 8 :
			                        payload == ResolvedType(kCharacter) ? 32 :
			                        payload.width                       ? payload.width :
			                                                              analyzer.program.layout.sizeBits;
			if (!type.nullable() && width >= 8 && width <= 64 && std::has_single_bit(width)) result.numericConstant = countConstant(width / 8);
			return result;
		}
	}
	if (s_name == provider::S_STRING) {
		if (std::optional<ProviderSelection> opt_format = textConversion(type)) {
			if (opt_receiver->isType) rejectForm();
			requirePresent();
			applyTextConversion(*node.receiver, *opt_format);
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
		for (const std::pair<std::string_view, MetadataQuery> &mq_entry : V_QUERIES) {
			if (mq_entry.first != s_name) continue;
			requirePresent();
			if (opt_receiver->isType) rejectForm();
			const MetadataQuery mq_query  = mq_entry.second;
			const bool          b_element = mq_query == MetadataQuery::Element;
			const bool          b_count = mq_query == MetadataQuery::Extent || mq_query == MetadataQuery::Width || mq_query == MetadataQuery::Rank;
			ExpressionInfo      ei_result = selection(ProviderKind::Metadata, ResolvedType(b_element ? kTypeinfo : b_count ? kUsize : kBoolean));
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
					fail(Code::INVALID_METADATA_ACCESS, node.name.get_span(),
					     std::format("Type metadata {}{} requires {}, got '{}'", to_string(oColonColon), s_name, s_requirement,
					                 rt_subject.spelling()));
				if (b_element) ei_result.representedType = rt_subject.element();
			}
			return ei_result;
		}
	}
	fail(Code::UNKNOWN_PROVIDER, node.name.get_span(), "Unknown provider '" + node.name.get_lexeme() + "' on " + type.spelling());
}
}
