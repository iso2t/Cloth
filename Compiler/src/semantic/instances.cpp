// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/language/types/Generics.rst

#include <algorithm>
#include <compare>
#include <cstddef>
#include <deque>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <ast/decl/declarations.hpp>
#include <ast/type/type_parameters.hpp>
#include <diagnostics.hpp>
#include <file/source_file.hpp>
#include <semantic/checked_program.hpp>
#include <semantic/checker.hpp>
#include <semantic/symbol_table.hpp>
#include <token/token.hpp>

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

// Diagnostic traversal follows source order, including the origins of nested type arguments.
struct StorageTypeOrder {
	const std::unordered_map<const Symbol *, DeclarationInfo>                  &declarations;
	const std::unordered_map<const TypeParameterDeclaration *, const Symbol *> &parameterOwners;

	std::tuple<std::size_t, std::size_t, std::size_t> position (const ResolvedType &type) const {
		if (!type.declaration && !type.parameter) return {};
		const Symbol   &sym_owner = type.parameter ? *parameterOwners.at(type.parameter) : *type.declaration;
		const TokenSpan ts_name   = (type.parameter ? type.parameter->name : sym_owner.name).getSpan();
		return { declarations.at(&sym_owner).file, ts_name.startLine, ts_name.startCol };
	}

	std::strong_ordering operator()(const ResolvedType &left, const ResolvedType &right) const {
		if (left.declaration != right.declaration || left.parameter != right.parameter) {
			if (const std::strong_ordering so_origin = position(left) <=> position(right); so_origin != 0) return so_origin;
		}
		const std::strong_ordering so_type = std::tie(left.name, left.width, left.suffixes, left.projectedSuffixes) <=>
		                                     std::tie(right.name, right.width, right.suffixes, right.projectedSuffixes);
		if (so_type != 0 || left.application == right.application) return so_type;
		return std::lexicographical_compare_three_way(left.arguments().begin(), left.arguments().end(), right.arguments().begin(),
		                                              right.arguments().end(), *this);
	}
};
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
	InstanceKey ik_key{ .declaration = &symbol, .receiver = std::nullopt, .arguments = {}, .operatorWidth = operatorWidth };
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
	InstanceKey ik_key{ .declaration = dispatch.contract.symbol, .receiver = dispatch.receiver, .arguments = {} };
	for (const TypeParameterDeclaration &tp_parameter : tpl_parameters->parameters) ik_key.arguments.push_back(arguments.at(&tp_parameter));
	const std::pair<DispatchUses::iterator, bool> it_use = m_dispatchUses.try_emplace(std::move(ik_key), DispatchUse{ .call = dispatch, .where = where });
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
		enqueue({ .declaration = body.symbol, .receiver = body.owner, .arguments = key.arguments }, use.where, false);
	};
	if (use.call.kind == DispatchKind::State) {
		if (const StateMethod *method = td_table.stateTarget(use.call.contract))
			for (const auto &[selection, body] : method->implementations) instantiate(body);
	} else if (const MethodRef *body = td_table.methodTarget(use.call)) {
		instantiate(*body);
	}
}

const GenericInstance *Analyzer::guaranteeOwner (const Symbol &guarantee, const Substitutions &arguments, const Symbol &context, const TokenSpan &where) {
	const Symbol &sym_owner = *declarations.at(&guarantee).owner;
	if (sym_owner.kind != SymbolKind::Type) return useCallable(sym_owner, arguments, context, where);
	return useInstance({ .declaration = &sym_owner, .receiver = std::nullopt, .arguments = substitute(ownerType(sym_owner), arguments).arguments() },
	                   context, where);
}

void Analyzer::useType (const ResolvedType &type, const Symbol &context, const TokenSpan &where) {
	type.visitApplications([&] (const ResolvedType &rtType) {
		useFormatterClass(rtType);
		if (rtType.declaration)
			useInstance({ .declaration = rtType.declaration, .receiver = std::nullopt, .arguments = rtType.arguments() }, context, where);
	});
}

