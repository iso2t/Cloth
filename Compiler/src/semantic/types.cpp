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

const Symbol *namedType (const Candidates &candidates) {
	for (const Symbol *sym : candidates) {
		if (sym->kind == SymbolKind::Type) {
			return sym;
		}
	}
	return nullptr;
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

}

ResolvedType Analyzer::ownerType (const Symbol &symbol) const {
	ResolvedType rt_type(symbol.name.get_lexeme());
	rt_type.declaration = &symbol;
	if (const TypeParameterList *tp = typeParameters(symbol)) {
		for (const TypeParameterDeclaration &param : tp->parameters) {
			ResolvedType rt_arg(param.name.get_lexeme());
			rt_arg.parameter = &param;
			rt_type.arguments.push_back(std::move(rt_arg));
		}
	}
	return rt_type;
}

Substitutions Analyzer::argumentsOf (const ResolvedType &type) const {
	Substitutions            m_args;
	const TypeParameterList *tp = type.declaration ? typeParameters(*type.declaration) : nullptr;
	if (tp) {
		for (std::size_t i = 0; i < type.arguments.size(); ++i) {
			m_args.emplace(&tp->parameters.at(i), type.arguments[i]);
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
	}
	return nullptr;
}

ResolvedType Analyzer::checkedType (const TypeReference &reference, const Symbol &context) const {
	ResolvedType resolved = type(reference, context);
	checkBounds(resolved, reference.get_span(), context);
	return resolved;
}

ResolvedType Analyzer::type (const TypeReference &reference, const Symbol &context, unsigned depth, bool allowOpen) const {
	const Token    &t_name  = reference.get_name();
	const TokenSpan ts_span = reference.get_span();
	if (depth > MAX_TYPE_ARG_NESTING) {
		fail(Code::GENERIC_SUBSTITUTION_NESTING, ts_span, "Recursive generic type or default argument");
	}
	const std::string &s_name = t_name.get_lexeme();
	ResolvedType       rt_type(s_name);
	if (t_name.get_type() == TokenType::Identifier) {
		rt_type.parameter = genericParameter(context, s_name);
		if (!rt_type.parameter) {
			const std::string_view s_separator = to_string(oDot);
			std::size_t            st_end      = s_name.find(s_separator);
			const Symbol *sym    = namedType(lookup(context, s_name.substr(0, st_end)));
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
			if (v_supplied.size() > st_count || (!tp && reference.get_application())) {
				fail(Code::GENERIC_ARITY, ts_span, "Unexpected type arguments for '" + s_name + "'");
			}
			Substitutions m_args;
			for (std::size_t i = 0; i < st_count; ++i) {
				const TypeParameterDeclaration &param = tp->parameters[i];
				ResolvedType                    rt_arg;
				if (allowOpen && (i >= v_supplied.size() || v_supplied[i].is_placeholder())) {
					rt_arg.name      = param.name.get_lexeme();
					rt_arg.parameter = &param;
				} else if (i < v_supplied.size()) {
					rt_arg = type(v_supplied[i], context, depth + 1);
				} else if (param.defaultType) {
					rt_arg = substitute(type(*param.defaultType, *sym, depth + 1), m_args);
				} else {
					fail(Code::GENERIC_ARITY, ts_span, "Missing type argument for '" + param.name.get_lexeme() + "'");
				}
				if (rt_arg.name == to_string(kVoid)) {
					fail(Code::INVALID_GENERIC_ARGUMENT, ts_span, "A type argument must be a value type");
				}
				m_args.emplace(&param, rt_arg);
				rt_type.arguments.emplace_back(std::move(rt_arg));
			}
		} else if (reference.get_application()) {
			fail(Code::GENERIC_ARITY, ts_span, "A type parameter cannot take type arguments");
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
			rt_type.width = 0;
		} else {
			const IntegerValue v_width = integerLiteralValue(t_width);
			if (v_width.size() != 1 || !v_width.front() || v_width.front() > 65536 ||
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
		std::string s_extent;
		if (i < reference.get_array_sizes().size() && reference.get_array_sizes()[i]) {
			const IntegerValue v_size = integerLiteralValue(*reference.get_array_sizes()[i]);
			if (v_size.size() > 2) {
				fail(Code::INVALID_ARRAY, ts_span, "Array extent is too large");
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
		fail(Code::VOID_TYPE, ts_span, std::format("{} cannot be nullable or an array element", to_string(kVoid)));
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
			if (kind == SymbolKind::Constructor) {
				const auto guarantees = m_guarantees.find(receiver.parameter);
				if (guarantees != m_guarantees.end()) {
					for (const Symbol *sym : guarantees->second) {
						v_members.push_back({ sym });
					}
				}
				return;
			}
			const Symbol &owner = *m_genericOwners.at(receiver.parameter);
			if (!s_path.insert(&owner).second) {
				return;
			}
			for (const TypeParameterBound &bound : receiver.parameter->bounds) {
				if (const TypeReference *ref = std::get_if<TypeReference>(&bound.value)) {
					visit(this->type(*ref, owner));
				}
			}
			s_path.erase(&owner);
			return;
		}
		if (!receiver.declaration || !s_path.insert(receiver.declaration).second) {
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
		} else if (lookup == MemberLookup::CallContract && it_prior->symbol != candidate.symbol && asClass(current.owner)) {
			const auto *md_base     = dynamic_cast<const MethodDeclaration *>(callable(*candidate.symbol));
			const auto *md_override = dynamic_cast<const MethodDeclaration *>(callable(*it_prior->symbol));
			// An override supplies a body. Calls retain the original labels, defaults and generic parameter identities.
			if (md_base && md_base->virtualPrefix && md_override && md_override->overridePrefix) {
				const Symbol *sym_override = it_prior->symbol;
				*it_prior                  = candidate;
				it_prior->overridingMethod = sym_override;
			}
		}
	}
	return unique;
}

bool Analyzer::accepts (const ResolvedType &expected, const ResolvedType &actual) const {
	if (expected == actual) {
		return true;
	}
	if (expected.nullable()) {
		return actual.name == to_string(kNull) || accepts(expected.element(), actual.nullable() ? actual.element() : actual);
	}
	if (actual.nullable()) {
		return false;
	}
	if (expected.array() && actual.array()) {
		if (expected.suffixes.back() != arraySuffix() && actual.suffixes.back() != arraySuffix() &&
		    expected.suffixes.back() != actual.suffixes.back()) {
			return false;
		}
		const ResolvedType left = expected.element(), right = actual.element();
		if (left.nullable() != right.nullable()) {
			return false;
		}
		return left.array() && right.array() ? accepts(left, right) : left == right;
	}
	if (actual.parameter && actual.suffixes.empty()) {
		std::set<const TypeParameterDeclaration *>            visited;
		std::function<bool(const TypeParameterDeclaration &)> check = [&] (const TypeParameterDeclaration &parameter) {
			if (!visited.insert(&parameter).second) {
				return false;
			}
			for (const TypeParameterBound &bound : parameter.bounds) {
				if (const TypeReference *ref = std::get_if<TypeReference>(&bound.value)) {
					const ResolvedType constraint = type(*ref, *m_genericOwners.at(&parameter));
					if (constraint == expected || (constraint.parameter ? check(*constraint.parameter) : accepts(expected, constraint))) {
						return true;
					}
				}
			}
			return false;
		};
		return check(*actual.parameter);
	}
	if (!expected.declaration || !actual.declaration || !actual.suffixes.empty() || !expected.suffixes.empty()) {
		return false;
	}
	for (const ResolvedType &parent : declarations.at(actual.declaration).parents) {
		if (accepts(expected, substitute(parent, argumentsOf(actual)))) {
			return true;
		}
	}
	return false;
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
		if (const MethodDeclaration *mt = dynamic_cast<const MethodDeclaration *>(cs); mt && mt->operatorMember && mt->operatorMember->selector) {
			info.value = type(*mt->operatorMember->selector, symbol);
		}
		if (symbol.kind == SymbolKind::Operator) {
			const Operator              o_operation = symbol.name.get_operator();
			std::optional<ResolvedType> fixed;
			if (o_operation == oIndexRead && info.returns.empty() && !std::holds_alternative<ExpressionBody>(cs->executableBody()->value)) {
				fail(Code::INVALID_OPERATOR_DECLARATION, cs->span, "A block-bodied index getter needs a result type");
			}
			if (!info.value &&
			    (o_operation == oNot || o_operation == oIndexValidate || symbol.name.get_lexeme() == to_string(kIs) || isComparison(o_operation))) {
				fixed = ResolvedType(kBoolean);
			}
			if (symbol.name.get_lexeme() == provider::S_STRING && dynamic_cast<const MethodDeclaration &>(*cs).operatorMember->provider) {
				fixed = ResolvedType(kString);
			}
			if (fixed) {
				if (!cs->returnTypes.empty() && info.returns != ResultTypes{ *fixed }) {
					fail(Code::INVALID_OPERATOR_DECLARATION, cs->span, "Expected operator result " + fixed->spelling());
				}
				info.returns = { *fixed };
			} else if (o_operation == oIndexWrite || isUpdate(o_operation)) {
				if (!info.returns.empty()) {
					fail(Code::INVALID_OPERATOR_DECLARATION, cs->span, std::format("Update operators must return {}", to_string(kVoid)));
				}
			} else if (cs->returnTypes.empty()) {
				if (!std::holds_alternative<ExpressionBody>(cs->executableBody()->value)) {
					fail(Code::INVALID_OPERATOR_DECLARATION, cs->span, "A block-bodied operator requires an explicit result type");
				}
				info.inferResult = true;
			}
		}
	} else if (symbol.kind == SymbolKind::Field) {
		info.value = valueType(fieldType(symbol));
	} else if (symbol.kind == SymbolKind::EnumCase) {
		info.value = ownerType(*info.owner);
	} else if (symbol.kind == SymbolKind::State) {
		const StateBodyDeclaration *state = std::get<const StateBodyDeclaration *>(symbol.declaration);
		info.value                        = type(state->payloadType, symbol);
		if (const TopLevelStateDeclaration *top = dynamic_cast<const TopLevelStateDeclaration *>(state)) {
			info.parents.push_back(valueType(top->host));
		}
	} else if (symbol.kind == SymbolKind::Type) {
		const TypeDeclaration *td = std::get<const TypeDeclaration *>(symbol.declaration);
		if (const ClassDeclaration *cd = dynamic_cast<const ClassDeclaration *>(td)) {
			for (const ClassParentClause &parent : cd->parents) {
				info.parents.push_back(valueType(parent.type));
			}
		} else if (const InterfaceDeclaration *id = dynamic_cast<const InterfaceDeclaration *>(td)) {
			for (const TypeReference &parent : id->parents) {
				info.parents.push_back(valueType(parent));
			}
		}
	}
}

void Analyzer::checkBounds (const ResolvedType &type, const TokenSpan &where, const Symbol &use) const {
	for (const ResolvedType &arg : type.arguments) {
		checkBounds(arg, where, use);
	}
	if (!type.declaration) {
		return;
	}
	const Substitutions args = argumentsOf(type);
	for (const auto &[param, argument] : args) {
		if (!satisfies(*param, args, *type.declaration, use)) {
			fail(Code::UNSATISFIED_GENERIC_BOUND, where, argument.spelling() + " does not satisfy the bounds of " + param->name.get_lexeme());
		}
	}
}

bool Analyzer::satisfies (const TypeParameterDeclaration &parameter, const Substitutions &arguments, const Symbol &declaration, const Symbol &use) const {
	const ResolvedType &actual = arguments.at(&parameter);
	for (const TypeParameterBound &bound : parameter.bounds) {
		if (const TypeReference *ref = std::get_if<TypeReference>(&bound.value)) {
			if (!accepts(substitute(type(*ref, declaration), arguments), actual)) {
				return false;
			}
		} else {
			ResultTypes required;
			for (const ParameterDeclaration &p : std::get<ConstructorBound>(bound.value).declaration.parameters) {
				required.push_back(substitute(type(p.type, declaration), arguments));
			}
			bool found = false;
			if (actual.parameter) {
				for (const TypeParameterBound &available : actual.parameter->bounds) {
					if (const ConstructorBound *ctor = std::get_if<ConstructorBound>(&available.value)) {
						ResultTypes signature;
						for (const ParameterDeclaration &p : ctor->declaration.parameters) {
							signature.push_back(type(p.type, *m_genericOwners.at(actual.parameter)));
						}
						found |= signature == required;
					}
				}
			} else {
				for (const Candidate &ctor : members(actual, actual.name, SymbolKind::Constructor)) {
					ResultTypes signature;
					for (const ResolvedType &p : declarations.at(ctor.symbol).parameters) {
						signature.push_back(substitute(p, ctor.arguments));
					}
					found |= accessible(*ctor.symbol, use) && signature == required;
				}
			}
			if (!found) {
				return false;
			}
		}
	}
	return true;
}

void Analyzer::checkDeclarations () {
	std::set<const Symbol *>            s_done, s_path;
	std::function<void(const Symbol &)> visit = [&] (const Symbol &sym) {
		if (s_done.contains(&sym)) {
			return;
		}
		if (!s_path.insert(&sym).second) {
			fail(Code::INTERFACE_INHERITANCE_CYCLE, sym.name.get_span(), "Cyclic inheritance at '" + sym.name.get_lexeme() + "'");
		}
		const ClassDeclaration  *base = nullptr;
		std::set<const Symbol *> direct;
		for (const ResolvedType &parent : declarations.at(&sym).parents) {
			if (!parent.declaration || !parent.suffixes.empty()) {
				fail(Code::INVALID_INTERFACE_HEADER, sym.name.get_span(), "Expected a named parent type");
			}
			if (!direct.insert(parent.declaration).second) {
				fail(Code::INVALID_INTERFACE_HEADER, sym.name.get_span(), "Duplicate parent type", &parent.declaration->name);
			}
			const ClassDeclaration *parentClass = asClass(parent.declaration);
			if (sym.kind == SymbolKind::State) {
				if (!parentClass) {
					fail(Code::INVALID_STATE_DECLARATION, sym.name.get_span(), "A state requires a class host", &parent.declaration->name);
				}
			} else if (parentClass && asClass(&sym)) {
				if (base || parentClass->finalKeyword) {
					fail(Code::INVALID_CLASS_MEMBER, sym.name.get_span(),
					     base ? "A class can have only one base class" : "Cannot inherit a final class", &parent.declaration->name);
				}
				base = parentClass;
			} else if (!asInterface(parent.declaration)) {
				fail(Code::INVALID_INTERFACE_HEADER, sym.name.get_span(), "Invalid parent type", &parent.declaration->name);
			}
			visit(*parent.declaration);
		}
		s_path.erase(&sym);
		s_done.insert(&sym);
	};
	for (const Symbol *sym : v_order) {
		visit(*sym);
	}
	for (const Symbol *sym : v_order) {
		DeclarationInfo &info = declarations.at(sym);
		if (sym->kind == SymbolKind::StateAttachment) {
			const auto imported = v_names[info.file].find(sym->name.get_lexeme());
			if (imported != v_names[info.file].end()) {
				for (const Symbol *state : imported->second) {
					if (state->kind == SymbolKind::State) {
						info.attachedState = state;
					}
				}
			}
			if (!info.attachedState) {
				fail(Code::INVALID_STATE_ATTACHMENT, sym->name.get_span(), "Unknown attached state '" + sym->name.get_lexeme() + "'");
			}
			const DeclarationInfo &state = declarations.at(info.attachedState);
			if (state.parents.front().declaration != info.owner) {
				fail(Code::INVALID_STATE_ATTACHMENT, sym->name.get_span(), "State is declared for a different host", &info.attachedState->name);
			}
			info.value = state.value;
		}
		if (const TypeParameterList *tp = typeParameters(*sym)) {
			std::set<std::string> names;
			for (const TypeParameterDeclaration &param : tp->parameters) {
				if (!names.insert(param.name.get_lexeme()).second) {
					fail(Code::INVALID_GENERIC_PARAMETER, param.name.get_span(), "Duplicate type parameter");
				}
				if (param.defaultType) {
					(void)type(*param.defaultType, *sym);
				}
				for (const TypeParameterBound &bound : param.bounds) {
					if (const TypeReference *ref = std::get_if<TypeReference>(&bound.value)) {
						(void)type(*ref, *sym);
					} else {
						for (const ParameterDeclaration &p : std::get<ConstructorBound>(bound.value).declaration.parameters) {
							(void)type(p.type, *sym);
						}
					}
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
		if (!callable(*sym) || dynamic_cast<const ConstructorGuaranteeDeclaration *>(callable(*sym))) {
			continue;
		}
		const SymbolScope &scope = info.owner ? info.owner->members : program.units[info.file]->get_globals();
		for (const Symbol &other : scope.at(sym->name.get_lexeme())) {
			if (&other == sym) {
				break;
			}
			if (other.kind != sym->kind) {
				continue;
			}
			if (!sameSignature({ &other }, { sym })) {
				continue;
			}
			if (sym->kind == SymbolKind::Operator) {
				const OperatorMember &a = *dynamic_cast<const MethodDeclaration &>(*callable(other)).operatorMember;
				const OperatorMember &b = *dynamic_cast<const MethodDeclaration &>(*callable(*sym)).operatorMember;
				if (a.rightReceiver != b.rightReceiver || a.provider != b.provider || declarations.at(&other).value != info.value) {
					continue;
				}
			}
			fail(info.owner ? Code::DUPLICATE_METHOD : Code::DUPLICATE_FUNCTION, sym->name.get_span(),
			     "Duplicate signature for '" + sym->name.get_lexeme() + "'", &other.name);
		}
	}
	for (const Symbol *symbol : v_order) {
		if (asClass(symbol) || asInterface(symbol)) {
			checkInterfaces(*symbol);
		}
	}
}

void Analyzer::checkInterfaces (const Symbol &symbol) const {
	const ResolvedType                        receiver = ownerType(symbol);
	std::map<const Symbol *, ResolvedType>    ancestors;
	std::function<void(const ResolvedType &)> visit = [&] (const ResolvedType &type) {
		const Substitutions arguments = argumentsOf(type);
		for (const ResolvedType &base : declarations.at(type.declaration).parents) {
			const ResolvedType parent    = substitute(base, arguments);
			const auto [prior, inserted] = ancestors.emplace(parent.declaration, parent);
			if (inserted) {
				visit(parent);
			} else if (prior->second != parent) {
				fail(Code::CONFLICTING_INTERFACE_APPLICATION, symbol.name.get_span(), "Conflicting applications of " + parent.name,
				     &parent.declaration->name);
			}
		}
	};
	visit(receiver);
	for (const auto &[ancestor, type] : ancestors) {
		if (!asInterface(ancestor)) {
			continue;
		}
		for (const auto &[name, overloads] : ancestor->members) {
			for (const Symbol &method : overloads) {
				if (method.kind != SymbolKind::Method || method.visibility != Visibility::Public) {
					continue;
				}
				const Candidate              requirement{ &method, argumentsOf(type) };
				const std::vector<Candidate> visible        = members(receiver, name, SymbolKind::Method);
				const Symbol                *implementation = nullptr;
				for (const Candidate &candidate : visible) {
					if (!sameSignature(candidate, requirement)) {
						continue;
					}
					if (!sameSignature(candidate, requirement, true) || candidate.symbol->visibility != Visibility::Public) {
						fail(Code::INCOMPATIBLE_INTERFACE_REQUIREMENT, candidate.symbol->name.get_span(),
						     std::format("The implementation of '{}' must be {} and match the interface's result types", name,
						                 to_string(kPublic)),
						     &method.name);
					}
					if (!callable(*candidate.symbol)->executableBody()) {
						continue;
					}
					if (implementation) {
						fail(Code::CONFLICTING_INTERFACE_DEFAULTS, symbol.name.get_span(), "Multiple implementations of '" + name + "'",
						     &candidate.symbol->name);
					}
					implementation = candidate.symbol;
				}
				if (asClass(&symbol) && !implementation) {
					fail(Code::MISSING_INTERFACE_IMPLEMENTATION, symbol.name.get_span(), "Missing implementation of '" + name + "'",
					     &method.name);
				}
			}
		}
	}
}

}
