// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <compare>
#include <cstddef>
#include <format>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <semantic/checker.hpp>

namespace cloth::semantic {
namespace {

struct ConstructorShape {
	ResultTypes              parameters;
	std::vector<std::string> labels;
	const ConstructorBound  *defaults                                    = nullptr;
	std::strong_ordering     operator<=>(const ConstructorShape &) const = default;
	bool                     operator==(const ConstructorShape &) const  = default;
};

const MethodDeclaration &classMethod (const MethodRef &method) {
	return dynamic_cast<const MethodDeclaration &>(*callable(*method.symbol));
}

bool sameLabels (const Symbol &left, const Symbol &right) {
	return std::ranges::equal(callable(left)->parameters, callable(right)->parameters,
	                          [] (const ParameterDeclaration &a, const ParameterDeclaration &b) { return a.name.get_lexeme() == b.name.get_lexeme(); });
}

bool hasDefaults (const Symbol &method) {
	return std::ranges::any_of(callable(method)->parameters, &ParameterDeclaration::isOptional);
}

[[noreturn]] void conflict (Code code, const Symbol &owner, const Symbol &first, const Symbol &second, const std::string &message) {
	throw cde::ClothDiagnostic(
	        code, message,
	        { { owner.name.get_span(), message }, { first.name.get_span(), "first declaration" }, { second.name.get_span(), "other declaration" } });
}

const StateSelectionDeclaration *stateSelection (const Symbol &symbol) {
	if (symbol.kind == SymbolKind::StateAttachment) return std::get<const StateAttachmentDeclaration *>(symbol.declaration);
	return dynamic_cast<const NestedStateDeclaration *>(std::get<const StateBodyDeclaration *>(symbol.declaration));
}

}

bool Analyzer::sameMethod (const MethodRef &left, const MethodRef &right, bool results) const {
	// Distinct templates in one owner keep their slots even when closing the owner makes their parameter types coincide.
	if (left.symbol != right.symbol && left.symbol->kind == SymbolKind::Method && right.symbol->kind == SymbolKind::Method &&
	    declarations.at(left.symbol).owner == declarations.at(right.symbol).owner && typeParameters(*left.symbol))
		return false;
	return left.symbol->name.get_lexeme() == right.symbol->name.get_lexeme() &&
	       sameSignature({ left.symbol, argumentsOf(left.owner) }, { right.symbol, argumentsOf(right.owner) }, results);
}

Substitutions Analyzer::methodArguments (const MethodRef &source, const Symbol &target) const {
	Substitutions m_arguments = argumentsOf(source.owner);
	if (const TypeParameterList *tpl_source = typeParameters(*source.symbol)) {
		const TypeParameterList &tpl_target = *typeParameters(target);
		for (std::size_t st_index = 0; st_index < tpl_source->parameters.size(); ++st_index) {
			const TypeParameterDeclaration &tp_target = tpl_target.parameters[st_index];
			ResolvedType                    rt_slot(tp_target.name.get_lexeme());
			rt_slot.parameter = &tp_target;
			m_arguments.emplace(&tpl_source->parameters[st_index], std::move(rt_slot));
		}
	}
	return m_arguments;
}

bool Analyzer::sameMethodBounds (const MethodRef &left, const MethodRef &right, bool constructorContracts) const {
	const TypeParameterList *tpl_left = typeParameters(*left.symbol);
	if (!tpl_left || left == right) return true;
	using Bounds                                                       = std::multiset<std::variant<ResolvedType, ConstructorShape>>;
	const std::function<Bounds(const MethodRef &, std::size_t)> bounds = [&] (const MethodRef &method, std::size_t index) {
		Bounds                                                   s_bounds;
		const Substitutions                                      m_arguments = methodArguments(method, *left.symbol);
		const ParameterContract                                  pc_origin   = parameterContract(typeParameters(*method.symbol)->parameters[index]);
		const Symbol                                            &sym_origin  = *m_genericOwners.at(pc_origin.parameter);
		const std::function<ResolvedType(const TypeReference &)> resolve     = [&] (const TypeReference &reference) {
                        return substitute(substitute(type(reference, sym_origin), pc_origin.arguments), m_arguments);
		};
		for (const TypeParameterBound &tp_bound : pc_origin.parameter->bounds) {
			if (const TypeReference *tr_type = std::get_if<TypeReference>(&tp_bound.value)) {
				s_bounds.emplace(resolve(*tr_type));
			} else {
				const ConstructorBound &cb_bound = std::get<ConstructorBound>(tp_bound.value);
				ConstructorShape        cs_shape;
				for (const ParameterDeclaration &pd_param : cb_bound.declaration.parameters) {
					cs_shape.parameters.push_back(resolve(pd_param.type));
					if (constructorContracts) {
						cs_shape.labels.push_back(pd_param.name.get_lexeme());
						if (pd_param.isOptional()) cs_shape.defaults = &cb_bound;
					}
				}
				s_bounds.emplace(std::move(cs_shape));
			}
		}
		return s_bounds;
	};
	for (std::size_t st_index = 0; st_index < tpl_left->parameters.size(); ++st_index)
		if (bounds(left, st_index) != bounds(right, st_index)) return false;
	return true;
}

bool Analyzer::sameTypeDefaults (const MethodRef &left, const MethodRef &right) const {
	const TypeParameterList *tpl_left = typeParameters(*left.symbol);
	if (!tpl_left) return true;
	const TypeParameterList &tpl_right = *typeParameters(*right.symbol);
	for (std::size_t st_index = 0; st_index < tpl_left->parameters.size(); ++st_index) {
		const std::optional<TypeReference> &opt_left  = tpl_left->parameters[st_index].defaultType,
		                                   &opt_right = tpl_right.parameters[st_index].defaultType;
		if (opt_left.has_value() != opt_right.has_value()) return false;
		if (opt_left && substitute(type(*opt_left, *left.symbol), argumentsOf(left.owner)) !=
		                        substitute(type(*opt_right, *right.symbol), methodArguments(right, *left.symbol)))
			return false;
	}
	return true;
}

Analyzer::ParameterContract Analyzer::parameterContract (const TypeParameterDeclaration &parameter) const {
	if (rt_activeAttachment) {
		const std::map<const Symbol *, ParameterContracts>::const_iterator it_host = m_attachmentContracts.find(rt_activeAttachment->declaration);
		if (it_host != m_attachmentContracts.end()) {
			const ParameterContracts::const_iterator it_origin = it_host->second.find(&parameter);
			if (it_origin != it_host->second.end()) return it_origin->second;
		}
	}
	const ParameterContracts::const_iterator it_origin = m_parameterContracts.find(&parameter);
	return it_origin == m_parameterContracts.end() ? ParameterContract{ &parameter, {} } : it_origin->second;
}

void Analyzer::inheritMethodBounds (const MethodRef &body, const MethodRef &contract, Code code) const {
	const TypeParameterList *tpl_body = typeParameters(*body.symbol);
	if (!tpl_body) return;
	const TypeParameterList &tpl_contract = *typeParameters(*contract.symbol);
	const Substitutions      m_arguments  = methodArguments(contract, *body.symbol);
	for (std::size_t st_index = 0; st_index < tpl_body->parameters.size(); ++st_index) {
		const TypeParameterDeclaration &tp_parameter = tpl_body->parameters[st_index];
		if (!tp_parameter.bounds.empty() || (tp_parameter.defaultType && code != Code::INVALID_INTERFACE_OVERRIDE)) {
			fail(code, tp_parameter.get_span(), "Declare inherited method bounds and defaults on the original contract", &contract.symbol->name);
		}
		if (body.owner == ownerType(*body.owner.declaration)) {
			ParameterContracts &m_contracts = standaloneState(*body.symbol) ? m_attachmentContracts[body.owner.declaration] : m_parameterContracts;
			m_contracts.try_emplace(&tp_parameter, ParameterContract{ &tpl_contract.parameters[st_index], m_arguments });
		}
	}
}

const TypeDispatch &Analyzer::dispatchFor (const ResolvedType &receiver) const {
	const std::map<ResolvedType, TypeDispatch>::const_iterator it_cached = m_dispatch.find(receiver);
	if (it_cached != m_dispatch.end()) return it_cached->second;
	const ResolvedType rt_declaration = ownerType(*receiver.declaration);
	if (receiver != rt_declaration) {
		dispatchFor(rt_declaration);
		// Interface requirements must have distinct slots as soon as their owner closes.
		if (asInterface(receiver.declaration)) {
			const Substitutions m_arguments = argumentsOf(receiver);
			for (const SymbolScope::value_type &entry : receiver.declaration->members)
				for (const Symbol &sym_method : entry.second)
					if (sym_method.kind == SymbolKind::Method && typeParameters(sym_method))
						checkOverload(sym_method, m_arguments, iw_active ? &iw_active->where : nullptr);
		}
	}
	TypeDispatch        td_table;
	const Substitutions m_arguments = argumentsOf(receiver);
	for (const ResolvedType &rt_pattern : declarations.at(receiver.declaration).parents) {
		const ResolvedType  rt_parent = substitute(rt_pattern, m_arguments);
		const TypeDispatch &td_parent = dispatchFor(rt_parent);
		if (asClass(rt_parent.declaration)) {
			for (const MethodSlot &ms_slot : td_parent.methods) {
				if (ms_slot.kind != DispatchKind::Interface) td_table.methods.push_back(ms_slot);
			}
			td_table.states       = td_parent.states;
			td_table.initialState = td_parent.initialState;
			td_table.stateMethods = td_parent.stateMethods;
		}
		for (const ResolvedType &rt_view : td_parent.interfaces) {
			const ResultTypes::const_iterator it_prior = std::ranges::find(td_table.interfaces, rt_view.declaration, &ResolvedType::declaration);
			if (it_prior == td_table.interfaces.end())
				td_table.interfaces.push_back(rt_view);
			else if (*it_prior != rt_view) {
				fail(Code::CONFLICTING_INTERFACE_APPLICATION, receiver.declaration->name.get_span(),
				     "Conflicting applications of " + it_prior->spelling() + " and " + rt_view.spelling(), &rt_view.declaration->name);
			}
		}
	}
	if (asInterface(receiver.declaration)) td_table.interfaces.push_back(receiver);
	buildMethods(receiver, td_table);
	if (asClass(receiver.declaration)) buildStates(receiver, td_table);
	return m_dispatch.emplace(receiver, std::move(td_table)).first->second;
}

void Analyzer::buildMethods (const ResolvedType &receiver, TypeDispatch &table) const {
	const Symbol &sym_owner = *receiver.declaration;
	MethodGroups  m_groups;
	for (const ResolvedType &rt_view : table.interfaces) {
		for (const SymbolScope::value_type &entry : rt_view.declaration->members) {
			for (const Symbol &sym_method : entry.second) {
				if (sym_method.kind != SymbolKind::Method) continue;
				const MethodRef mr_method{ &sym_method, rt_view };
				if (sym_method.visibility != Visibility::Public) {
					if (receiver == rt_view)
						table.methods.push_back({ mr_method,
						                          callable(sym_method)->executableBody() ? std::optional{ mr_method } : std::nullopt,
						                          DispatchKind::Direct });
					continue;
				}
				std::vector<MethodGroup>                &v_groups = m_groups[entry.first];
				const std::vector<MethodGroup>::iterator it_group =
				        std::ranges::find_if(v_groups, [&] (const MethodGroup &group) { return sameMethod(group.front(), mr_method); });
				if (it_group == v_groups.end())
					v_groups.push_back({ mr_method });
				else
					it_group->push_back(mr_method);
			}
		}
	}

	if (asClass(&sym_owner)) {
		for (const SymbolScope::value_type &entry : sym_owner.members) {
			for (const Symbol &sym_method : entry.second) {
				if (sym_method.kind == SymbolKind::Method) addClassMethod({ &sym_method, receiver }, m_groups, table);
			}
		}
	}
	for (const MethodGroups::value_type &entry : m_groups) {
		for (const MethodGroup &v_group : entry.second) mergeRequirements(receiver, v_group, table);
	}
}

void Analyzer::addClassMethod (const MethodRef &method, const MethodGroups &requirements, TypeDispatch &table) const {
	const Symbol                           &sym_method = *method.symbol;
	const MethodDeclaration                &md_method  = classMethod(method);
	const std::vector<MethodSlot>::iterator it_base =
	        std::ranges::find_if(table.methods, [&] (const MethodSlot &slot) { return sameMethod(slot.contract, method); });
	if (it_base != table.methods.end() && it_base->kind == DispatchKind::Virtual) {
		const Symbol &sym_root = *it_base->contract.symbol;
		if (md_method.overridePrefix && typeParameters(sym_method) &&
		    std::any_of(std::next(it_base), table.methods.end(),
		                [&] (const MethodSlot &slot) { return slot.kind == DispatchKind::Virtual && sameMethod(slot.contract, method); }))
			fail(Code::INVALID_VIRTUAL_METHOD, sym_method.name.get_span(), "Override matches multiple generic virtual methods", &sym_root.name);
		if (!md_method.overridePrefix)
			fail(Code::INVALID_VIRTUAL_METHOD, sym_method.name.get_span(),
			     std::format("Overriding '{}' requires {}{}", sym_method.name.get_lexeme(), to_string(kOverride), to_string(oColon)),
			     &sym_root.name);
		if (sym_method.visibility != Visibility::Public)
			fail(Code::INVALID_VIRTUAL_METHOD, sym_method.name.get_span(),
			     std::format("Override of '{}' requires {}", sym_method.name.get_lexeme(), to_string(kPublic)), &sym_root.name);
		if (!sameMethod(it_base->contract, method, true))
			fail(Code::INVALID_VIRTUAL_METHOD, sym_method.name.get_span(),
			     "Override of '" + sym_method.name.get_lexeme() + "' has different ordered result types", &sym_root.name);
		if (hasDefaults(sym_method))
			fail(Code::INVALID_VIRTUAL_METHOD, sym_method.name.get_span(), "Declare argument defaults on the original virtual method",
			     &sym_root.name);
		it_base->implementation = method;
		inheritMethodBounds(method, it_base->contract, Code::INVALID_VIRTUAL_METHOD);
		return;
	}
	if (md_method.virtualPrefix && sym_method.visibility != Visibility::Public)
		fail(Code::INVALID_VIRTUAL_METHOD, sym_method.name.get_span(), std::format("A virtual method requires {}", to_string(kPublic)));
	if (md_method.overridePrefix) {
		const MethodGroups::const_iterator it_groups = requirements.find(sym_method.name.get_lexeme());
		if (it_groups == requirements.end() ||
		    std::ranges::none_of(it_groups->second, [&] (const MethodGroup &group) { return sameMethod(group.front(), method); })) {
			const std::vector<MethodSlot>::const_iterator it_named = std::ranges::find_if(table.methods, [&] (const MethodSlot &slot) {
				return slot.contract.symbol->name.get_lexeme() == sym_method.name.get_lexeme();
			});
			fail(typeParameters(sym_method) ? Code::INVALID_VIRTUAL_METHOD : Code::INVALID_INTERFACE_OVERRIDE, sym_method.name.get_span(),
			     "No matching virtual method or interface requirement for '" + sym_method.name.get_lexeme() + "'",
			     it_named == table.methods.end() ? nullptr : &it_named->contract.symbol->name);
		}
	}
	const MethodSlot ms_method{ method, method, md_method.virtualPrefix ? DispatchKind::Virtual : DispatchKind::Direct };
	if (it_base == table.methods.end())
		table.methods.push_back(ms_method);
	else
		*it_base = ms_method;
}

void Analyzer::mergeRequirements (const ResolvedType &receiver, const MethodGroup &group, TypeDispatch &table) const {
	const Symbol    &sym_owner = *receiver.declaration;
	const MethodRef &mr_first  = group.front();
	for (const MethodRef &mr_method : group) {
		if (!sameMethod(mr_first, mr_method, true) || !sameMethodBounds(mr_first, mr_method)) {
			conflict(Code::INCOMPATIBLE_INTERFACE_REQUIREMENT, sym_owner, *mr_first.symbol, *mr_method.symbol,
			         "Incompatible interface requirements for '" + mr_first.symbol->name.get_lexeme() + "'");
		}
	}
	MethodGroup v_contracts, v_defaults;
	for (const MethodRef &mr_method : group) {
		const bool b_default  = callable(*mr_method.symbol)->executableBody();
		bool       b_shadowed = false, b_replaced = false;
		for (const MethodRef &mr_other : group) {
			if (mr_other.owner == mr_method.owner) continue;
			const ResultTypes &v_ancestors = mr_other.owner == receiver ? table.interfaces : dispatchFor(mr_other.owner).interfaces;
			if (std::ranges::find(v_ancestors, mr_method.owner) == v_ancestors.end()) continue;
			b_shadowed = true;
			if (callable(*mr_other.symbol)->executableBody()) b_replaced = true;
		}
		if (!b_shadowed) v_contracts.push_back(mr_method);
		if (b_default && !b_replaced) v_defaults.push_back(mr_method);
	}
	const MethodRef &mr_contract = v_contracts.front();
	if (asInterface(&sym_owner)) {
		for (const MethodSlot &ms_private : table.methods) {
			if (ms_private.kind == DispatchKind::Direct && sameMethod(ms_private.contract, mr_contract))
				fail(Code::INCOMPATIBLE_INTERFACE_REQUIREMENT, ms_private.contract.symbol->name.get_span(),
				     "An inherited public requirement must remain accessible", &mr_contract.symbol->name);
		}
		for (const MethodRef &mr_other : v_contracts) {
			if (mr_other == mr_contract) continue;
			if (!sameLabels(*mr_contract.symbol, *mr_other.symbol) || hasDefaults(*mr_contract.symbol) || hasDefaults(*mr_other.symbol) ||
			    !sameTypeDefaults(mr_contract, mr_other)) {
				conflict(Code::INCOMPATIBLE_INTERFACE_REQUIREMENT, sym_owner, *mr_contract.symbol, *mr_other.symbol,
				         "Redeclare '" + mr_first.symbol->name.get_lexeme() + "' to select its argument labels and defaults");
			}
		}
		if (mr_contract.owner == receiver && !callable(*mr_contract.symbol)->executableBody() && !v_defaults.empty())
			fail(Code::INCOMPATIBLE_INTERFACE_REQUIREMENT, mr_contract.symbol->name.get_span(), "An inherited default needs a replacement body",
			     &v_defaults.front().symbol->name);
		table.methods.push_back({ mr_contract, v_defaults.size() == 1 ? std::optional{ v_defaults.front() } : std::nullopt, DispatchKind::Interface });
		return;
	}
	implementRequirements(receiver, group, mr_contract, v_defaults, table);
}

void Analyzer::implementRequirements (const ResolvedType &receiver, const MethodGroup &group, const MethodRef &contract, const MethodGroup &defaults,
                                      TypeDispatch &table) const {
	const Symbol                           &sym_owner = *receiver.declaration;
	const std::vector<MethodSlot>::iterator it_class  = std::ranges::find_if(table.methods, [&] (const MethodSlot &slot) {
                return slot.kind != DispatchKind::Interface && sameMethod(slot.contract, contract);
        });
	std::optional<MethodRef>                opt_target;
	if (it_class != table.methods.end()) {
		const MethodRef &mr_body = *it_class->implementation;
		if (!sameMethod(mr_body, contract, true) || mr_body.symbol->visibility != Visibility::Public) {
			if (mr_body.owner == receiver || mr_body.symbol->visibility == Visibility::Public)
				fail(classMethod(mr_body).overridePrefix ? Code::INVALID_INTERFACE_OVERRIDE : Code::INCOMPATIBLE_INTERFACE_REQUIREMENT,
				     mr_body.symbol->name.get_span(),
				     std::format("Interface implementation requires {} and matching ordered result types", to_string(kPublic)),
				     &contract.symbol->name);
		} else {
			opt_target = mr_body;
			if (mr_body.owner == receiver && it_class->kind != DispatchKind::Virtual && classMethod(mr_body).overridePrefix) {
				for (const MethodRef &mr_other : group) {
					if (!sameMethodBounds(contract, mr_other, true))
						conflict(Code::INCOMPATIBLE_INTERFACE_REQUIREMENT, sym_owner, *contract.symbol, *mr_other.symbol,
						         "Constructor guarantee labels or defaults differ");
				}
				inheritMethodBounds(mr_body, contract, Code::INVALID_INTERFACE_OVERRIDE);
			}
			if (!sameMethodBounds(mr_body, contract))
				fail(Code::INVALID_INTERFACE_OVERRIDE, mr_body.symbol->name.get_span(), "Interface method bounds differ",
				     &contract.symbol->name);
			if (mr_body.owner == receiver && !defaults.empty() && !classMethod(mr_body).overridePrefix)
				fail(Code::INVALID_INTERFACE_OVERRIDE, mr_body.symbol->name.get_span(),
				     std::format("Replacing an interface default requires {}{}", to_string(kOverride), to_string(oColon)),
				     &defaults.front().symbol->name);
		}
	}
	if (!opt_target) {
		if (defaults.size() > 1)
			conflict(Code::CONFLICTING_INTERFACE_DEFAULTS, sym_owner, *defaults[0].symbol, *defaults[1].symbol,
			         "Conflicting interface defaults for '" + contract.symbol->name.get_lexeme() + "'");
		if (defaults.empty())
			fail(Code::MISSING_INTERFACE_IMPLEMENTATION, sym_owner.name.get_span(),
			     "Missing implementation of '" + contract.symbol->name.get_lexeme() + "'", &contract.symbol->name);
		opt_target = defaults.front();
		const MethodSlot ms_fallback{ *opt_target, opt_target, DispatchKind::Interface };
		if (it_class == table.methods.end())
			table.methods.push_back(ms_fallback);
		else
			*it_class = ms_fallback;
	}
	for (const ResolvedType &rt_view : table.interfaces) {
		for (const MethodSlot &ms_requirement : dispatchFor(rt_view).methods) {
			if (ms_requirement.kind == DispatchKind::Interface && sameMethod(ms_requirement.contract, contract))
				table.interfaceTargets.push_back({ rt_view, ms_requirement.contract, *opt_target });
		}
	}
}

void Analyzer::buildStates (const ResolvedType &receiver, TypeDispatch &table) const {
	const Symbol &sym_host = *receiver.declaration;
	for (const SymbolScope::value_type &entry : sym_host.members) {
		for (const Symbol &sym_member : entry.second) {
			if (sym_member.kind != SymbolKind::StateMethodContract) continue;
			const MethodRef mr_contract{ &sym_member, receiver };
			for (const StateMethod &sm_prior : table.stateMethods) {
				if (sameMethod(sm_prior.contract, mr_contract))
					fail(Code::INVALID_STATE_CONTRACT, sym_member.name.get_span(), "State method contract is already declared",
					     &sm_prior.contract.symbol->name);
			}
			table.stateMethods.push_back({ mr_contract, {} });
		}
	}
	const std::size_t st_inheritedStates = table.states.size();
	const Symbol     *sym_initial        = nullptr;
	for (const ClassMember &cm_member : asClass(&sym_host)->members) {
		const StateClassMemberPtr       *sc_member = std::get_if<StateClassMemberPtr>(&cm_member.value);
		const StateSelectionDeclaration *ss_state  = sc_member ? dynamic_cast<const StateSelectionDeclaration *>(sc_member->get()) : nullptr;
		if (!ss_state) continue;
		const SymbolKind       sk_kind       = dynamic_cast<const NestedStateDeclaration *>(ss_state) ? SymbolKind::State : SymbolKind::StateAttachment;
		const Symbol          &sym_selection = *std::ranges::find(sym_host.members.at(ss_state->get_name().get_lexeme()), sk_kind, &Symbol::kind);
		const DeclarationInfo &di_info       = declarations.at(&sym_selection);
		const Symbol          &sym_definition = di_info.attachedState ? *di_info.attachedState : sym_selection;
		const ResolvedType     rt_payload     = substitute(*di_info.value, argumentsOf(receiver));
		const std::pair<StateSelections::iterator, bool> it_state =
		        table.states.emplace(sym_selection.name.get_lexeme(), StateSelection{ &sym_selection, &sym_definition, receiver, rt_payload });
		if (!it_state.second)
			fail(Code::INVALID_STATE_DECLARATION, sym_selection.name.get_span(), "State is already supported on this host",
			     &it_state.first->second.selection->name);
		if (ss_state->initial) {
			if (sym_initial)
				fail(Code::INVALID_STATE_DECLARATION, ss_state->initial->keyword.get_span(), "A class can select only one initial state",
				     &stateSelection(*sym_initial)->initial->keyword);
			table.initialState = sym_initial = &sym_selection;
		}
		for (const SymbolScope::value_type &entry : sym_definition.members) {
			for (const Symbol &sym_method : entry.second) {
				if (sym_method.kind != SymbolKind::Method) continue;
				const MethodRef mr_body{ &sym_method, receiver };
				if (hasDefaults(sym_method))
					fail(Code::INVALID_STATE_CONTRACT, sym_method.name.get_span(),
					     "Declare state method argument defaults on the host contract");
				if (const TypeParameterList *tpl_method = typeParameters(sym_method)) {
					for (const TypeParameterDeclaration &tp_parameter : tpl_method->parameters) {
						if (tp_parameter.defaultType)
							fail(Code::INVALID_STATE_CONTRACT, tp_parameter.name.get_span(),
							     "Declare state method type defaults on the host contract");
					}
				}
				std::vector<StateMethod>::iterator it_contract = std::ranges::find_if(table.stateMethods, [&] (const StateMethod &method) {
					return sameMethod(method.contract, mr_body);
				});
				if (it_contract == table.stateMethods.end()) {
					table.stateMethods.push_back({ mr_body, { { &sym_selection, mr_body } } });
					continue;
				}
				const MethodRef &mr_contract = it_contract->contract;
				if (mr_contract.symbol->kind == SymbolKind::StateMethodContract)
					inheritMethodBounds(mr_body, mr_contract, Code::INVALID_STATE_CONTRACT);
				if (!sameMethod(mr_contract, mr_body, true) ||
				    (mr_contract.symbol->kind != SymbolKind::StateMethodContract &&
				     (!sameLabels(*mr_contract.symbol, sym_method) || mr_contract.symbol->visibility != sym_method.visibility ||
				      !sameMethodBounds(mr_contract, mr_body, true)))) {
					fail(Code::INVALID_STATE_CONTRACT, sym_method.name.get_span(), "State method does not match its shared contract",
					     &mr_contract.symbol->name);
				}
				it_contract->implementations.emplace(&sym_selection, mr_body);
			}
		}
	}
	if (!sym_initial && table.states.size() != st_inheritedStates)
		fail(Code::INVALID_STATE_DECLARATION, sym_host.name.get_span(),
		     std::format("Class '{}' must select an initial state with '{}{}'", sym_host.name.get_lexeme(), to_string(kDefault), to_string(oColon)));
}

bool Analyzer::hasStateName (const Symbol &context, std::string_view name) const {
	for (const Names::value_type &entry : v_names[declarations.at(&context).file]) {
		for (const Symbol *sym : entry.second) {
			if (sym->kind == SymbolKind::State && sym->name.get_lexeme() == name) return true;
			if (asClass(sym) && dispatchFor(ownerType(*sym)).states.contains(name)) return true;
		}
	}
	return false;
}

void Analyzer::publishDispatch () {
	// Only declarations and committed instances need tables after checking.
	for (const Symbol *sym_type : v_order) {
		if (asClass(sym_type) || asInterface(sym_type)) {
			const ResolvedType rt_owner = ownerType(*sym_type);
			program.dispatchTables.insert(m_dispatch.extract(rt_owner));
		}
	}
	for (const std::pair<const InstanceKey, GenericInstance> &entry : program.instances) {
		const Symbol *sym_type = entry.first.declaration;
		if (!asClass(sym_type) && !asInterface(sym_type)) continue;
		ResolvedType rt_owner = ownerType(*sym_type);
		applyArguments(rt_owner, entry.first.arguments);
		program.dispatchTables.insert(m_dispatch.extract(rt_owner));
	}
}

}
