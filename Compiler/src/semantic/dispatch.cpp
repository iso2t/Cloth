// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/language/objects/States.rst

#include <algorithm>
#include <compare>
#include <cstddef>
#include <format>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <ast/decl/class_declaration.hpp>
#include <ast/decl/declarations.hpp>
#include <ast/decl/state_declaration.hpp>
#include <ast/decl/struct_declaration.hpp>
#include <ast/type/type_parameters.hpp>
#include <ast/type/type_reference.hpp>
#include <semantic/checked_program.hpp>
#include <semantic/checker.hpp>
#include <semantic/symbol_table.hpp>
#include <token/token.hpp>

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
	                          [] (const ParameterDeclaration &a, const ParameterDeclaration &b) { return a.name.getNameKey() == b.name.getNameKey(); });
}

bool hasDefaults (const Symbol &method) {
	return std::ranges::any_of(callable(method)->parameters, &ParameterDeclaration::isOptional);
}

[[noreturn]] void conflict (const Code code, const Symbol &owner, const Symbol &first, const Symbol &second, const std::string &message) {
	throw cde::ClothDiagnostic(code, message,
	                           { { .span = owner.name.getSpan(), .text = message },
	                             { .span = first.name.getSpan(), .text = "first declaration" },
	                             { .span = second.name.getSpan(), .text = "other declaration" } });
}

}

const StateSelectionDeclaration *stateSelection (const Symbol &symbol) {
	if (symbol.kind == SymbolKind::StateAttachment) return std::get<const StateAttachmentDeclaration *>(symbol.declaration);
	return symbol.kind == SymbolKind::State ? dynamic_cast<const NestedStateDeclaration *>(std::get<const StateBodyDeclaration *>(symbol.declaration)) :
	                                          nullptr;
}

bool Analyzer::sameMethod (const MethodRef &left, const MethodRef &right, const bool results) const {
	// Distinct templates in one owner keep their slots even when closing the owner makes their parameter types coincide.
	if (left.symbol != right.symbol && left.symbol->kind == SymbolKind::Method && right.symbol->kind == SymbolKind::Method &&
	    declarations.at(left.symbol).owner == declarations.at(right.symbol).owner && typeParameters(*left.symbol))
		return false;
	return left.symbol->name.getNameKey() == right.symbol->name.getNameKey() &&
	       sameSignature({ .symbol = left.symbol, .arguments = argumentsOf(left.owner) }, { .symbol = right.symbol, .arguments = argumentsOf(right.owner) },
	                     results);
}

Substitutions Analyzer::methodArguments (const MethodRef &source, const Symbol &target) const {
	Substitutions m_arguments = argumentsOf(source.owner);
	if (const TypeParameterList *tpl_source = typeParameters(*source.symbol)) {
		const TypeParameterList &tpl_target = *typeParameters(target);
		for (std::size_t st_index = 0; st_index < tpl_source->parameters.size(); ++st_index) {
			const TypeParameterDeclaration &tp_target = tpl_target.parameters[st_index];
			ResolvedType                    rt_slot(tp_target.name.getNameKey());
			rt_slot.parameter = &tp_target;
			m_arguments.emplace(&tpl_source->parameters[st_index], std::move(rt_slot));
		}
	}
	return m_arguments;
}