const GenericInstance *Analyzer::useInstance (InstanceKey key, const Symbol &context, const TokenSpan &where) {
	if (!iw_active && (isTemplate(context) || (rt_activeAttachment && rt_activeAttachment->dependent()))) {
		m_uses[&context].push_back({ .key = std::move(key), .where = where });
		return nullptr;
	}
	const bool b_symbolic = iw_active && !iw_active->instance;
	if (!b_symbolic && dependsOnTypeParameter(key)) {
		fail(Code::GenericInferenceFailed, where, "Cannot determine the type arguments of '" + key.declaration->name.getLexeme() + "'",
		     &key.declaration->name);
	}
	return enqueue(std::move(key), where, b_symbolic);
}

std::optional<GenericLimit> genericLimit (unsigned nesting, std::size_t depth, std::size_t count, bool symbolic) {
	if (nesting > MAX_TYPE_ARG_NESTING)
		return GenericLimit{ .code    = Code::GenericSubstitutionNesting,
			             .message = std::format("Generic type argument nesting limit exceeded ({})", MAX_TYPE_ARG_NESTING) };
	if (depth > MAX_GENERIC_DEPENDENCIES)
		return GenericLimit{ .code    = symbolic ? Code::GenericTemplateDependencyLimit : Code::GenericDependencyLimit,
			             .message = std::format("Generic {} dependency limit exceeded ({})", symbolic ? "template analysis" : "instantiation",
			                                    MAX_GENERIC_DEPENDENCIES) };
	if (count >= MAX_GENERIC_INSTANCES)
		return GenericLimit{ .code    = symbolic ? Code::GenericTemplateInstanceLimit : Code::GenericInstanceLimit,
			             .message = std::format("Too many {} generic instances ({})", symbolic ? "symbolic" : "distinct", MAX_GENERIC_INSTANCES) };
	return {};
}

void failGenericLimit (const GenericLimit &limit, const Token &declaration, const TokenSpan &use, const std::vector<GenericDependency> &ancestry) {
	std::vector<cde::SourceLabel>                                                                v_labels;
	std::set<std::tuple<const SourceFile *, std::size_t, std::size_t, std::size_t, std::size_t>> s_labeled;
	const std::function                                                                          add = [&] (const TokenSpan &span, std::string text) {
                if (s_labeled.emplace(span.source.get(), span.startLine, span.startCol, span.endLine, span.endCol).second)
                        v_labels.push_back({ .span = span, .text = std::move(text) });
	};
	add(use, "this application exceeds the generic resource limit");
	add(declaration.getSpan(), "generic template declared here");
	for (std::size_t st_index = 0; st_index < ancestry.size(); ++st_index) {
		const GenericDependency &gd_step = ancestry[ancestry.size() - st_index - 1];
		add(gd_step.use, std::format("dependency {}: '{}' required here", st_index + 1, gd_step.name));
	}
	throw cde::ClothDiagnostic(limit.code, limit.message, std::move(v_labels));
}

const GenericInstance *Analyzer::enqueue (InstanceKey key, const TokenSpan &where, bool symbolic) {
	const InstanceKey *ik_stored   = nullptr;
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
			if (!iw_step->key.arguments.empty())
				v_ancestry.push_back({ .use = iw_step->where, .name = iw_step->key.declaration->name.getLexeme() });
		failGenericLimit(*opt_limit, sym_declaration.name, where, v_ancestry);
	}
	st_count += b_charged;
	(symbolic ? q_templates : q_instances).push_back({ .key = *ik_stored, .instance = gi_instance, .parent = iw_active, .where = where, .depth = st_depth });
	return gi_instance;
}

void Analyzer::collectSignature (const Symbol &symbol) {
	const DeclarationInfo &di_info = declarations.at(&symbol);
	useDeclaredBounds(symbol, {});
	if (const CallableSignature *cs_signature = callable(symbol)) {
		for (std::size_t st_index = 0; st_index < di_info.parameters.size(); ++st_index) {
			useType(di_info.parameters[st_index], symbol, cs_signature->parameters[st_index].type.getSpan());
		}
		for (std::size_t st_index = 0; st_index < di_info.returns.size(); ++st_index) {
			useType(di_info.returns[st_index], symbol,
			        st_index < cs_signature->returnTypes.size() ? cs_signature->returnTypes[st_index].getSpan() : symbol.name.getSpan());
		}
	}
	if (di_info.value) useType(*di_info.value, symbol, symbol.name.getSpan());
	for (const ResolvedType &rt_parent : di_info.parents) useType(rt_parent, symbol, symbol.name.getSpan());
}

