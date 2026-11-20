// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <compare>
#include <format>
#include <functional>
#include <tuple>

#include <ast/decl/struct_declaration.hpp>
#include <semantic/checker.hpp>

namespace cloth {

std::strong_ordering InstanceKey::operator<=>(const InstanceKey &other) const {
	if (const std::strong_ordering so_decl = std::compare_three_way{}(declaration, other.declaration); so_decl != 0) return so_decl;
	return std::tie(receiver, arguments, operatorWidth) <=> std::tie(other.receiver, other.arguments, other.operatorWidth);
}

}

namespace cloth::semantic {
namespace {

constexpr std::size_t MAX_GENERIC_DEPENDENCIES = 64;
constexpr std::size_t MAX_GENERIC_INSTANCES    = 4096;

bool dependsOnTypeParameter (const InstanceKey &key) {
	return (key.receiver && key.receiver->dependent()) || std::ranges::any_of(key.arguments, &ResolvedType::dependent);
}
}

DeclarationInfo Analyzer::specialize (DeclarationInfo info, const Substitutions &arguments) const {
	for (ResultTypes *v_types : { &info.parameters, &info.returns, &info.parents }) {
		for (ResolvedType &rt_type : *v_types) rt_type = substitute(std::move(rt_type), arguments);
	}
	if (info.value) info.value = substitute(std::move(*info.value), arguments);
	return info;
}

const Symbol *Analyzer::instanceOwner (const Symbol &symbol) const {
	for (const Symbol *sym_owner = declarations.at(&symbol).owner; sym_owner; sym_owner = declarations.at(sym_owner).owner) {
		if (sym_owner->kind == SymbolKind::Type) return sym_owner;
	}
	return nullptr;
}

bool Analyzer::isTemplate (const Symbol &symbol) const {
	for (const Symbol *sym_scope = &symbol; sym_scope; sym_scope = declarations.at(sym_scope).owner) {
		if (typeParameters(*sym_scope)) return true;
		if (sym_scope->kind == SymbolKind::Type) break;
	}
	return false;
}

const Symbol *Analyzer::constructorFor (const ResolvedType &type, const ResultTypes &parameters, const Symbol *access) const {
	for (const Candidate &c_ctor : members(type, type.name, SymbolKind::Constructor)) {
		const ResultTypes &v_signature = declarations.at(c_ctor.symbol).parameters;
		if (v_signature.size() == parameters.size() && (!access || accessible(*c_ctor.symbol, *access)) &&
		    std::ranges::equal(v_signature, parameters, {}, [&] (const ResolvedType &type) { return substitute(type, c_ctor.arguments); })) {
			return c_ctor.symbol;
		}
	}
	return nullptr;
}

const GenericInstance *Analyzer::useCallable (const Symbol &symbol, const Substitutions &arguments, const Symbol &context, const TokenSpan &where,
                                              const ResolvedType *stateHost, unsigned operatorWidth) {
	if (widthSelector(symbol)) {
		if (!operatorWidth) return nullptr;
	} else if (!isTemplate(symbol) || dynamic_cast<const ConstructorGuaranteeDeclaration *>(callable(symbol))) {
		return nullptr;
	}
	InstanceKey ik_key{ &symbol, std::nullopt, {}, operatorWidth };
	if (const Symbol *sym_owner = instanceOwner(symbol))
		ik_key.receiver = substitute(ownerType(*sym_owner), arguments);
	else if (standaloneState(symbol)) {
		const ResolvedType *rt_host = stateHost ? stateHost : rt_activeAttachment;
		if (rt_host) ik_key.receiver = *rt_host;
	}
	if (const TypeParameterList *tpl_parameters = typeParameters(symbol)) {
		for (const TypeParameterDeclaration &tp_parameter : tpl_parameters->parameters) ik_key.arguments.push_back(arguments.at(&tp_parameter));
	}
	return useInstance(std::move(ik_key), context, where);
}

// A generic dispatch can be seen before or after its concrete receiver enters the instance worklist.
void Analyzer::useDispatch (const CallDispatch &dispatch, const Substitutions &arguments, const Symbol &context, const TokenSpan &where) {
	const TypeParameterList *tpl_parameters = typeParameters(*dispatch.contract.symbol);
	if (!tpl_parameters || (iw_active ? !iw_active->instance : isTemplate(context) || (rt_activeAttachment && rt_activeAttachment->dependent()))) return;
	InstanceKey ik_key{ dispatch.contract.symbol, dispatch.receiver, {} };
	for (const TypeParameterDeclaration &tp_parameter : tpl_parameters->parameters) ik_key.arguments.push_back(arguments.at(&tp_parameter));
	const std::pair<DispatchUses::iterator, bool> it_use = m_dispatchUses.try_emplace(std::move(ik_key), DispatchUse{ dispatch, where });
	if (it_use.second) {
		for (const ResolvedType &rt_receiver : s_receivers) instantiateTargets(it_use.first->first, it_use.first->second, rt_receiver);
	}
}

void Analyzer::registerReceiver (const ResolvedType &receiver) {
	if (s_receivers.insert(receiver).second) {
		for (const DispatchUses::value_type &entry : m_dispatchUses) instantiateTargets(entry.first, entry.second, receiver);
	}
}

void Analyzer::instantiateTargets (const InstanceKey &key, const DispatchUse &use, const ResolvedType &receiver) {
	if (!accepts(use.call.receiver, receiver)) return;
	const TypeDispatch                          &td_table    = dispatchFor(receiver);
	const std::function<void(const MethodRef &)> instantiate = [&] (const MethodRef &body) {
		enqueue({ body.symbol, body.owner, key.arguments }, use.where, false);
	};
	if (use.call.kind == DispatchKind::Interface) {
		const ResolvedType &rt_view = asInterface(use.call.receiver.declaration) ? use.call.receiver : use.call.contract.owner;
		for (const InterfaceTarget &it_target : td_table.interfaceTargets) {
			if (it_target.view == rt_view && it_target.contract == use.call.contract) instantiate(it_target.implementation);
		}
	} else if (use.call.kind == DispatchKind::Virtual) {
		for (const MethodSlot &ms_slot : td_table.methods) {
			if (ms_slot.contract == use.call.contract && ms_slot.implementation) instantiate(*ms_slot.implementation);
		}
	} else if (use.call.kind == DispatchKind::State) {
		for (const StateMethod &sm_method : td_table.stateMethods) {
			if (sm_method.contract == use.call.contract) {
				for (const std::pair<const Symbol *const, MethodRef> &entry : sm_method.implementations) instantiate(entry.second);
			}
		}
	}
}

const GenericInstance *Analyzer::guaranteeOwner (const Symbol &guarantee, const Substitutions &arguments, const Symbol &context, const TokenSpan &where) {
	const Symbol &sym_owner = *declarations.at(&guarantee).owner;
	if (sym_owner.kind != SymbolKind::Type) return useCallable(sym_owner, arguments, context, where);
	return useInstance({ &sym_owner, std::nullopt, substitute(ownerType(sym_owner), arguments).arguments() }, context, where);
}

void Analyzer::useType (const ResolvedType &type, const Symbol &context, const TokenSpan &where) {
	type.visitApplications([&] (const ResolvedType &rt_type) {
		useFormatterClass(rt_type);
		if (rt_type.declaration) useInstance({ rt_type.declaration, std::nullopt, rt_type.arguments() }, context, where);
	});
}

const GenericInstance *Analyzer::useInstance (InstanceKey key, const Symbol &context, const TokenSpan &where) {
	if (!iw_active && (isTemplate(context) || (rt_activeAttachment && rt_activeAttachment->dependent()))) {
		m_uses[&context].push_back({ std::move(key), where });
		return nullptr;
	}
	const bool b_symbolic = iw_active && !iw_active->instance;
	if (!b_symbolic && dependsOnTypeParameter(key)) {
		fail(Code::GENERIC_INFERENCE_FAILED, where, "Cannot determine the type arguments of '" + key.declaration->name.get_lexeme() + "'",
		     &key.declaration->name);
	}
	return enqueue(std::move(key), where, b_symbolic);
}

std::optional<GenericLimit> genericLimit (unsigned nesting, std::size_t depth, std::size_t count, bool symbolic) {
	if (nesting > MAX_TYPE_ARG_NESTING)
		return GenericLimit{ Code::GENERIC_SUBSTITUTION_NESTING,
			             std::format("Generic type argument nesting limit exceeded ({})", MAX_TYPE_ARG_NESTING) };
	if (depth > MAX_GENERIC_DEPENDENCIES)
		return GenericLimit{ symbolic ? Code::GENERIC_TEMPLATE_DEPENDENCY_LIMIT : Code::GENERIC_DEPENDENCY_LIMIT,
			             std::format("Generic {} dependency limit exceeded ({})", symbolic ? "template analysis" : "instantiation",
			                         MAX_GENERIC_DEPENDENCIES) };
	if (count >= MAX_GENERIC_INSTANCES)
		return GenericLimit{ symbolic ? Code::GENERIC_TEMPLATE_INSTANCE_LIMIT : Code::GENERIC_INSTANCE_LIMIT,
			             std::format("Too many {} generic instances ({})", symbolic ? "symbolic" : "distinct", MAX_GENERIC_INSTANCES) };
	return {};
}

void failGenericLimit (const GenericLimit &limit, const Token &declaration, const TokenSpan &use, const std::vector<GenericDependency> &ancestry) {
	std::vector<cde::SourceLabel>                                                                v_labels;
	std::set<std::tuple<const SourceFile *, std::size_t, std::size_t, std::size_t, std::size_t>> s_labeled;
	const std::function                                                                          add = [&] (const TokenSpan &span, std::string text) {
                if (s_labeled.emplace(span.source.get(), span.startLine, span.startCol, span.endLine, span.endCol).second)
                        v_labels.push_back({ span, std::move(text) });
	};
	add(use, "this application exceeds the generic resource limit");
	add(declaration.get_span(), "generic template declared here");
	for (std::size_t st_index = 0; st_index < ancestry.size(); ++st_index) {
		const GenericDependency &gd_step = ancestry[ancestry.size() - st_index - 1];
		add(gd_step.use, std::format("dependency {}: '{}' required here", st_index + 1, gd_step.name));
	}
	throw cde::ClothDiagnostic(limit.code, limit.message, std::move(v_labels));
}

const GenericInstance *Analyzer::enqueue (InstanceKey key, const TokenSpan &where, bool symbolic) {
	const InstanceKey *ik_stored;
	GenericInstance   *gi_instance = nullptr;
	if (symbolic) {
		const std::pair<std::set<InstanceKey>::iterator, bool> it_entry = s_templates.emplace(std::move(key));
		if (!it_entry.second) return nullptr;
		ik_stored = &*it_entry.first;
	} else {
		const std::pair<std::map<InstanceKey, GenericInstance>::iterator, bool> it_entry = program.instances.try_emplace(std::move(key));
		gi_instance                                                                      = &it_entry.first->second;
		if (!it_entry.second) return gi_instance;
		ik_stored = &it_entry.first->first;
	}
	// A non-generic member of a specialized receiver shares its owner's budget charge.
	const bool        b_charged = !ik_stored->arguments.empty();
	const std::size_t st_depth  = (iw_active ? iw_active->depth : 0) + b_charged;
	// Template analysis can reach a fully concrete application before any executable code calls it.
	const bool   b_open   = symbolic && dependsOnTypeParameter(*ik_stored);
	std::size_t &st_count = !symbolic ? st_instances : b_open ? st_openTemplates : st_closedTemplates;
	unsigned     u_depth  = ik_stored->receiver ? ik_stored->receiver->argumentDepth() : 0;
	for (const ResolvedType &rt_arg : ik_stored->arguments) u_depth = std::max(u_depth, 1 + rt_arg.argumentDepth());
	const Symbol &sym_declaration = *ik_stored->declaration;
	if (const std::optional<GenericLimit> opt_limit = genericLimit(u_depth, st_depth, b_charged ? st_count : 0, b_open)) {
		std::vector<GenericDependency> v_ancestry;
		for (const InstanceWork *iw_step = iw_active; iw_step; iw_step = iw_step->parent)
			if (!iw_step->key.arguments.empty()) v_ancestry.push_back({ iw_step->where, iw_step->key.declaration->name.get_lexeme() });
		failGenericLimit(*opt_limit, sym_declaration.name, where, v_ancestry);
	}
	st_count += b_charged;
	(symbolic ? q_templates : q_instances).push_back({ *ik_stored, gi_instance, iw_active, where, st_depth });
	return gi_instance;
}

void Analyzer::collectSignature (const Symbol &symbol) {
	const DeclarationInfo &di_info = declarations.at(&symbol);
	useDeclaredBounds(symbol, {});
	if (const CallableSignature *cs_signature = callable(symbol)) {
		for (std::size_t st_index = 0; st_index < di_info.parameters.size(); ++st_index) {
			useType(di_info.parameters[st_index], symbol, cs_signature->parameters[st_index].type.get_span());
		}
		for (std::size_t st_index = 0; st_index < di_info.returns.size(); ++st_index) {
			useType(di_info.returns[st_index], symbol,
			        st_index < cs_signature->returnTypes.size() ? cs_signature->returnTypes[st_index].get_span() : symbol.name.get_span());
		}
	}
	if (di_info.value) useType(*di_info.value, symbol, symbol.name.get_span());
	for (const ResolvedType &rt_parent : di_info.parents) useType(rt_parent, symbol, symbol.name.get_span());
}

void Analyzer::useDeclaredBounds (const Symbol &symbol, const Substitutions &arguments) {
	const TypeParameterList *tpl_parameters = typeParameters(symbol);
	if (!tpl_parameters) return;
	for (const TypeParameterDeclaration &tp_parameter : tpl_parameters->parameters) {
		for (const TypeParameterBound &tp_bound : tp_parameter.bounds) {
			if (const TypeReference *tr_bound = std::get_if<TypeReference>(&tp_bound.value)) {
				useType(substitute(type(*tr_bound, symbol), arguments), symbol, tr_bound->get_span());
			}
		}
		if (tp_parameter.defaultType) {
			useType(substitute(type(*tp_parameter.defaultType, symbol), arguments), symbol, tp_parameter.defaultType->get_span());
		}
	}
}

void Analyzer::visitTypeMembers (const ResolvedType &type, const Substitutions &arguments, GenericInstance *instance) {
	const Symbol &sym_type = *type.declaration;
	StorageEdges *v_edges  = nullptr;
	if (asStruct(&sym_type)) {
		const std::pair<std::map<ResolvedType, StorageEdges>::iterator, bool> it_layout = m_storage.try_emplace(type);
		if (it_layout.second) v_edges = &it_layout.first->second;
	}
	for (const SymbolScope::value_type &entry : sym_type.members) {
		for (const Symbol &sym_member : entry.second) {
			if (iw_active && !typeParameters(sym_member)) checkOverload(sym_member, arguments, &iw_active->where);
			if (sym_member.kind == SymbolKind::Field) {
				DeclarationInfo di_field = specialize(declarations.at(&sym_member), arguments);
				ResolvedType    rt_field = *di_field.value;
				if (iw_active) useType(rt_field, sym_member, sym_member.name.get_span());
				if (instance) {
					instance->fields.emplace(&sym_member, rt_field);
					BodyChecker(*this, sym_member, instance->body, arguments, di_field).check();
				}
				if (v_edges && asStruct(rt_field.declaration)) {
					rt_field.suffixes.clear();
					v_edges->emplace_back(std::move(rt_field), &sym_member);
				}
			} else if (iw_active && !typeParameters(sym_member) &&
			           (callable(sym_member) || sym_member.kind == SymbolKind::State || sym_member.kind == SymbolKind::StateAttachment)) {
				useCallable(sym_member, arguments, sym_member, sym_member.name.get_span());
			}
		}
	}
}

Substitutions Analyzer::instanceArguments (const InstanceKey &key) const {
	Substitutions m_arguments = key.receiver ? argumentsOf(*key.receiver) : Substitutions{};
	if (const TypeParameterList *tpl_parameters = typeParameters(*key.declaration)) {
		for (std::size_t st_index = 0; st_index < key.arguments.size(); ++st_index) {
			m_arguments.emplace(&tpl_parameters->parameters[st_index], key.arguments[st_index]);
		}
	}
	return m_arguments;
}

bool Analyzer::hasConstructorProof (const ResolvedType &type, const ResultTypes &parameters) const {
	if (!iw_active) return false;
	const Substitutions m_arguments = instanceArguments(iw_active->key);
	for (const Substitutions::value_type &entry : m_arguments) {
		if (entry.second != type) continue;
		const ParameterContract          pc_contract     = parameterContract(*entry.first);
		const Guarantees::const_iterator it_requirements = m_guarantees.find(pc_contract.parameter);
		if (it_requirements == m_guarantees.end()) continue;
		for (const Symbol *sym_requirement : it_requirements->second) {
			if (specialize(specialize(declarations.at(sym_requirement), pc_contract.arguments), m_arguments).parameters == parameters) return true;
		}
	}
	return false;
}

void Analyzer::completeInstance (const InstanceWork &work) {
	const Symbol                             &sym_declaration = *work.key.declaration;
	const Substitutions                       m_arguments     = instanceArguments(work.key);
	const std::function<void(const Symbol &)> dependencies    = [&] (const Symbol &owner) {
                const std::unordered_map<const Symbol *, std::vector<InstanceUse>>::const_iterator it_uses = m_uses.find(&owner);
                if (it_uses == m_uses.end()) return;
                for (const InstanceUse &iu_use : it_uses->second) {
                        InstanceKey ik_next = iu_use.key;
                        if (ik_next.receiver) ik_next.receiver = substitute(std::move(*ik_next.receiver), m_arguments);
                        for (ResolvedType &rt_arg : ik_next.arguments) rt_arg = substitute(std::move(rt_arg), m_arguments);
                        enqueue(std::move(ik_next), iu_use.where, true);
                }
	};
	if (sym_declaration.kind == SymbolKind::Type) {
		const ResolvedType rt_type = substitute(ownerType(sym_declaration), m_arguments);
		useFormatterClass(rt_type);
		visitTypeMembers(rt_type, m_arguments, work.instance);
		if (asClass(&sym_declaration) || asInterface(&sym_declaration)) dispatchFor(rt_type);
		if (work.instance && asClass(&sym_declaration)) {
			registerReceiver(rt_type);
			for (const StateSelections::value_type &entry_state : dispatchFor(rt_type).states) {
				const StateSelection &ss_state = entry_state.second;
				if (ss_state.owner != rt_type || ss_state.definition == ss_state.selection) continue;
				for (const SymbolScope::value_type &entry : ss_state.definition->members) {
					for (const Symbol &sym_member : entry.second) {
						if (!typeParameters(sym_member)) enqueue({ &sym_member, rt_type, {} }, work.where, false);
					}
				}
			}
		}
	} else if (sym_declaration.kind == SymbolKind::State) {
		for (const SymbolScope::value_type &entry : sym_declaration.members) {
			for (const Symbol &sym_member : entry.second) {
				if (!typeParameters(sym_member)) useCallable(sym_member, m_arguments, sym_member, sym_member.name.get_span());
			}
		}
	}
	if (const TypeParameterList *tpl_parameters = typeParameters(sym_declaration)) {
		for (const TypeParameterDeclaration &tp_parameter : tpl_parameters->parameters) {
			const Guarantees::const_iterator it_requirements = m_guarantees.find(&tp_parameter);
			if (it_requirements == m_guarantees.end()) continue;
			for (const Symbol *sym_requirement : it_requirements->second) {
				if (!work.instance) {
					dependencies(*sym_requirement);
				} else {
					DeclarationInfo di_requirement = specialize(declarations.at(sym_requirement), m_arguments);
					BodyChecker(*this, *sym_requirement, work.instance->body, m_arguments, di_requirement).check();
				}
			}
		}
	}
	if (!work.instance) {
		if (work.key.operatorWidth) widthTemplate(sym_declaration, work.key.operatorWidth);
		dependencies(sym_declaration);
		if (sym_declaration.kind == SymbolKind::Type) {
			for (const SymbolScope::value_type &entry : sym_declaration.members) {
				for (const Symbol &sym_member : entry.second)
					if (sym_member.kind == SymbolKind::Field) dependencies(sym_member);
			}
		}
	} else {
		GenericInstance &gi_instance = *work.instance;
		if (work.key.operatorWidth && isTemplate(sym_declaration)) widthTemplate(sym_declaration, work.key.operatorWidth);
		gi_instance.signature = work.key.operatorWidth ? widthSignature(sym_declaration, m_arguments, work.key.operatorWidth) :
		                                                 specialize(declarations.at(&sym_declaration), m_arguments);
		useDeclaredBounds(sym_declaration, m_arguments);
		for (const ResultTypes *v_types : { &gi_instance.signature.parameters, &gi_instance.signature.returns, &gi_instance.signature.parents }) {
			for (const ResolvedType &rt_type : *v_types) useType(rt_type, sym_declaration, work.where);
		}
		if (gi_instance.signature.value) useType(*gi_instance.signature.value, sym_declaration, work.where);
		if (work.key.receiver && standaloneState(sym_declaration))
			checkAttachedBody(sym_declaration, *work.key.receiver, gi_instance.body, m_arguments, gi_instance.signature);
		else
			BodyChecker(*this, sym_declaration, gi_instance.body, m_arguments, gi_instance.signature, nullptr, work.key.operatorWidth).check();
		if (work.key.operatorWidth && !isTemplate(sym_declaration)) checkPublicSurface(sym_declaration, nullptr, &gi_instance.signature);
	}
}

void Analyzer::checkStorage () const {
	// Only a type already on the current field path makes the layout recursive.
	std::set<ResolvedType>    s_active, s_done;
	std::vector<StorageVisit> v_path;
	for (const std::pair<const ResolvedType, StorageEdges> &entry : m_storage) {
		if (s_done.contains(entry.first)) continue;
		s_active.insert(entry.first);
		v_path.push_back({ entry.first, entry.second });
		while (!v_path.empty()) {
			StorageVisit &v_current = v_path.back();
			if (v_current.next == v_current.fields.size()) {
				s_active.erase(v_current.type);
				s_done.insert(v_current.type);
				v_path.pop_back();
				continue;
			}
			const std::pair<ResolvedType, const Symbol *> &edge = v_current.fields[v_current.next++];
			if (s_done.contains(edge.first)) continue;
			if (!s_active.insert(edge.first).second) {
				fail(Code::RECURSIVE_STRUCT, edge.second->name.get_span(),
				     std::format("Recursive storage in {} '{}'", to_string(kStruct), edge.first.name), &edge.first.declaration->name);
			}
			v_path.push_back({ edge.first, m_storage.at(edge.first) });
		}
	}
}

void Analyzer::checkInstances () {
	for (const Symbol *sym_declaration : v_order) {
		const TypeParameterList *tpl_parameters = typeParameters(*sym_declaration);
		if (tpl_parameters) {
			InstanceKey ik_key{ sym_declaration, std::nullopt, {} };
			for (const TypeParameterDeclaration &tp_parameter : tpl_parameters->parameters) {
				ResolvedType rt_argument(tp_parameter.name.get_name_key());
				rt_argument.parameter = &tp_parameter;
				ik_key.arguments.push_back(std::move(rt_argument));
			}
			if (callable(*sym_declaration)) {
				if (const Symbol *sym_owner = instanceOwner(*sym_declaration)) ik_key.receiver = ownerType(*sym_owner);
			}
			enqueue(std::move(ik_key), sym_declaration->name.get_span(), true);
		} else if (asStruct(sym_declaration)) {
			visitTypeMembers(ownerType(*sym_declaration), {}, nullptr);
		} else if (asClass(sym_declaration)) {
			registerReceiver(ownerType(*sym_declaration));
		}
	}
	// A map entry is the provisional identity. Each new entry appends one job, including calls discovered in another body.
	for (std::deque<InstanceWork> *q_work : { &q_templates, &q_instances }) {
		for (std::size_t st_next = 0; st_next < q_work->size(); ++st_next) {
			iw_active = &(*q_work)[st_next];
			completeInstance(*iw_active);
		}
	}
	iw_active = nullptr;
	checkStorage();
}

}