bool Analyzer::sameMethodBounds (const MethodRef &left, const MethodRef &right, const bool constructorContracts) const {
	const TypeParameterList *tpl_left = typeParameters(*left.symbol);
	if (!tpl_left || left == right) return true;
	using Bounds               = std::multiset<std::variant<ResolvedType, ConstructorShape>>;
	const std::function bounds = [&] (const MethodRef &method, const std::size_t index) {
		Bounds              s_bounds;
		const Substitutions m_arguments   = methodArguments(method, *left.symbol);
		const auto [parameter, arguments] = parameterContract(typeParameters(*method.symbol)->parameters[index]);
		const Symbol       &sym_origin    = *m_genericOwners.at(parameter);
		const std::function resolve       = [&] (const TypeReference &reference) {
                        return substitute(substitute(type(reference, sym_origin), arguments), m_arguments);
		};
		for (const auto &[value] : parameter->bounds) {
			if (const TypeReference *tr_type = std::get_if<TypeReference>(&value)) {
				s_bounds.emplace(resolve(*tr_type));
			} else {
				const auto      &cb_bound = std::get<ConstructorBound>(value);
				ConstructorShape cs_shape;
				for (const ParameterDeclaration &pd_param : cb_bound.declaration.parameters) {
					cs_shape.parameters.push_back(resolve(pd_param.type));
					if (constructorContracts) {
						cs_shape.labels.push_back(pd_param.name.getNameKey());
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
		if (const auto it_host = m_attachmentContracts.find(rt_activeAttachment->declaration); it_host != m_attachmentContracts.end()) {
			if (const auto it_origin = it_host->second.find(&parameter); it_origin != it_host->second.end()) return it_origin->second;
		}
	}
	const auto it_origin = m_parameterContracts.find(&parameter);
	return it_origin == m_parameterContracts.end() ? ParameterContract{ .parameter = &parameter, .arguments = {} } : it_origin->second;
}

void Analyzer::inheritMethodBounds (const MethodRef &body, const MethodRef &contract, const Code code) const {
	const TypeParameterList *tpl_body = typeParameters(*body.symbol);
	if (!tpl_body) return;
	const TypeParameterList &tpl_contract = *typeParameters(*contract.symbol);
	const Substitutions      m_arguments  = methodArguments(contract, *body.symbol);
	for (std::size_t st_index = 0; st_index < tpl_body->parameters.size(); ++st_index) {
		const TypeParameterDeclaration &tp_parameter = tpl_body->parameters[st_index];
		if (!tp_parameter.bounds.empty() || (tp_parameter.defaultType && code != Code::InvalidInterfaceOverride)) {
			fail(code, tp_parameter.getSpan(), "Declare inherited method bounds and defaults on the original contract", &contract.symbol->name);
		}
		if (body.owner == ownerType(*body.owner.declaration)) {
			ParameterContracts &m_contracts = standaloneState(*body.symbol) ? m_attachmentContracts[body.owner.declaration] : m_parameterContracts;
			m_contracts.try_emplace(&tp_parameter, ParameterContract{ .parameter = &tpl_contract.parameters[st_index], .arguments = m_arguments });
		}
	}
}

const TypeDispatch &Analyzer::dispatchFor (const ResolvedType &receiver) const {
	if (const auto it_cached = m_dispatch.find(receiver); it_cached != m_dispatch.end()) return it_cached->second;
	if (const ResolvedType rt_declaration = ownerType(*receiver.declaration); receiver != rt_declaration) {
		dispatchFor(rt_declaration);
		// Interface requirements must have distinct slots as soon as their owner closes.
		if (asInterface(receiver.declaration)) {
			const Substitutions m_arguments = argumentsOf(receiver);
			for (const auto &val : receiver.declaration->members | std::views::values)
				for (const Symbol &sym_method : val)
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
			if (const auto it_prior = std::ranges::find(td_table.interfaces, rt_view.declaration, &ResolvedType::declaration);
			    it_prior == td_table.interfaces.end())
				td_table.interfaces.push_back(rt_view);
			else if (*it_prior != rt_view) {
				fail(Code::ConflictingInterfaceApplication, receiver.declaration->name.getSpan(),
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
				const MethodRef mr_method{ .symbol = &sym_method, .owner = rt_view };
				if (sym_method.visibility != Visibility::Public) {
					if (receiver == rt_view)
						table.methods.push_back(
						        { .contract       = mr_method,
						          .implementation = callable(sym_method)->executableBody() ? std::optional{ mr_method } : std::nullopt,
						          .kind           = DispatchKind::Direct });
					continue;
				}
				std::vector<MethodGroup> &v_groups = m_groups[entry.first];
				const auto                it_group =
				        std::ranges::find_if(v_groups, [&] (const MethodGroup &group) { return sameMethod(group.front(), mr_method); });
				if (it_group == v_groups.end())
					v_groups.push_back({ mr_method });
				else
					it_group->push_back(mr_method);
			}
		}
	}

	if (asClass(&sym_owner)) {
		for (const auto &val : sym_owner.members | std::views::values) {
			for (const Symbol &sym_method : val) {
				if (sym_method.kind == SymbolKind::Method) addClassMethod({ .symbol = &sym_method, .owner = receiver }, m_groups, table);
			}
		}
	}
	for (const auto &val : m_groups | std::views::values) {
		for (const MethodGroup &v_group : val) mergeRequirements(receiver, v_group, table);
	}
}

void Analyzer::addClassMethod (const MethodRef &method, const MethodGroups &requirements, TypeDispatch &table) const {
	const Symbol            &sym_method = *method.symbol;
	const MethodDeclaration &md_method  = classMethod(method);
	const auto               it_base    = std::ranges::find_if(table.methods, [&] (const MethodSlot &slot) { return sameMethod(slot.contract, method); });
	if (it_base != table.methods.end() && it_base->kind == DispatchKind::Virtual) {
		const Symbol &sym_root = *it_base->contract.symbol;
		if (md_method.overridePrefix && typeParameters(sym_method) &&
		    std::any_of(std::next(it_base), table.methods.end(),
		                [&] (const MethodSlot &slot) { return slot.kind == DispatchKind::Virtual && sameMethod(slot.contract, method); }))
			fail(Code::InvalidVirtualMethod, sym_method.name.getSpan(), "Override matches multiple generic virtual methods", &sym_root.name);
		if (!md_method.overridePrefix)
			fail(Code::InvalidVirtualMethod, sym_method.name.getSpan(),
			     std::format("Overriding '{}' requires {}", sym_method.name.getLexeme(), toString(KOverride)), &sym_root.name);
		if (sym_method.visibility != Visibility::Public)
			fail(Code::InvalidVirtualMethod, sym_method.name.getSpan(),
			     std::format("Override of '{}' requires {}", sym_method.name.getLexeme(), toString(KPublic)), &sym_root.name);
		if (!sameMethod(it_base->contract, method, true))
			fail(Code::InvalidVirtualMethod, sym_method.name.getSpan(),
			     "Override of '" + sym_method.name.getLexeme() + "' has different ordered result types", &sym_root.name);
		if (hasDefaults(sym_method))
			fail(Code::InvalidVirtualMethod, sym_method.name.getSpan(), "Declare argument defaults on the original virtual method", &sym_root.name);
		it_base->implementation = method;
		inheritMethodBounds(method, it_base->contract, Code::InvalidVirtualMethod);
		return;
	}
	if (md_method.virtualPrefix && sym_method.visibility != Visibility::Public)
		fail(Code::InvalidVirtualMethod, sym_method.name.getSpan(), std::format("A virtual method requires {}", toString(KPublic)));
	if (md_method.overridePrefix) {
		if (const auto it_groups = requirements.find(sym_method.name.getNameKey());
		    it_groups == requirements.end() ||
		    std::ranges::none_of(it_groups->second, [&] (const MethodGroup &group) { return sameMethod(group.front(), method); })) {
			const auto it_named = std::ranges::find_if(table.methods, [&] (const MethodSlot &slot) {
				return slot.contract.symbol->name.getNameKey() == sym_method.name.getNameKey();
			});
			fail(typeParameters(sym_method) ? Code::InvalidVirtualMethod : Code::InvalidInterfaceOverride, sym_method.name.getSpan(),
			     "No matching virtual method or interface requirement for '" + sym_method.name.getLexeme() + "'",
			     it_named == table.methods.end() ? nullptr : &it_named->contract.symbol->name);
		}
	}
	const MethodSlot ms_method{ .contract       = method,
		                    .implementation = method,
		                    .kind           = md_method.virtualPrefix ? DispatchKind::Virtual : DispatchKind::Direct };
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
			conflict(Code::IncompatibleInterfaceRequirement, sym_owner, *mr_first.symbol, *mr_method.symbol,
			         "Incompatible interface requirements for '" + mr_first.symbol->name.getLexeme() + "'");
		}
	}
	MethodGroup v_contracts, v_defaults;
	for (const MethodRef &mr_method : group) {
		const bool b_default  = callable(*mr_method.symbol)->executableBody();
		bool       b_shadowed = false, b_replaced = false;
		for (const auto &[symbol, owner] : group) {
			if (owner == mr_method.owner) continue;
			if (const ResultTypes &v_ancestors = owner == receiver ? table.interfaces : dispatchFor(owner).interfaces;
			    std::ranges::find(v_ancestors, mr_method.owner) == v_ancestors.end())
				continue;
			b_shadowed = true;
			if (callable(*symbol)->executableBody()) b_replaced = true;
		}
		if (!b_shadowed) v_contracts.push_back(mr_method);
		if (b_default && !b_replaced) v_defaults.push_back(mr_method);
	}
	const MethodRef &mr_contract = v_contracts.front();
	if (asInterface(&sym_owner)) {
		for (const MethodSlot &ms_private : table.methods) {
			if (ms_private.kind == DispatchKind::Direct && sameMethod(ms_private.contract, mr_contract))
				fail(Code::IncompatibleInterfaceRequirement, ms_private.contract.symbol->name.getSpan(),
				     "An inherited public requirement must remain accessible", &mr_contract.symbol->name);
		}
		for (const MethodRef &mr_other : v_contracts) {
			if (mr_other == mr_contract) continue;
			if (!sameLabels(*mr_contract.symbol, *mr_other.symbol) || hasDefaults(*mr_contract.symbol) || hasDefaults(*mr_other.symbol) ||
			    !sameTypeDefaults(mr_contract, mr_other)) {
				conflict(Code::IncompatibleInterfaceRequirement, sym_owner, *mr_contract.symbol, *mr_other.symbol,
				         "Redeclare '" + mr_first.symbol->name.getLexeme() + "' to select its argument labels and defaults");
			}
		}
		if (mr_contract.owner == receiver && !callable(*mr_contract.symbol)->executableBody() && !v_defaults.empty())
			fail(Code::IncompatibleInterfaceRequirement, mr_contract.symbol->name.getSpan(), "An inherited default needs a replacement body",
			     &v_defaults.front().symbol->name);
		table.methods.push_back({ .contract       = mr_contract,
		                          .implementation = v_defaults.size() == 1 ? std::optional{ v_defaults.front() } : std::nullopt,
		                          .kind           = DispatchKind::Interface });
		return;
	}
	implementRequirements(receiver, group, mr_contract, v_defaults, table);
}

void Analyzer::implementRequirements (const ResolvedType &receiver, const MethodGroup &group, const MethodRef &contract, const MethodGroup &defaults,
                                      TypeDispatch &table) const {
	const Symbol            &sym_owner = *receiver.declaration;
	const auto               it_class  = std::ranges::find_if(table.methods, [&] (const MethodSlot &slot) {
                return slot.kind != DispatchKind::Interface && sameMethod(slot.contract, contract);
        });
	std::optional<MethodRef> opt_target;
	if (it_class != table.methods.end()) {
		if (const MethodRef &mr_body = *it_class->implementation;
		    !sameMethod(mr_body, contract, true) || mr_body.symbol->visibility != Visibility::Public) {
			if (mr_body.owner == receiver || mr_body.symbol->visibility == Visibility::Public)
				fail(classMethod(mr_body).overridePrefix ? Code::InvalidInterfaceOverride : Code::IncompatibleInterfaceRequirement,
				     mr_body.symbol->name.getSpan(),
				     std::format("Interface implementation requires {} and matching ordered result types", toString(KPublic)),
				     &contract.symbol->name);
		} else {
			opt_target = mr_body;
			if (mr_body.owner == receiver && it_class->kind != DispatchKind::Virtual && classMethod(mr_body).overridePrefix) {
				for (const MethodRef &mr_other : group) {
					if (!sameMethodBounds(contract, mr_other, true))
						conflict(Code::IncompatibleInterfaceRequirement, sym_owner, *contract.symbol, *mr_other.symbol,
						         "Constructor guarantee labels or defaults differ");
				}
				inheritMethodBounds(mr_body, contract, Code::InvalidInterfaceOverride);
			}
			if (!sameMethodBounds(mr_body, contract))
				fail(Code::InvalidInterfaceOverride, mr_body.symbol->name.getSpan(), "Interface method bounds differ", &contract.symbol->name);
			if (mr_body.owner == receiver && !defaults.empty() && !classMethod(mr_body).overridePrefix)
				fail(Code::InvalidInterfaceOverride, mr_body.symbol->name.getSpan(),
				     std::format("Replacing an interface default requires {}", toString(KOverride)), &defaults.front().symbol->name);
		}
	}
	if (!opt_target) {
		if (defaults.size() > 1)
			conflict(Code::ConflictingInterfaceDefaults, sym_owner, *defaults[0].symbol, *defaults[1].symbol,
			         "Conflicting interface defaults for '" + contract.symbol->name.getLexeme() + "'");
		if (defaults.empty())
			fail(Code::MissingInterfaceImplementation, sym_owner.name.getSpan(),
			     "Missing implementation of '" + contract.symbol->name.getLexeme() + "'", &contract.symbol->name);
		opt_target = defaults.front();
		const MethodSlot ms_fallback{ .contract = *opt_target, .implementation = opt_target, .kind = DispatchKind::Interface };
		if (it_class == table.methods.end())
			table.methods.push_back(ms_fallback);
		else
			*it_class = ms_fallback;
	}
	for (const ResolvedType &rt_view : table.interfaces) {
		for (const MethodSlot &ms_requirement : dispatchFor(rt_view).methods) {
			if (ms_requirement.kind == DispatchKind::Interface && sameMethod(ms_requirement.contract, contract))
				table.interfaceTargets.push_back({ .view = rt_view, .contract = ms_requirement.contract, .implementation = *opt_target });
		}
	}
}

void Analyzer::buildStates (const ResolvedType &receiver, TypeDispatch &table) const {
	const Symbol &sym_host = *receiver.declaration;
	for (const auto &val : sym_host.members | std::views::values) {
		for (const Symbol &sym_member : val) {
			if (sym_member.kind != SymbolKind::StateMethodContract) continue;
			const MethodRef mr_contract{ .symbol = &sym_member, .owner = receiver };
			for (const StateMethod &sm_prior : table.stateMethods) {
				if (sameMethod(sm_prior.contract, mr_contract))
					fail(Code::InvalidStateContract, sym_member.name.getSpan(), "State method contract is already declared",
					     &sm_prior.contract.symbol->name);
			}
			table.stateMethods.push_back({ .contract = mr_contract, .implementations = {} });
		}
	}
	const std::size_t st_inheritedStates = table.states.size();
	const Symbol     *sym_initial        = nullptr;
	for (const ClassMember &cm_member : asClass(&sym_host)->members) {
		const StateClassMemberPtr       *sc_member = std::get_if<StateClassMemberPtr>(&cm_member.value);
		const StateSelectionDeclaration *ss_state  = sc_member ? dynamic_cast<const StateSelectionDeclaration *>(sc_member->get()) : nullptr;
		if (!ss_state) continue;
		const SymbolKind       sk_kind       = dynamic_cast<const NestedStateDeclaration *>(ss_state) ? SymbolKind::State : SymbolKind::StateAttachment;
		const Symbol          &sym_selection = *std::ranges::find(sym_host.members.at(ss_state->getName().getNameKey()), sk_kind, &Symbol::kind);
		const DeclarationInfo &di_info       = declarations.at(&sym_selection);
		const Symbol          &sym_definition                     = di_info.attachedState ? *di_info.attachedState : sym_selection;
		const ResolvedType     rt_payload                         = substitute(*di_info.value, argumentsOf(receiver));
		const std::pair<StateSelections::iterator, bool> it_state = table.states.emplace(
		        sym_selection.name.getNameKey(),
		        StateSelection{ .selection = &sym_selection, .definition = &sym_definition, .owner = receiver, .payload = rt_payload });
		if (!it_state.second)
			fail(Code::InvalidStateDeclaration, sym_selection.name.getSpan(), "State is already supported on this host",
			     &it_state.first->second.selection->name);
		if (ss_state->initial) {
			if (sym_initial)
				fail(Code::InvalidStateDeclaration, ss_state->initial->keyword.getSpan(), "A class can select only one initial state",
				     &stateSelection(*sym_initial)->initial->keyword);
			table.initialState = sym_initial = &sym_selection;
		}
		for (const auto &val : sym_definition.members | std::views::values) {
			for (const Symbol &sym_method : val) {
				if (sym_method.kind != SymbolKind::Method) continue;
				const MethodRef mr_body{ .symbol = &sym_method, .owner = receiver };
				if (hasDefaults(sym_method))
					fail(Code::InvalidStateContract, sym_method.name.getSpan(),
					     "Declare state method argument defaults on the host contract");
				if (const TypeParameterList *tpl_method = typeParameters(sym_method)) {
					for (const TypeParameterDeclaration &tp_parameter : tpl_method->parameters) {
						if (tp_parameter.defaultType)
							fail(Code::InvalidStateContract, tp_parameter.name.getSpan(),
							     "Declare state method type defaults on the host contract");
					}
				}
				auto it_contract = std::ranges::find_if(table.stateMethods,
				                                        [&] (const StateMethod &method) { return sameMethod(method.contract, mr_body); });
				if (it_contract == table.stateMethods.end()) {
					table.stateMethods.push_back({ .contract = mr_body, .implementations = { { &sym_selection, mr_body } } });
					continue;
				}
				const MethodRef &mr_contract = it_contract->contract;
				if (mr_contract.symbol->kind == SymbolKind::StateMethodContract)
					inheritMethodBounds(mr_body, mr_contract, Code::InvalidStateContract);
				if (!sameMethod(mr_contract, mr_body, true) ||
				    (mr_contract.symbol->kind != SymbolKind::StateMethodContract &&
				     (!sameLabels(*mr_contract.symbol, sym_method) || mr_contract.symbol->visibility != sym_method.visibility ||
				      !sameMethodBounds(mr_contract, mr_body, true)))) {
					fail(Code::InvalidStateContract, sym_method.name.getSpan(), "State method does not match its shared contract",
					     &mr_contract.symbol->name);
				}
				it_contract->implementations.emplace(&sym_selection, mr_body);
			}
		}
	}
	if (!sym_initial && table.states.size() != st_inheritedStates)
		fail(Code::InvalidStateDeclaration, sym_host.name.getSpan(),
		     std::format("Class '{}' must select an initial state with '{}'", sym_host.name.getLexeme(), toString(KDefault)));
}

bool Analyzer::hasStateName (const Symbol &context, const std::string_view name) const {
	for (const auto &val : v_names[declarations.at(&context).file] | std::views::values) {
		for (const Symbol *sym : val) {
			if (sym->kind == SymbolKind::State && sym->name.getNameKey() == name) return true;
			if (asClass(sym) && dispatchFor(ownerType(*sym)).states.contains(name)) return true;
		}
	}
	return false;
}

void Analyzer::publishDispatch () const {
	// Only declarations and committed instances need tables after checking.
	for (const Symbol *sym_type : v_order) {
		if (asClass(sym_type) || asInterface(sym_type)) {
			const ResolvedType rt_owner = ownerType(*sym_type);
			program.dispatchTables.insert(m_dispatch.extract(rt_owner));
		}
	}
	for (const auto &key : program.instances | std::views::keys) {
		const Symbol *sym_type = key.declaration;
		if (!asClass(sym_type) && !asInterface(sym_type)) continue;
		ResolvedType rt_owner = ownerType(*sym_type);
		applyArguments(rt_owner, key.arguments);
		program.dispatchTables.insert(m_dispatch.extract(rt_owner));
	}
	for (auto &[owner, table] : program.dispatchTables) {
		if (!owner.classReference() || owner.dependent()) continue;
		table.formatter = formatterFor(owner);
		for (const Operator op : { OEqual, ONotEqual }) table.referenceEquality.emplace(op, referenceEquality(owner, op));
	}
}

}