void Analyzer::useDeclaredBounds (const Symbol &symbol, const Substitutions &arguments) {
	const TypeParameterList *tpl_parameters = typeParameters(symbol);
	if (!tpl_parameters) return;
	for (const TypeParameterDeclaration &tp_parameter : tpl_parameters->parameters) {
		for (const TypeParameterBound &tp_bound : tp_parameter.bounds) {
			if (const TypeReference *tr_bound = std::get_if<TypeReference>(&tp_bound.value)) {
				useType(substitute(type(*tr_bound, symbol), arguments), symbol, tr_bound->getSpan());
			}
		}
		if (tp_parameter.defaultType) {
			useType(substitute(type(*tp_parameter.defaultType, symbol), arguments), symbol, tp_parameter.defaultType->getSpan());
		}
	}
}

void Analyzer::visitTypeMembers (const ResolvedType &type, const Substitutions &arguments, GenericInstance *instance) {
	const Symbol  &sym_type = *type.declaration;
	StorageFields *v_fields = nullptr;
	if (asStruct(&sym_type)) {
		const std::pair<std::map<ResolvedType, StorageFields>::iterator, bool> it_layout = m_storage.try_emplace(type);
		if (it_layout.second) v_fields = &it_layout.first->second;
	}
	for (const SymbolScope::value_type &entry : sym_type.members) {
		for (const Symbol &sym_member : entry.second) {
			if (iw_active && !typeParameters(sym_member)) checkOverload(sym_member, arguments, &iw_active->where);
			if (sym_member.kind == SymbolKind::Field) {
				DeclarationInfo di_field = specialize(declarations.at(&sym_member), arguments);
				ResolvedType    rt_field = *di_field.value;
				if (iw_active) useType(rt_field, sym_member, sym_member.name.getSpan());
				if (instance) {
					instance->fields.emplace(&sym_member, rt_field);
					BodyChecker(*this, sym_member, instance->body, arguments, di_field).check();
				}
				if (v_fields) v_fields->emplace_back(std::move(rt_field), &sym_member);
			} else if (iw_active && !typeParameters(sym_member) &&
			           (callable(sym_member) || sym_member.kind == SymbolKind::State || sym_member.kind == SymbolKind::StateAttachment)) {
				useCallable(sym_member, arguments, sym_member, sym_member.name.getSpan());
			}
		}
	}
	if (v_fields) {
		std::ranges::sort(*v_fields, {}, [] (const StorageFields::value_type &field) {
			const TokenSpan ts_field = field.symbol->name.getSpan();
			return std::pair{ ts_field.startLine, ts_field.startCol };
		});
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
		const ParameterContract pc_contract     = parameterContract(*entry.first);
		const auto              it_requirements = m_guarantees.find(pc_contract.parameter);
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
                const auto it_uses = m_uses.find(&owner);
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
						if (!typeParameters(sym_member))
							enqueue({ .declaration = &sym_member, .receiver = rt_type, .arguments = {} }, work.where, false);
					}
				}
			}
		}
	} else if (sym_declaration.kind == SymbolKind::State) {
		for (const SymbolScope::value_type &entry : sym_declaration.members) {
			for (const Symbol &sym_member : entry.second) {
				if (!typeParameters(sym_member)) useCallable(sym_member, m_arguments, sym_member, sym_member.name.getSpan());
			}
		}
	}
	if (const TypeParameterList *tpl_parameters = typeParameters(sym_declaration)) {
		for (const TypeParameterDeclaration &tp_parameter : tpl_parameters->parameters) {
			const auto it_requirements = m_guarantees.find(&tp_parameter);
			if (it_requirements == m_guarantees.end()) continue;
			for (const Symbol *sym_requirement : it_requirements->second) {
				if (!work.instance) {
					dependencies(*sym_requirement);
				} else {
					DeclarationInfo &di_requirement =
					        work.instance->constructorContracts
					                .emplace(sym_requirement, specialize(declarations.at(sym_requirement), m_arguments))
					                .first->second;
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
	using Layout = std::map<ResolvedType, StorageFields>::value_type;

	struct StorageVisit {
		const Layout *node;
		std::size_t   next = 0;
	};

	std::vector<const Layout *> v_roots, v_postorder;
	v_roots.reserve(m_storage.size());
	for (const Layout &layout : m_storage) v_roots.push_back(&layout);
	const StorageTypeOrder compare{ .declarations = declarations, .parameterOwners = m_genericOwners };
	std::ranges::sort(v_roots, [&] (const Layout *left, const Layout *right) { return compare(left->first, right->first) < 0; });

	// Only a type already on the current field path makes the layout recursive.
	std::set<const Layout *>  s_active, s_done;
	std::vector<StorageVisit> v_path;
	for (const Layout *root : v_roots) {
		if (s_done.contains(root)) continue;
		s_active.insert(root);
		v_path.push_back({ .node = root });
		while (!v_path.empty()) {
			StorageVisit &sv_current = v_path.back();
			const Layout *sl_node    = sv_current.node;
			if (sv_current.next == sl_node->second.size()) {
				if (!sl_node->first.dependent()) v_postorder.push_back(sl_node);
				s_active.erase(sl_node);
				s_done.insert(sl_node);
				v_path.pop_back();
				continue;
			}
			const StorageFields::value_type &field = sl_node->second[sv_current.next++];
			if (!asStruct(field.type.declaration)) continue;
			ResolvedType rt_nested = field.type;
			rt_nested.suffixes.clear();
			const auto it_child = m_storage.find(rt_nested);
			if (it_child == m_storage.end()) throw std::logic_error("Missing struct field specialization");
			const Layout *sl_child = &*it_child;
			if (s_done.contains(sl_child)) continue;
			if (!s_active.insert(sl_child).second) {
				const Token &t_name = rt_nested.declaration->name;
				fail(Code::RecursiveStruct, field.symbol->name.getSpan(),
				     std::format("Recursive storage in {} '{}'", toString(KStruct), t_name.getLexeme()), &t_name);
			}
			v_path.push_back({ .node = sl_child });
		}
	}
	for (const Layout *sl_struct : v_postorder) layoutStruct(sl_struct->first, sl_struct->second);
	for (const ArrayAllocation &aa_literal : v_arrayLiterals) {
		if (!program.arrayDataLayout(aa_literal.type, aa_literal.count))
			fail(Code::InvalidArray, aa_literal.where,
			     std::format("Array element storage exceeds {} on a {}-bit target", toString(KUsize), program.layout.sizeBits));
	}
}

void Analyzer::checkInstances () {
	for (const Symbol *sym_declaration : v_order) {
		const TypeParameterList *tpl_parameters = typeParameters(*sym_declaration);
		if (tpl_parameters) {
			InstanceKey ik_key{ .declaration = sym_declaration, .receiver = std::nullopt, .arguments = {} };
			for (const TypeParameterDeclaration &tp_parameter : tpl_parameters->parameters) {
				ResolvedType rt_argument(tp_parameter.name.getNameKey());
				rt_argument.parameter = &tp_parameter;
				ik_key.arguments.push_back(std::move(rt_argument));
			}
			if (callable(*sym_declaration)) {
				if (const Symbol *sym_owner = instanceOwner(*sym_declaration)) ik_key.receiver = ownerType(*sym_owner);
			}
			enqueue(std::move(ik_key), sym_declaration->name.getSpan(), true);
		} else if (asStruct(sym_declaration)) {
			visitTypeMembers(ownerType(*sym_declaration), {}, nullptr);
		} else if (asClass(sym_declaration)) {
			registerReceiver(ownerType(*sym_declaration));
		}
	}
	// A map entry is the provisional identity. Each new entry appends one job, including calls discovered in another body.
	for (std::deque<InstanceWork> *q_work : { &q_templates, &q_instances }) {
		std::size_t next = 0;
		while (next < q_work->size()) {
			iw_active = &(*q_work)[next++];
			completeInstance(*iw_active);
		}
	}
	iw_active = nullptr;
	checkStorage();
}
}
