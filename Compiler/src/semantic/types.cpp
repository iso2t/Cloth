// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <format>
#include <functional>
#include <set>

#include <ast/decl/enum_declaration.hpp>
#include <ast/decl/interface_declaration.hpp>
#include <semantic/checker.hpp>
#include <token/numeric_literals.hpp>
#include <token/token.hpp>

namespace cloth::semantic {
namespace {

template <typename Matches> bool hasTypeView (const Analyzer &analyzer, const ResolvedType &actual, Matches matches) {
	std::set<ResolvedType> s_seen;
	ResultTypes            v_pending{ actual };
	while (!v_pending.empty()) {
		const ResolvedType rt_type = std::move(v_pending.back());
		v_pending.pop_back();
		if (!s_seen.insert(rt_type).second) continue;
		if (matches(rt_type)) return true;
		const Substitutions m_arguments = analyzer.argumentsOf(rt_type);
		for (const ResolvedType &rt_pattern : analyzer.declarations.at(rt_type.declaration).parents) {
			ResolvedType rt_parent = analyzer.substitute(rt_pattern, m_arguments);
			if (rt_parent.declaration) v_pending.push_back(std::move(rt_parent));
		}
	}
	return false;
}

// A symbolic type argument can take on a concrete type when the class is instantiated.
bool possibleView (const ResolvedType &expected, const ResolvedType &view) {
	if (!expected.dependent() && !view.dependent()) return expected == view;
	if (expected.parameter || view.parameter) return true;
	return expected.declaration == view.declaration && expected.name == view.name && expected.width == view.width && expected.suffixes == view.suffixes &&
	       std::ranges::equal(expected.arguments(), view.arguments(), possibleView);
}

// An intersection may reach the same interface through several ancestors or different written bounds.
template <typename Resolve>
void checkBoundInterfaces (const Analyzer &analyzer, const TypeParameterDeclaration &parameter, Resolve resolve, const TokenSpan *use) {
	if (parameter.bounds.size() < 2) return;
	using Applications = std::map<ResolvedType, TokenSpan>;
	std::map<const Symbol *, Applications> m_interfaces;
	for (const TypeParameterBound &tp_bound : parameter.bounds) {
		const TypeReference *tr_bound = std::get_if<TypeReference>(&tp_bound.value);
		if (!tr_bound) continue;
		ResolvedType rt_required = resolve(*tr_bound);
		if (rt_required.nullable()) rt_required = rt_required.element();
		if (!rt_required.declaration || !rt_required.suffixes.empty()) continue;
		hasTypeView(analyzer, rt_required, [&] (const ResolvedType &view) {
			if (!asInterface(view.declaration)) return false;
			Applications &m_prior = m_interfaces[view.declaration];
			if (m_prior.contains(view)) return false;
			for (const Applications::value_type &entry : m_prior) {
				if (possibleView(view, entry.first)) continue;
				std::vector<cde::SourceLabel> v_labels;
				if (use) v_labels.push_back({ *use, "type argument here" });
				v_labels.push_back({ tr_bound->get_span(), "requires " + view.spelling() });
				v_labels.push_back({ entry.second, "requires " + entry.first.spelling() });
				throw cde::ClothDiagnostic(Code::CONFLICTING_INTERFACE_APPLICATION,
				                           "Conflicting interface applications for '" + parameter.name.get_lexeme() + "'", std::move(v_labels));
			}
			m_prior.emplace(view, tr_bound->get_span());
			return false;
		});
	}
}
}

const Symbol &objectRoot () {
	// The implicit base has a stable nominal identity and no source constructor.
	static const TokenSpan        TS_ROOT{ 1, 1, 1, 1 };
	static const Token            T_CLASS{ std::string(to_string(kClass)), TokenType::Keyword, TS_ROOT };
	static const Token            T_NAME{ "Object", TokenType::Identifier, TS_ROOT };
	static const ClassDeclaration CD_ROOT{ T_CLASS, T_NAME, {}, {}, {}, {}, TS_ROOT };
	static const Symbol           SYM_ROOT{ SymbolKind::Type, Visibility::Public, T_NAME, &CD_ROOT, {} };
	return SYM_ROOT;
}

const StructDeclaration *asStruct (const Symbol *symbol) {
	if (!symbol || symbol->kind != SymbolKind::Type) return nullptr;
	return dynamic_cast<const StructDeclaration *>(std::get<const TypeDeclaration *>(symbol->declaration));
}

const EnumDeclaration *enumeration (const ResolvedType &type) {
	return type.declaration && type.suffixes.empty() ?
	               dynamic_cast<const EnumDeclaration *>(std::get<const TypeDeclaration *>(type.declaration->declaration)) :
	               nullptr;
}

const InterfaceDeclaration *asInterface (const Symbol *symbol) {
	if (!symbol || symbol->kind != SymbolKind::Type) {
		return nullptr;
	}
	return dynamic_cast<const InterfaceDeclaration *>(std::get<const TypeDeclaration *>(symbol->declaration));
}

const ClassDeclaration *asClass (const Symbol *symbol) {
	if (!symbol || symbol->kind != SymbolKind::Type) {
		return nullptr;
	}
	return dynamic_cast<const ClassDeclaration *>(std::get<const TypeDeclaration *>(symbol->declaration));
}

const TypeReference &fieldType (const Symbol &symbol) {
	if (const auto field = std::get_if<const StructFieldDeclaration *>(&symbol.declaration)) {
		return (*field)->type;
	}
	if (const auto field = std::get_if<const ClassFieldDeclaration *>(&symbol.declaration)) {
		return (*field)->type;
	}
	if (const auto field = std::get_if<const ClassParameterDeclaration *>(&symbol.declaration)) {
		return (*field)->parameter.type;
	}
	return std::get<const EnumFieldDeclaration *>(symbol.declaration)->type;
}

void Analyzer::applyArguments (ResolvedType &type, ResultTypes arguments) const {
	if (arguments.empty()) {
		type.application.reset();
		return;
	}
	unsigned u_depth = 0;
	for (const ResolvedType &rt_argument : arguments) u_depth = std::max(u_depth, 1 + rt_argument.argumentDepth());
	const bool b_dependent = std::ranges::any_of(arguments, &ResolvedType::dependent);
	type.application       = *s_arguments.emplace(std::make_shared<TypeArguments>(std::move(arguments), u_depth, b_dependent)).first;
}

ResolvedType Analyzer::substitute (ResolvedType type, const Substitutions &arguments) const {
	if (arguments.empty() || !type.dependent()) return type;
	using Replacements = std::map<const TypeArguments *, std::shared_ptr<const TypeArguments>>;
	Replacements                                    m_replaced;
	const std::function<ResolvedType(ResolvedType)> replace = [&] (ResolvedType rt_type) {
		if (const Substitutions::const_iterator it_argument = arguments.find(rt_type.parameter); it_argument != arguments.end()) {
			ResolvedType rt_result = it_argument->second;
			if (rt_type.projectedSuffixes) {
				rt_result = boundShape(rt_result);
				if (rt_result.suffixes.size() < rt_type.projectedSuffixes)
					fail(Code::UNSATISFIED_GENERIC_BOUND, rt_type.parameter->name.get_span(), "Type argument lacks the required payload");
				rt_result.suffixes.resize(rt_result.suffixes.size() - rt_type.projectedSuffixes);
			}
			for (const std::string &s_suffix : rt_type.suffixes) {
				if (s_suffix != to_string(oQuestion) || !rt_result.nullable()) rt_result.suffixes.push_back(s_suffix);
			}
			return rt_result;
		}
		if (rt_type.application && rt_type.application->dependent) {
			const std::pair<Replacements::iterator, bool> it_result = m_replaced.try_emplace(rt_type.application.get());
			if (it_result.second) {
				ResultTypes v_arguments;
				for (const ResolvedType &rt_argument : rt_type.arguments()) v_arguments.push_back(replace(rt_argument));
				applyArguments(rt_type, std::move(v_arguments));
				it_result.first->second = rt_type.application;
			} else {
				rt_type.application = it_result.first->second;
			}
		}
		return rt_type;
	};
	return replace(std::move(type));
}

ResolvedType Analyzer::ownerType (const Symbol &symbol) const {
	ResolvedType rt_type(symbol.name.get_lexeme());
	rt_type.declaration = &symbol;
	if (const TypeParameterList *tp = typeParameters(symbol)) {
		ResultTypes v_arguments;
		for (const TypeParameterDeclaration &param : tp->parameters) {
			ResolvedType rt_arg(param.name.get_lexeme());
			rt_arg.parameter = &param;
			v_arguments.push_back(std::move(rt_arg));
		}
		applyArguments(rt_type, std::move(v_arguments));
	}
	return rt_type;
}

Substitutions Analyzer::argumentsOf (const ResolvedType &type) const {
	Substitutions            m_args;
	const TypeParameterList *tp = type.declaration ? typeParameters(*type.declaration) : nullptr;
	if (tp) {
		for (std::size_t i = 0; i < type.arguments().size(); ++i) {
			m_args.emplace(&tp->parameters.at(i), type.arguments()[i]);
		}
	}
	return m_args;
}

const TypeParameterDeclaration *Analyzer::genericParameter (const Symbol &context, const std::string &name) const {
	for (const Symbol *sym = &context; sym; sym = declarations.at(sym).owner) {
		if (const TypeParameterList *tp = typeParameters(*sym)) {
			for (const TypeParameterDeclaration &param : tp->parameters) {
				if (param.name.get_lexeme() == name) {
					return &param;
				}
			}
		}
		if (sym->kind == SymbolKind::Type) break;
	}
	return nullptr;
}

void Analyzer::checkParameterNames (const Symbol &symbol) const {
	const TypeParameterList *tpl_parameters = typeParameters(symbol);
	if (!tpl_parameters) return;
	const Symbol *sym_owner = declarations.at(&symbol).owner;
	using ParameterNames    = std::map<std::string, const Token *>;
	ParameterNames m_names;
	for (const TypeParameterDeclaration &tp_parameter : tpl_parameters->parameters) {
		const Token &t_name = tp_parameter.name;
		if (t_name.get_lexeme() == symbol.name.get_lexeme())
			fail(Code::INVALID_GENERIC_PARAMETER, t_name.get_span(), "A type parameter cannot have its owner's name", &symbol.name);
		if (sym_owner && symbol.kind != SymbolKind::Type) {
			if (const TypeParameterDeclaration *tp_outer = genericParameter(*sym_owner, t_name.get_lexeme()))
				fail(Code::INVALID_GENERIC_PARAMETER, t_name.get_span(), "Type parameter already declared by the enclosing type",
				     &tp_outer->name);
		}
		const std::pair<ParameterNames::iterator, bool> it_name = m_names.emplace(t_name.get_lexeme(), &t_name);
		if (!it_name.second) fail(Code::INVALID_GENERIC_PARAMETER, t_name.get_span(), "Duplicate type parameter", it_name.first->second);
	}
	// Defaults can refer to enclosing and earlier parameters, including inside a nested type application.
	for (const TypeParameterDeclaration &tp_parameter : tpl_parameters->parameters) {
		if (tp_parameter.defaultType) {
			const std::function<void(const TypeReference &)> check = [&] (const TypeReference &ref) {
				if (m_names.contains(ref.get_name().get_lexeme()))
					fail(Code::INVALID_GENERIC_ARGUMENT, ref.get_span(), "A default can only use earlier type parameters",
					     &tp_parameter.name);
				for (const TypeReference &tr_argument : ref.get_type_arguments()) check(tr_argument);
			};
			check(*tp_parameter.defaultType);
		}
		m_names.erase(tp_parameter.name.get_lexeme());
	}
}

ResolvedType Analyzer::checkedType (const TypeReference &reference, const Symbol &context, const Substitutions &arguments, bool allowOpen,
                                    unsigned operatorWidth) const {
	std::set<const TypeParameterDeclaration *> s_activeDefaults;
	ResolvedType rt_type = substitute(resolveType(reference, context, 0, allowOpen, s_activeDefaults, operatorWidth), arguments);
	if (!allowOpen) checkBounds(rt_type, reference.get_span(), context);
	return rt_type;
}

ResolvedType Analyzer::type (const TypeReference &reference, const Symbol &context, unsigned depth, bool allowOpen) const {
	std::set<const TypeParameterDeclaration *> s_activeDefaults;
	return resolveType(reference, context, depth, allowOpen, s_activeDefaults);
}

ResolvedType Analyzer::resolveType (const TypeReference &reference, const Symbol &context, unsigned depth, bool allowOpen,
                                    std::set<const TypeParameterDeclaration *> &activeDefaults, unsigned operatorWidth) const {
	const Token    &t_name  = reference.get_name();
	const TokenSpan ts_span = reference.get_span();
	if (reference.get_application()) b_hasTypeApplications = true;
	if (depth > MAX_TYPE_ARG_NESTING) {
		fail(Code::GENERIC_SUBSTITUTION_NESTING, ts_span, "Recursive generic type or default argument");
	}
	const std::string &s_name = t_name.get_lexeme();
	ResolvedType       rt_type(s_name);
	if (s_name == to_string(kRange) && !hasTypeName(context, t_name)) {
		if (reference.get_type_arguments().size() != 1) fail(Code::INVALID_RANGE_VALUE, ts_span, "A range requires one numeric endpoint type");
		ResolvedType element = resolveType(reference.get_type_arguments().front(), context, depth + 1, false, activeDefaults, operatorWidth);
		if (!element.parameter && !numeric(element)) fail(Code::INVALID_RANGE_VALUE, ts_span, "Range endpoints must be numeric");
		applyArguments(rt_type, { element });
	} else if (t_name.get_type() == TokenType::Identifier) {
		rt_type.parameter = genericParameter(context, s_name);
		if (!rt_type.parameter) {
			const std::string_view s_separator = to_string(oDot);
			std::size_t            st_end      = s_name.find(s_separator);
			const Symbol          *sym         = lookupTypeRoot(context, s_name.substr(0, st_end), st_end != std::string::npos);
			if (!sym) {
				fail(Code::UNSUPPORTED_TYPE, ts_span, "Unknown type '" + s_name + "'");
			}
			requireAccess(*sym, context, ts_span);
			while (st_end != std::string::npos) {
				const std::size_t st_start = st_end + s_separator.size();
				st_end                     = s_name.find(s_separator, st_start);
				const std::string s_part   = s_name.substr(st_start, st_end - st_start);
				const auto        it       = sym->members.find(s_part);
				const Symbol     *nested   = nullptr;
				if (it != sym->members.end()) {
					for (const Symbol &member : it->second) {
						if (member.kind == SymbolKind::Type) {
							nested = &member;
						}
					}
				}
				if (!nested) {
					fail(Code::UNSUPPORTED_TYPE, ts_span, "Unknown nested type '" + s_name + "'");
				}
				sym = nested;
				requireAccess(*sym, context, ts_span);
			}
			rt_type.name                                 = sym->name.get_lexeme();
			rt_type.declaration                          = sym;
			const TypeParameterList          *tp         = typeParameters(*sym);
			const std::size_t                 st_count   = tp ? tp->parameters.size() : 0;
			const std::vector<TypeReference> &v_supplied = reference.get_type_arguments();
			if (tp && !allowOpen && !reference.get_application())
				fail(Code::RAW_GENERIC_TYPE, ts_span, "Expected type arguments for '" + s_name + "'", &sym->name);
			if (v_supplied.size() > st_count || (!tp && reference.get_application())) {
				fail(Code::GENERIC_ARITY, ts_span, "Unexpected type arguments for '" + s_name + "'");
			}
			Substitutions m_args;
			ResultTypes   v_arguments;
			for (std::size_t i = 0; i < st_count; ++i) {
				const TypeParameterDeclaration &param = tp->parameters[i];
				ResolvedType                    rt_arg;
				if (allowOpen && (i >= v_supplied.size() || v_supplied[i].is_placeholder())) {
					rt_arg.name      = param.name.get_lexeme();
					rt_arg.parameter = &param;
				} else if (i < v_supplied.size()) {
					rt_arg = resolveType(v_supplied[i], context, depth + 1, false, activeDefaults, operatorWidth);
				} else if (param.defaultType) {
					if (!activeDefaults.insert(&param).second)
						fail(Code::INVALID_GENERIC_ARGUMENT, param.defaultType->get_span(), "Cyclic type argument default",
						     &param.name);
					rt_arg = substitute(resolveType(*param.defaultType, *sym, depth + 1, false, activeDefaults), m_args);
					activeDefaults.erase(&param);
				} else {
					fail(Code::GENERIC_ARITY, ts_span, "Missing type argument for '" + param.name.get_lexeme() + "'");
				}
				if (rt_arg.name == to_string(kVoid)) {
					fail(Code::INVALID_GENERIC_ARGUMENT, ts_span, "A type argument must be a value type");
				}
				m_args.emplace(&param, rt_arg);
				v_arguments.emplace_back(std::move(rt_arg));
			}
			applyArguments(rt_type, std::move(v_arguments));
		} else if (reference.get_application()) {
			fail(Code::INVALID_GENERIC_ARGUMENT, ts_span, "A type parameter cannot take type arguments");
		}
	} else if (reference.is_placeholder()) {
		fail(Code::INVALID_GENERIC_ARGUMENT, ts_span, "This type argument needs a concrete type");
	}
	if (reference.isUnsigned()) {
		if (s_name != to_string(kInteger)) {
			fail(Code::INVALID_UNSIGNED, ts_span, std::format("Only {} can be {}", to_string(kInteger), to_string(kUnsigned)));
		}
		rt_type.name = unsignedIntegerSpelling();
	}
	if (reference.get_width()) {
		const Token &t_width = *reference.get_width();
		if (s_name != to_string(kInteger) && s_name != to_string(kFloat)) {
			fail(Code::UNSUPPORTED_WIDTH, ts_span, "Width requires an integer or floating-point type");
		}
		if (t_width.get_lexeme() == to_string(kAuto)) {
			if (depth) fail(Code::UNSUPPORTED_TYPE, ts_span, "A type application needs a concrete numeric width");
			if (operatorWidth && reference.isUnsigned()) fail(Code::UNSUPPORTED_TYPE, ts_span, "A captured width requires a signed numeric type");
			if (operatorWidth && s_name == to_string(kFloat) && operatorWidth != 16 && operatorWidth != 32 && operatorWidth != 64)
				fail(Code::UNSUPPORTED_WIDTH, ts_span, "Unsupported captured floating-point width");
			rt_type.width = operatorWidth;
		} else {
			const IntegerValue v_width = integerLiteralValue(t_width);
			if (v_width.size() != 1 || !v_width.front() || v_width.front() > MAX_INTEGER_BITS ||
			    (s_name == to_string(kFloat) && v_width.front() != 16 && v_width.front() != 32 && v_width.front() != 64)) {
				fail(Code::UNSUPPORTED_WIDTH, ts_span, "Unsupported numeric width");
			}
			rt_type.width = v_width.front();
		}
	}
	if (reference.get_nullable()) {
		rt_type.suffixes.emplace_back(to_string(oQuestion));
	}
	for (std::size_t i = 0; i < reference.get_array_suffixes().size(); ++i) {
		std::string       s_extent;
		const std::size_t index = reference.get_array_suffixes().size() - 1 - i;
		if (index < reference.get_array_sizes().size() && reference.get_array_sizes()[index]) {
			const IntegerValue v_size = integerLiteralValue(*reference.get_array_sizes()[index]);
			if (v_size.size() > program.layout.sizeBits / 32) {
				fail(Code::INVALID_ARRAY, reference.get_array_sizes()[index]->get_span(),
				     std::format("Array extent exceeds {} on a {}-bit target", to_string(kUsize), program.layout.sizeBits));
			}
			std::uint64_t extent = v_size.front();
			if (v_size.size() == 2) {
				extent |= static_cast<std::uint64_t>(v_size[1]) << 32;
			}
			s_extent = std::to_string(extent);
		}
		rt_type.suffixes.push_back(arraySuffix(s_extent));
		if (i < reference.get_array_nullability().size() && reference.get_array_nullability()[i]) {
			rt_type.suffixes.emplace_back(to_string(oQuestion));
		}
	}
	if (rt_type.name == to_string(kVoid) && !rt_type.suffixes.empty()) {
		if (reference.hasNullability())
			fail(Code::INVALID_NULLABLE_TYPE, ts_span, std::format("{} has no value and cannot be nullable", to_string(kVoid)));
		fail(Code::VOID_TYPE, ts_span, std::format("{} cannot be an array element", to_string(kVoid)));
	}
	return rt_type;
}

bool Analyzer::sameSignature (const Candidate &left, const Candidate &right, bool includeResults) const {
	const TypeParameterList *a = typeParameters(*left.symbol), *b = typeParameters(*right.symbol);
	if ((a ? a->parameters.size() : 0) != (b ? b->parameters.size() : 0)) {
		return false;
	}
	Substitutions mapped = right.arguments;
	if (a && b) {
		for (std::size_t i = 0; i < a->parameters.size(); ++i) {
			ResolvedType parameter(a->parameters[i].name.get_lexeme());
			parameter.parameter       = &a->parameters[i];
			mapped[&b->parameters[i]] = parameter;
		}
	}
	const auto sameTypes = [&] (const ResultTypes &first, const ResultTypes &second) {
		if (first.size() != second.size()) {
			return false;
		}
		for (std::size_t i = 0; i < first.size(); ++i) {
			if (substitute(first[i], left.arguments) != substitute(second[i], mapped)) {
				return false;
			}
		}
		return true;
	};
	const DeclarationInfo &first = declarations.at(left.symbol), &second = declarations.at(right.symbol);
	return sameTypes(first.parameters, second.parameters) && (!includeResults || sameTypes(first.returns, second.returns));
}

std::vector<Candidate> Analyzer::members (const ResolvedType &type, std::string_view name, SymbolKind kind, MemberLookup lookup) const {
	std::vector<Candidate> v_members;
	if (!type.suffixes.empty()) {
		return v_members;
	}
	std::set<const Symbol *>                  s_path;
	std::function<void(const ResolvedType &)> visit = [&] (const ResolvedType &receiver) {
		if (receiver.parameter) {
			const ParameterContract pc_contract = parameterContract(*receiver.parameter);
			if (kind == SymbolKind::Constructor) {
				const Guarantees::const_iterator guarantees = m_guarantees.find(pc_contract.parameter);
				if (guarantees != m_guarantees.end()) {
					for (const Symbol *sym : guarantees->second) {
						v_members.push_back({ sym, pc_contract.arguments });
					}
				}
				return;
			}
			const Symbol &owner = *m_genericOwners.at(pc_contract.parameter);
			if (!s_path.insert(&owner).second) {
				return;
			}
			for (const TypeParameterBound &bound : pc_contract.parameter->bounds) {
				if (const TypeReference *ref = std::get_if<TypeReference>(&bound.value)) {
					visit(substitute(this->type(*ref, owner), pc_contract.arguments));
				}
			}
			s_path.erase(&owner);
			return;
		}
		if (!receiver.declaration || !s_path.insert(receiver.declaration).second) {
			return;
		}
		if ((kind == SymbolKind::Method || kind == SymbolKind::StateMethodContract) &&
		    (asClass(receiver.declaration) || asInterface(receiver.declaration))) {
			const TypeDispatch &td_table = dispatchFor(receiver);
			if (kind == SymbolKind::StateMethodContract) {
				for (const StateMethod &sm_method : td_table.stateMethods) {
					if (sm_method.contract.symbol->name.get_lexeme() != name) continue;
					Candidate c_method{ sm_method.contract.symbol, argumentsOf(sm_method.contract.owner) };
					c_method.dispatch = CallDispatch{ DispatchKind::State, receiver, sm_method.contract };
					v_members.push_back(std::move(c_method));
				}
			} else {
				for (const MethodSlot &ms_slot : td_table.methods) {
					if (ms_slot.contract.symbol->name.get_lexeme() != name) continue;
					const MethodRef &mr_static = lookup == MemberLookup::Declaration && ms_slot.implementation ? *ms_slot.implementation :
					                                                                                             ms_slot.contract;
					Candidate        c_method{ mr_static.symbol, argumentsOf(mr_static.owner) };
					if (ms_slot.implementation && ms_slot.implementation->symbol != mr_static.symbol) {
						c_method.overridingMethod    = ms_slot.implementation->symbol;
						c_method.overridingArguments = argumentsOf(ms_slot.implementation->owner);
					}
					if (ms_slot.kind != DispatchKind::Direct) c_method.dispatch = CallDispatch{ ms_slot.kind, receiver, ms_slot.contract };
					v_members.push_back(std::move(c_method));
				}
			}
			s_path.erase(receiver.declaration);
			return;
		}
		const Substitutions m_args = argumentsOf(receiver);
		const auto          it     = receiver.declaration->members.find(name);
		if (it != receiver.declaration->members.end()) {
			for (const Symbol &sym : it->second) {
				if (sym.kind == kind) {
					v_members.push_back({ &sym, m_args });
				}
			}
		}
		if (kind != SymbolKind::Constructor && kind != SymbolKind::Destructor) {
			for (const ResolvedType &parent : declarations.at(receiver.declaration).parents) {
				visit(substitute(parent, m_args));
			}
		}
		s_path.erase(receiver.declaration);
	};
	visit(type);
	if (v_members.size() < 2 || (!type.parameter && (asClass(type.declaration) || asInterface(type.declaration)) &&
	                             (kind == SymbolKind::Method || kind == SymbolKind::StateMethodContract)))
		return v_members;
	if (kind == SymbolKind::Method) {
		std::stable_partition(v_members.begin(), v_members.end(),
		                      [&] (const Candidate &candidate) { return !asInterface(declarations.at(candidate.symbol).owner); });
	}
	// A diamond can reach one declaration twice. A derived override also occupies its parent's callable slot.
	std::vector<Candidate> unique;
	for (const Candidate &candidate : v_members) {
		const DeclarationInfo                 &current  = declarations.at(candidate.symbol);
		const std::vector<Candidate>::iterator it_prior = std::ranges::find_if(unique, [&] (const Candidate &prior) {
			if (prior.symbol == candidate.symbol) {
				return prior.arguments == candidate.arguments;
			}
			const DeclarationInfo &previous = declarations.at(prior.symbol);
			if (kind != SymbolKind::Method || previous.owner == current.owner || !previous.owner || !current.owner ||
			    !sameSignature(prior, candidate)) {
				return false;
			}
			if (asClass(previous.owner) && asInterface(current.owner)) {
				return true;
			}
			return accepts(substitute(ownerType(*current.owner), candidate.arguments), substitute(ownerType(*previous.owner), prior.arguments));
		});
		if (it_prior == unique.end()) {
			unique.push_back(candidate);
		}
	}
	return unique;
}

Candidate Analyzer::boundMethod (const ExpressionInfo &call, const ResolvedType &receiver, const std::vector<Candidate> &candidates,
                                 const Substitutions &arguments) const {
	// The template selects the overload. A concrete receiver can supply an implementation of the same method.
	Candidate c_contract{ call.argumentContract };
	for (const Substitutions::value_type &entry : call.typeArguments) {
		c_contract.arguments.emplace(entry.first, substitute(entry.second, arguments));
	}
	if (call.dispatch) {
		c_contract.dispatch                 = *call.dispatch;
		c_contract.dispatch->receiver       = substitute(call.dispatch->receiver, arguments);
		c_contract.dispatch->contract.owner = substitute(call.dispatch->contract.owner, arguments);
		if (call.dispatch->kind == DispatchKind::Interface && asClass(receiver.declaration)) {
			for (const InterfaceTarget &it_target : dispatchFor(receiver).interfaceTargets) {
				if (it_target.view == c_contract.dispatch->receiver && it_target.contract == c_contract.dispatch->contract) {
					if (it_target.implementation.symbol != c_contract.symbol) {
						c_contract.overridingMethod    = it_target.implementation.symbol;
						c_contract.overridingArguments = argumentsOf(it_target.implementation.owner);
					}
					return c_contract;
				}
			}
		}
	}
	for (const Candidate &c_target : candidates) {
		if (c_target.symbol == call.argumentContract) {
			c_contract.arguments.insert(c_target.arguments.begin(), c_target.arguments.end());
			c_contract.overridingMethod    = c_target.overridingMethod;
			c_contract.overridingArguments = c_target.overridingArguments;
			break;
		}
	}
	return c_contract;
}

ResultTypes Analyzer::typeBounds (const ResolvedType &parameter) const {
	const ParameterContract pc_contract = parameterContract(*parameter.parameter);
	const Symbol           &sym_owner   = *m_genericOwners.at(pc_contract.parameter);
	ResultTypes             v_bounds;
	bool                    b_constructor = false;
	for (const TypeParameterBound &tp_bound : pc_contract.parameter->bounds) {
		if (const TypeReference *tr_bound = std::get_if<TypeReference>(&tp_bound.value)) {
			ResolvedType rt_required = substitute(type(*tr_bound, sym_owner), pc_contract.arguments);
			if (parameter.projectedSuffixes > rt_required.suffixes.size()) continue;
			rt_required.suffixes.resize(rt_required.suffixes.size() - parameter.projectedSuffixes);
			v_bounds.push_back(std::move(rt_required));
		} else
			b_constructor = true;
	}
	// A constructor guarantee supplies the class root when the parameter has no named bound.
	if (b_constructor && v_bounds.empty() && !parameter.projectedSuffixes) v_bounds.push_back(ownerType(objectRoot()));
	return v_bounds;
}

ResolvedType Analyzer::boundShape (const ResolvedType &value) const {
	ResolvedType                               rt_result = value;
	std::set<const TypeParameterDeclaration *> s_active;
	while (rt_result.parameter && !rt_result.projectedSuffixes) {
		const ParameterContract pc_contract = parameterContract(*rt_result.parameter);
		const TypeReference    *tr_shape    = nullptr;
		for (const TypeParameterBound &tp_bound : pc_contract.parameter->bounds) {
			const TypeReference *tr_bound = std::get_if<TypeReference>(&tp_bound.value);
			if (tr_bound && (!tr_bound->get_array_suffixes().empty() || tr_bound->get_nullable())) {
				tr_shape = tr_bound;
				break;
			}
		}
		if (!tr_shape) break;
		if (!s_active.insert(rt_result.parameter).second)
			fail(Code::INVALID_GENERIC_BOUND, tr_shape->get_span(), "Cyclic array bound", &rt_result.parameter->name);
		ResolvedType rt_required = substitute(type(*tr_shape, *m_genericOwners.at(pc_contract.parameter)), pc_contract.arguments);
		if (tr_shape->get_array_suffixes().empty() || (sym_number && rt_required.declaration == sym_number)) {
			rt_required.name        = rt_result.name;
			rt_required.parameter   = rt_result.parameter;
			rt_required.declaration = nullptr;
			rt_required.application.reset();
			rt_required.projectedSuffixes = rt_required.suffixes.size();
		}
		for (const std::string &s_suffix : rt_result.suffixes)
			if (s_suffix != to_string(oQuestion) || !rt_required.nullable()) rt_required.suffixes.push_back(s_suffix);
		rt_result = std::move(rt_required);
	}
	return rt_result;
}

bool Analyzer::numeric (const ResolvedType &value) const {
	if (value.integer() || value.floating()) return true;
	if (!sym_number || !value.parameter || !value.suffixes.empty()) return false;
	return std::ranges::any_of(typeBounds(value), [&] (const ResolvedType &bound) { return bound.declaration == sym_number && bound.suffixes.empty(); });
}

ResultTypes Analyzer::applicationsOf (const ResolvedType &actual, const Symbol &definition) const {
	if (!actual.suffixes.empty()) return {};
	if (actual.declaration == &definition) return { actual };
	ResultTypes       v_views;
	const ResultTypes v_bounds = actual.parameter ? typeBounds(actual) : ResultTypes{ actual };
	for (const ResolvedType &rt_bound : v_bounds) {
		if (!rt_bound.declaration || !rt_bound.suffixes.empty()) continue;
		hasTypeView(*this, rt_bound, [&] (const ResolvedType &view) {
			if (view.declaration == &definition) v_views.push_back(view);
			return false;
		});
	}
	return v_views;
}

bool Analyzer::referenceType (const ResolvedType &type) const {
	if (!type.suffixes.empty()) return false;
	if (sym_number && type.declaration == sym_number) return false;
	if (!type.parameter) return asClass(type.declaration) || asInterface(type.declaration);
	const ParameterContract pc_contract = parameterContract(*type.parameter);
	for (const TypeParameterBound &tp_bound : pc_contract.parameter->bounds) {
		const TypeReference *tr_bound = std::get_if<TypeReference>(&tp_bound.value);
		if (!tr_bound) return true;
		const ResolvedType rt_bound   = substitute(this->type(*tr_bound, *m_genericOwners.at(pc_contract.parameter)), pc_contract.arguments);
		const ResolvedType rt_payload = rt_bound.nullable() ? rt_bound.element() : rt_bound;
		if (rt_payload.declaration != sym_number && rt_payload.suffixes.empty() &&
		    (asClass(rt_payload.declaration) || asInterface(rt_payload.declaration)))
			return true;
	}
	return false;
}

bool Analyzer::mayBeAbsent (const ResolvedType &type) const {
	return type.nullable() || (type.suffixes.empty() && (type.name == to_string(kNull) ||
	                                                     (type.parameter && !type.width && parameterContract(*type.parameter).parameter->bounds.empty())));
}

bool Analyzer::hasMultipleTypeBounds (const ResolvedType &type) const {
	if (!type.parameter) return false;
	const ParameterContract pc_contract = parameterContract(*type.parameter);
	return std::ranges::count_if(pc_contract.parameter->bounds,
	                             [] (const TypeParameterBound &bound) { return std::holds_alternative<TypeReference>(bound.value); }) > 1;
}

bool Analyzer::supportsEquality (const ResolvedType &type) const {
	if (!type.parameter) return true;
	ResolvedType rt_element = type;
	rt_element.suffixes.clear();
	return referenceType(rt_element) || numeric(rt_element);
}

bool Analyzer::classEqualityMayCall (const ResolvedType &type, Operator operation) const {
	const std::function hasEquality = [&] (const ResolvedType &owner) {
		for (const Candidate &c_op : members(owner, to_string(operation), SymbolKind::Operator)) {
			const MethodDeclaration &md_equal = dynamic_cast<const MethodDeclaration &>(*callable(*c_op.symbol));
			const OperatorMember    &om_form  = *md_equal.operatorMember;
			if (!om_form.provider && !om_form.selector && !om_form.rightReceiver && md_equal.parameters.size() == 1 &&
			    c_op.symbol->visibility == Visibility::Public &&
			    accepts(substitute(declarations.at(c_op.symbol).parameters.front(), c_op.arguments), owner))
				return true;
		}
		return false;
	};
	if (operation == oNotEqual && classEqualityMayCall(type, oEqual)) return true;
	return hasEquality(type) || anyReceiverClass(type, hasEquality);
}

bool Analyzer::anyReceiverClass (const ResolvedType &type, const std::function<bool(const ResolvedType &)> &matches) const {
	// Constrain dynamic dispatch by every nominal bound on the receiver.
	ResultTypes                                v_views, v_bounds{ type };
	std::set<const TypeParameterDeclaration *> s_parameters;
	while (!v_bounds.empty()) {
		const ResolvedType rt_bound = std::move(v_bounds.back());
		v_bounds.pop_back();
		if (!rt_bound.parameter) {
			v_views.push_back(rt_bound);
			continue;
		}
		if (!s_parameters.insert(rt_bound.parameter).second) continue;
		const ParameterContract pc_contract = parameterContract(*rt_bound.parameter);
		for (const TypeParameterBound &tp_bound : pc_contract.parameter->bounds)
			if (const TypeReference *tr_bound = std::get_if<TypeReference>(&tp_bound.value)) {
				ResolvedType rt_view = substitute(this->type(*tr_bound, *m_genericOwners.at(pc_contract.parameter)), pc_contract.arguments);
				v_bounds.push_back(rt_view.nullable() ? rt_view.element() : std::move(rt_view));
			}
	}
	for (const Symbol *sym_class : v_order) {
		if (!asClass(sym_class)) continue;
		const ResolvedType rt_class = ownerType(*sym_class);
		if (matches(rt_class) && std::ranges::all_of(v_views, [&] (const ResolvedType &view) {
			    return hasTypeView(*this, rt_class, [&] (const ResolvedType &parent) { return possibleView(view, parent); });
		    }))
			return true;
	}
	return false;
}

bool Analyzer::accepts (const ResolvedType &expected, const ResolvedType &actual) const {
	if (expected == actual) {
		return true;
	}
	if (expected.parameter || actual.parameter) {
		const ResolvedType rt_target = boundShape(expected), rt_source = boundShape(actual);
		if (rt_target != expected || rt_source != actual) return accepts(rt_target, rt_source);
	}
	if (expected.nullable()) {
		return actual.name == to_string(kNull) || accepts(expected.element(), actual.nullable() ? actual.element() : actual);
	}
	if (actual.nullable()) {
		return false;
	}
	if (sym_number && expected.declaration == sym_number && expected.suffixes.empty() && numeric(actual)) return true;
	if (expected.array() && actual.array()) {
		if (expected.suffixes.back() != arraySuffix() && actual.suffixes.back() != arraySuffix() &&
		    expected.suffixes.back() != actual.suffixes.back()) {
			return false;
		}
		const ResolvedType left = expected.element(), right = actual.element();
		if (left.nullable() != right.nullable()) {
			return false;
		}
		const ResolvedType a = left.nullable() ? left.element() : left, b = right.nullable() ? right.element() : right;
		return a.array() && b.array() ? accepts(a, b) : a == b;
	}
	if (actual.parameter && actual.suffixes.empty()) {
		return std::ranges::any_of(typeBounds(actual),
		                           [&] (const ResolvedType &constraint) { return !constraint.parameter && accepts(expected, constraint); });
	}
	if (!expected.declaration || !actual.declaration || !actual.suffixes.empty() || !expected.suffixes.empty()) {
		return false;
	}
	if (expected.declaration == &objectRoot() && asInterface(actual.declaration)) return referenceType(actual);
	return hasTypeView(*this, actual, [&] (const ResolvedType &view) { return view == expected; });
}

void Analyzer::resolveDeclaration (const Symbol &symbol) {
	DeclarationInfo &info      = declarations.at(&symbol);
	const auto       valueType = [&] (const TypeReference &reference) {
                ResolvedType rt = type(reference, symbol);
                if (rt.name == to_string(kVoid)) {
                        fail(Code::VOID_TYPE, reference.get_span(), std::format("A value needs a non-{} type", to_string(kVoid)));
                }
                return rt;
	};
	if (const CallableSignature *cs = callable(symbol)) {
		std::set<std::string> s_names;
		for (const ParameterDeclaration &param : cs->parameters) {
			if (!s_names.insert(param.name.get_lexeme()).second) {
				fail(Code::DUPLICATE_BINDING, param.name.get_span(), "Duplicate parameter '" + param.name.get_lexeme() + "'");
			}
			info.parameters.push_back(valueType(param.type));
		}
		for (const TypeReference &ref : cs->returnTypes) {
			ResolvedType rt = type(ref, symbol);
			if (rt.name != to_string(kVoid)) {
				info.returns.push_back(std::move(rt));
			} else if (cs->returnTypes.size() != 1) {
				fail(Code::VOID_TYPE, ref.get_span(), std::format("{} must be the only result", to_string(kVoid)));
			}
		}
		if (symbol.kind == SymbolKind::Operator) {
			resolveOperator(symbol, dynamic_cast<const MethodDeclaration &>(*cs));
		}
	} else if (symbol.kind == SymbolKind::Field) {
		info.value = valueType(fieldType(symbol));
	} else if (symbol.kind == SymbolKind::EnumCase) {
		info.value = ownerType(*info.owner);
	} else if (symbol.kind == SymbolKind::State) {
		const auto state = std::get<const StateBodyDeclaration *>(symbol.declaration);
		info.value       = type(state->payloadType, symbol);
		if (const auto *top = dynamic_cast<const TopLevelStateDeclaration *>(state)) {
			info.parents.push_back(valueType(top->host));
		}
	} else if (symbol.kind == SymbolKind::Type) {
		const auto td = std::get<const TypeDeclaration *>(symbol.declaration);
		if (const auto *cd = dynamic_cast<const ClassDeclaration *>(td)) {
			for (const ClassParentClause &parent : cd->parents) {
				const ResolvedType &rt_parent = info.parents.emplace_back(valueType(parent.type));
				if (asInterface(rt_parent.declaration) && parent.constructorArguments)
					fail(Code::INVALID_INTERFACE_HEADER, parent.span, "An interface clause takes no constructor arguments",
					     &rt_parent.declaration->name);
				if (rt_parent.declaration == &objectRoot() && parent.constructorArguments && !parent.constructorArguments->empty())
					fail(Code::INVALID_CLASS_MEMBER, parent.span, std::format("{} takes no base arguments", rt_parent.name));
			}
			if (std::ranges::none_of(info.parents, [] (const ResolvedType &parent) { return asClass(parent.declaration); }))
				info.parents.push_back(ownerType(objectRoot()));
		} else if (const auto *id = dynamic_cast<const InterfaceDeclaration *>(td)) {
			for (const TypeReference &parent : id->parents) {
				info.parents.push_back(valueType(parent));
			}
		}
	}
}

void Analyzer::checkBounds (const ResolvedType &type, const TokenSpan &where, const Symbol &use) const {
	type.visitApplications([&] (const ResolvedType &rt_type) {
		if (!rt_type.declaration && rt_type.name == to_string(kRange) && !numeric(rt_type.arguments().front()))
			fail(Code::INVALID_RANGE_VALUE, where, "Range endpoints must be numeric");
		const Substitutions m_arguments = argumentsOf(rt_type);
		for (const Substitutions::value_type &entry : m_arguments) {
			checkBounds(*entry.first, m_arguments, use, where);
		}
	});
}

bool Analyzer::acceptsBound (const ResolvedType &required, const ResolvedType &actual) const {
	if (required.nullable() != actual.nullable()) return false;
	if (!(required.nullable() ? required.element() : required).array()) return accepts(required, actual);
	if (!sym_number || required.declaration != sym_number) return required == actual;
	if (required.suffixes != actual.suffixes) return false;
	ResolvedType rt_leaf = actual;
	rt_leaf.suffixes.clear();
	return accepts(ownerType(*sym_number), rt_leaf);
}

void Analyzer::checkBounds (const TypeParameterDeclaration &parameter, const Substitutions &arguments, const Symbol &use, const TokenSpan &where) const {
	const ParameterContract pc_contract = parameterContract(parameter);
	if (pc_contract.parameter->bounds.empty()) return;
	const ResolvedType                                       rt_actual  = boundShape(arguments.at(&parameter));
	const Symbol                                            &sym_origin = *m_genericOwners.at(pc_contract.parameter);
	const std::function<ResolvedType(const TypeReference &)> resolve    = [&] (const TypeReference &reference) {
                return substitute(substitute(type(reference, sym_origin), pc_contract.arguments), arguments);
	};
	checkBoundInterfaces(*this, *pc_contract.parameter, resolve, &where);
	for (const TypeParameterBound &tp_bound : pc_contract.parameter->bounds) {
		if (const TypeReference *tr_bound = std::get_if<TypeReference>(&tp_bound.value)) {
			if (acceptsBound(boundShape(resolve(*tr_bound)), rt_actual)) continue;
		} else {
			ResultTypes v_required;
			for (const ParameterDeclaration &pd_param : std::get<ConstructorBound>(tp_bound.value).declaration.parameters)
				v_required.push_back(resolve(pd_param.type));
			if (constructorFor(rt_actual, v_required, rt_actual.parameter ? nullptr : &use) || hasConstructorProof(rt_actual, v_required)) continue;
		}
		throw cde::ClothDiagnostic(Code::UNSATISFIED_GENERIC_BOUND,
		                           rt_actual.spelling() + " does not satisfy the bounds of '" + parameter.name.get_lexeme() + "'",
		                           { { where, "type argument here" }, { tp_bound.get_span(), "unsatisfied bound" } });
	}
}

void Analyzer::checkOverload (const Symbol &symbol, const Substitutions &arguments, const TokenSpan *application) const {
	if (!callable(symbol) || dynamic_cast<const ConstructorGuaranteeDeclaration *>(callable(symbol))) return;
	const DeclarationInfo &di_info  = declarations.at(&symbol);
	const SymbolScope     &ss_scope = di_info.owner ? di_info.owner->members : program.units[di_info.file]->get_globals();
	for (const Symbol &sym_prior : ss_scope.at(symbol.name.get_lexeme())) {
		if (&sym_prior == &symbol) break;
		if (sym_prior.kind != symbol.kind || !sameSignature({ &sym_prior, arguments }, { &symbol, arguments })) continue;
		if (symbol.kind == SymbolKind::Operator) {
			const OperatorMember &om_prior   = *dynamic_cast<const MethodDeclaration &>(*callable(sym_prior)).operatorMember;
			const OperatorMember &om_current = *dynamic_cast<const MethodDeclaration &>(*callable(symbol)).operatorMember;
			if (om_prior.rightReceiver != om_current.rightReceiver || om_prior.provider != om_current.provider ||
			    specialize(declarations.at(&sym_prior), arguments).value != specialize(di_info, arguments).value)
				continue;
		}
		const std::string s_message = "Duplicate signature for '" + symbol.name.get_lexeme() + "'";
		const Code        dc_code   = di_info.owner ? Code::DUPLICATE_METHOD : Code::DUPLICATE_FUNCTION;
		if (application) {
			throw cde::ClothDiagnostic(symbol.kind == SymbolKind::Constructor ? Code::INVALID_CLASS_MEMBER : dc_code, s_message,
			                           { { *application, "instantiated here" },
			                             { symbol.name.get_span(), s_message },
			                             { sym_prior.name.get_span(), "previous declaration" } });
		}
		fail(dc_code, symbol.name.get_span(), s_message, &sym_prior.name);
	}
}

void Analyzer::checkPublicSurface (const Symbol &symbol, const Symbol *exportingHost, const DeclarationInfo *specialization) const {
	if (symbol.visibility != Visibility::Public) return;
	for (const Symbol *sym_scope = exportingHost ? exportingHost : declarations.at(&symbol).owner; sym_scope;
	     sym_scope               = declarations.at(sym_scope).owner) {
		if (sym_scope->visibility != Visibility::Public) return;
	}
	const DeclarationInfo &di_info = specialization ? *specialization : declarations.at(&symbol);
	std::set<ResolvedType>                                                                    s_exposed;
	const std::function<void(const ResolvedType &, const TypeReference *, const TokenSpan &)> expose =
	        [&] (const ResolvedType &type, const TypeReference *reference, const TokenSpan &where) {
		        if (!s_exposed.insert(type).second) return;
		        for (const Symbol *sym_type = type.declaration; sym_type; sym_type = declarations.at(sym_type).owner) {
			        if (sym_type->visibility != Visibility::Public)
				        fail(Code::PRIVATE_GENERIC_SIGNATURE, where,
				             "Public signature exposes private type '" + sym_type->name.get_lexeme() + "'", &sym_type->name);
		        }
		        for (std::size_t st_index = 0; st_index < type.arguments().size(); ++st_index) {
			        const TypeReference *tr_arg =
			                reference && st_index < reference->get_type_arguments().size() ? &reference->get_type_arguments()[st_index] : nullptr;
			        expose(type.arguments()[st_index], tr_arg, tr_arg ? tr_arg->get_span() : where);
		        }
	        };
	const std::function<void(const TypeReference &)> written = [&] (const TypeReference &reference) {
		expose(type(reference, symbol), &reference, reference.get_span());
	};
	if (const CallableSignature *cs_method = callable(symbol)) {
		for (const ParameterDeclaration &pd_param : cs_method->parameters) written(pd_param.type);
		for (const TypeReference &tr_result : cs_method->returnTypes) written(tr_result);
		if (cs_method->returnTypes.empty()) {
			for (const ResolvedType &rt_result : di_info.returns) expose(rt_result, nullptr, symbol.name.get_span());
		}
		if (const MethodDeclaration *md_method = dynamic_cast<const MethodDeclaration *>(cs_method);
		    md_method && md_method->operatorMember && md_method->operatorMember->selector)
			written(*md_method->operatorMember->selector);
	} else if (symbol.kind == SymbolKind::Field) {
		written(fieldType(symbol));
	} else if (const ClassDeclaration *cd_class = asClass(&symbol)) {
		for (const ClassParentClause &cp_parent : cd_class->parents) written(cp_parent.type);
	} else if (const InterfaceDeclaration *id_interface = asInterface(&symbol)) {
		for (const TypeReference &tr_parent : id_interface->parents) written(tr_parent);
	} else if (symbol.kind == SymbolKind::State) {
		const StateBodyDeclaration &sd_state = *std::get<const StateBodyDeclaration *>(symbol.declaration);
		written(sd_state.payloadType);
		if (const TopLevelStateDeclaration *ts_state = dynamic_cast<const TopLevelStateDeclaration *>(&sd_state)) written(ts_state->host);
	} else if (symbol.kind == SymbolKind::StateAttachment) {
		expose(*di_info.value, nullptr, symbol.name.get_span());
	}
	if (const TypeParameterList *tpl_parameters = typeParameters(symbol)) {
		for (const TypeParameterDeclaration &tp_param : tpl_parameters->parameters) {
			if (tp_param.defaultType) written(*tp_param.defaultType);
			for (const TypeParameterBound &tp_bound : tp_param.bounds) {
				if (const TypeReference *tr_bound = std::get_if<TypeReference>(&tp_bound.value))
					written(*tr_bound);
				else
					for (const ParameterDeclaration &pd_param : std::get<ConstructorBound>(tp_bound.value).declaration.parameters)
						written(pd_param.type);
			}
		}
	}
}

void Analyzer::checkArrayBound (const TypeParameterDeclaration &parameter, const ResolvedType &shape, const TokenSpan &where) const {
	std::set<const TypeParameterDeclaration *>      s_active{ &parameter };
	std::set<ResolvedType>                          s_complete;
	const std::function<void(const ResolvedType &)> visit = [&] (const ResolvedType &part) {
		if (s_complete.contains(part)) return;
		if (part.parameter) {
			if (!s_active.insert(part.parameter).second) fail(Code::INVALID_GENERIC_BOUND, where, "Cyclic array bound", &part.parameter->name);
			if (s_active.size() > MAX_TYPE_ARG_NESTING)
				fail(Code::GENERIC_SUBSTITUTION_NESTING, where, "Array bound nesting limit exceeded", &parameter.name);
			for (const ResolvedType &rt_next : typeBounds(part))
				if ((rt_next.nullable() ? rt_next.element() : rt_next).array()) visit(rt_next);
			s_active.erase(part.parameter);
		}
		for (const ResolvedType &rt_argument : part.arguments()) visit(rt_argument);
		s_complete.insert(part);
	};
	visit(shape);
}

void Analyzer::checkDeclaredBounds (const TypeParameterDeclaration &parameter, const Symbol &symbol) const {
	using NominalBounds     = std::map<ResolvedType, const Token *>;
	using ConstructorBounds = std::map<ResultTypes, const Token *>;
	NominalBounds     m_nominal;
	ConstructorBounds m_constructors;
	const Token      *t_classBound = nullptr, *t_nullable = nullptr, *t_present = nullptr;
	for (const TypeParameterBound &tp_bound : parameter.bounds) {
		if (const ConstructorBound *cb_bound = std::get_if<ConstructorBound>(&tp_bound.value)) {
			ResultTypes v_signature;
			for (const ParameterDeclaration &pd_argument : cb_bound->declaration.parameters) v_signature.push_back(type(pd_argument.type, symbol));
			const Token                                       &t_keyword   = cb_bound->declaration.keyword;
			const std::pair<ConstructorBounds::iterator, bool> it_previous = m_constructors.emplace(std::move(v_signature), &t_keyword);
			if (!it_previous.second)
				fail(Code::INVALID_GENERIC_BOUND, tp_bound.get_span(), "Duplicate constructor bound", it_previous.first->second);
			t_present = &t_keyword;
		} else {
			const TypeReference &tr_bound    = std::get<TypeReference>(tp_bound.value);
			const ResolvedType   rt_required = type(tr_bound, symbol);
			const ResolvedType   rt_payload  = rt_required.nullable() ? rt_required.element() : rt_required;
			if (rt_payload.array() || (sym_number && rt_payload.declaration == sym_number)) {
				if (parameter.bounds.size() > 1)
					fail(Code::INVALID_GENERIC_BOUND, tr_bound.get_span(), rt_required.spelling() + " must be the only bound",
					     &parameter.name);
				if (rt_payload.array()) checkArrayBound(parameter, rt_required, tr_bound.get_span());
			} else if (!asClass(rt_payload.declaration) && !asInterface(rt_payload.declaration)) {
				fail(Code::INVALID_GENERIC_BOUND, tr_bound.get_span(), "Expected a class, interface, or array bound", &parameter.name);
			}
			const std::pair<NominalBounds::iterator, bool> it_previous = m_nominal.emplace(rt_payload, &tr_bound.get_name());
			if (!it_previous.second)
				fail(Code::INVALID_GENERIC_BOUND, tr_bound.get_span(), "Duplicate bound '" + rt_required.spelling() + "'",
				     it_previous.first->second);
			if (!rt_payload.array() && asClass(rt_payload.declaration)) {
				if (t_classBound)
					fail(Code::INVALID_GENERIC_BOUND, tr_bound.get_span(), "A parameter can have only one class bound", t_classBound);
				t_classBound = &tr_bound.get_name();
			}
			(rt_required.nullable() ? t_nullable : t_present) = &tr_bound.get_name();
		}
		if (t_nullable && t_present)
			fail(Code::INVALID_GENERIC_BOUND, t_nullable->get_span(), "Nullable and non-nullable bounds cannot be combined", t_present);
	}
	checkBoundInterfaces(*this, parameter, [&] (const TypeReference &ref) { return type(ref, symbol); }, nullptr);
}

void Analyzer::checkDeclarations () {
	checkInheritance();
	for (const Symbol *sym : v_order) {
		DeclarationInfo &info = declarations.at(sym);
		if (sym->kind == SymbolKind::StateAttachment) {
			info.attachedState = lookupFileState(*sym, sym->name.get_lexeme());
			if (!info.attachedState) {
				fail(Code::INVALID_STATE_ATTACHMENT, sym->name.get_span(), "Unknown attached state '" + sym->name.get_lexeme() + "'");
			}
			const DeclarationInfo &state = declarations.at(info.attachedState);
			if (!accepts(state.parents.front(), ownerType(*info.owner))) {
				fail(Code::INVALID_STATE_ATTACHMENT, sym->name.get_span(), "State is declared for a different host", &info.attachedState->name);
			}
			info.value = state.value;
			m_stateHosts[info.attachedState].push_back(ownerType(*info.owner));
		}
	}
	for (const Symbol *sym_type : v_order) {
		if (asClass(sym_type) || asInterface(sym_type)) dispatchFor(ownerType(*sym_type));
	}
	for (const Symbol *sym : v_order) {
		const DeclarationInfo &info = declarations.at(sym);
		if (const TypeParameterList *tp = typeParameters(*sym)) {
			Substitutions m_defaults = argumentsOf(ownerType(*sym));
			for (const TypeParameterDeclaration &param : tp->parameters) {
				checkDeclaredBounds(param, *sym);
				if (param.defaultType) {
					ResolvedType &rt_default = m_defaults.at(&param);
					rt_default               = substitute(type(*param.defaultType, *sym), m_defaults);
					if (rt_default.name == to_string(kVoid))
						fail(Code::INVALID_GENERIC_ARGUMENT, param.defaultType->get_span(), "A default must be a value type",
						     &param.name);
					checkBounds(rt_default, param.defaultType->get_span(), *sym);
					checkBounds(param, m_defaults, *sym, param.defaultType->get_span());
				}
			}
		}
		for (const ResolvedType &rt : info.parameters) {
			checkBounds(rt, sym->name.get_span(), *sym);
		}
		for (const ResolvedType &rt : info.returns) {
			checkBounds(rt, sym->name.get_span(), *sym);
		}
		for (const ResolvedType &rt : info.parents) {
			checkBounds(rt, sym->name.get_span(), *sym);
		}
		if (info.value) {
			checkBounds(*info.value, sym->name.get_span(), *sym);
		}
		checkOverload(*sym, {}, nullptr);
		if (sym->kind == SymbolKind::Operator) checkCollectionOperator(*sym);
	}
	for (const Symbol *symbol : v_order) {
		checkPublicSurface(*symbol);
		if (asClass(symbol) && symbol->visibility == Visibility::Public) {
			for (const StateMethod &sm_method : dispatchFor(ownerType(*symbol)).stateMethods) {
				if (sm_method.contract.symbol->kind == SymbolKind::Method) checkPublicSurface(*sm_method.contract.symbol, symbol);
			}
		}
	}
}
}
