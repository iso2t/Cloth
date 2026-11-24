// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <cstddef>
#include <deque>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <ast/decl/class_declaration.hpp>
#include <ast/type/type_reference.hpp>
#include <diagnostics.hpp>
#include <semantic/checked_program.hpp>
#include <semantic/checker.hpp>
#include <semantic/symbol_table.hpp>
#include <token/token.hpp>

namespace cloth::semantic {
namespace {

struct InterfaceVisit {
	const Symbol        *owner;
	const TypeReference *via;
	std::size_t          next = 0;
};

// Interface cycles are defined on declarations, even when an edge changes the type arguments.
void checkInterfaceCycles (const Analyzer &analyzer, const Candidates &types) {
	std::set<const Symbol *> s_complete;
	for (const Symbol *sym_root : types) {
		if (!asInterface(sym_root) || s_complete.contains(sym_root)) continue;
		std::vector<InterfaceVisit>           v_stack{ { .owner = sym_root, .via = nullptr } };
		std::map<const Symbol *, std::size_t> m_active{ { sym_root, 0 } };
		while (!v_stack.empty()) {
			InterfaceVisit    &iv_current = v_stack.back();
			const ResultTypes &v_parents  = analyzer.declarations.at(iv_current.owner).parents;
			if (iv_current.next == v_parents.size()) {
				s_complete.insert(iv_current.owner);
				m_active.erase(iv_current.owner);
				v_stack.pop_back();
				continue;
			}
			const std::size_t st_parent  = iv_current.next++;
			const Symbol     *sym_parent = v_parents[st_parent].declaration;
			if (s_complete.contains(sym_parent)) continue;
			const TypeReference                                                   &tr_parent = asInterface(iv_current.owner)->parents[st_parent];
			const std::pair<std::map<const Symbol *, std::size_t>::iterator, bool> it_start  = m_active.emplace(sym_parent, v_stack.size());
			if (!it_start.second) {
				std::vector<cde::SourceLabel> v_labels{ { .span = tr_parent.getSpan(), .text = "parent closes this cycle" },
					                                { .span = sym_parent->name.getSpan(), .text = "interface declared here" } };
				for (std::size_t st_index = it_start.first->second + 1; st_index < v_stack.size(); ++st_index)
					v_labels.push_back({ .span = v_stack[st_index].via->getSpan(), .text = "parent on the cycle" });
				throw cde::ClothDiagnostic(Code::InterfaceInheritanceCycle,
				                           std::format("Interface inheritance cycle involving '{}'", sym_parent->name.getLexeme()),
				                           std::move(v_labels));
			}
			v_stack.push_back({ .owner = sym_parent, .via = &tr_parent });
		}
	}
}

struct ClassBase {
	const ResolvedType  &pattern;
	const TypeReference &clause;
};

using ClassBases = std::map<const Symbol *, ClassBase>;

template <typename Node, typename Base> const Node *repeatedBase (const Node *root, std::set<const Node *> &complete, Base base) {
	std::set<const Node *> s_path;
	for (const Node *p_node = root; p_node && !complete.contains(p_node); p_node = base(p_node)) {
		if (!s_path.insert(p_node).second) return p_node;
	}
	complete.insert(s_path.begin(), s_path.end());
	return nullptr;
}

[[noreturn]] void classCycle (const Symbol &owner, const ClassBases &bases, const std::string &name) {
	throw cde::ClothDiagnostic(Code::InvalidClassMember, std::format("Inheritance cycle involving '{}'", name),
	                           { { .span = bases.at(&owner).clause.getName().getSpan(), .text = "class member here" } });
}

struct ClassApplication {
	const ResolvedType     &type;
	const ClassApplication *via;
	TokenSpan               use;
	std::size_t             depth;
	const ClassApplication *base = nullptr;
};

[[noreturn]] void classApplicationLimit (const GenericLimit &limit, const Symbol &declaration, const TokenSpan &use, const ClassApplication *via) {
	std::vector<GenericDependency> v_ancestry;
	for (; via && via->depth; via = via->via) v_ancestry.push_back({ .use = via->use, .name = via->type.declaration->name.getLexeme() });
	failGenericLimit(limit, declaration.name, use, v_ancestry);
}

// Expand only after finding a cyclic declaration chain. Distinct applications can converge, repeat, or exhaust the generic worklist limits.
void checkClassApplications (const Analyzer &analyzer, const Candidates &types, const ClassBases &bases) {
	using Applications = std::map<ResolvedType, ClassApplication *>;
	using Intern       = std::function<ClassApplication *(ResolvedType, const TypeReference *, const ClassApplication *)>;
	std::deque<ClassApplication> q_applications;
	Applications                 m_types;
	std::size_t                  st_open = 0, st_closed = 0;
	const Intern                 intern = [&] (ResolvedType type, const TypeReference *clause, const ClassApplication *via) {
                if (clause) {
                        // Written argument applications are dependencies of the same owner and are resolved before the enclosing base.
                        const std::vector<TypeReference> &v_written = clause->getTypeArguments();
                        for (std::size_t st_arg = 0; st_arg < v_written.size(); ++st_arg) {
                                if (v_written[st_arg].getApplication() && type.arguments()[st_arg].declaration)
                                        intern(type.arguments()[st_arg], &v_written[st_arg], via);
                        }
                        type.suffixes.clear();
                }
                const std::pair<Applications::iterator, bool> it_type = m_types.try_emplace(std::move(type));
                if (it_type.second) {
                        const ResolvedType &rt_type  = it_type.first->first;
                        const TokenSpan     ts_use   = clause ? clause->getSpan() : rt_type.declaration->name.getSpan();
                        std::size_t         st_depth = 0;
                        if (!rt_type.arguments().empty()) {
                                st_depth              = (via ? via->depth : 0) + 1;
                                std::size_t &st_count = rt_type.dependent() ? st_open : st_closed;
                                if (const std::optional<GenericLimit> opt_limit =
                                            genericLimit(rt_type.argumentDepth(), st_depth, st_count, rt_type.dependent()))
                                        classApplicationLimit(*opt_limit, *rt_type.declaration, ts_use, via);
                                ++st_count;
                        }
                        it_type.first->second = &q_applications.emplace_back(rt_type, via, ts_use, st_depth);
                }
                return it_type.first->second;
	};
	for (const bool b_generic : { false, true }) {
		for (const Symbol *sym : types) {
			if ((typeParameters(*sym) != nullptr) == b_generic) intern(analyzer.ownerType(*sym), nullptr, nullptr);
		}
	}
	// Resolving a base can append more applications to this deque.
	std::size_t next = 0;
	while (next < q_applications.size()) {
		ClassApplication &ca_type = q_applications[next++];
		const auto        it_base = bases.find(ca_type.type.declaration);
		if (it_base == bases.end()) continue;
		const ClassBase &cb_base = it_base->second;
		ca_type.base             = intern(analyzer.substitute(cb_base.pattern, analyzer.argumentsOf(ca_type.type)), &cb_base.clause, &ca_type);
	}
	std::set<const ClassApplication *> s_complete;
	for (const ClassApplication &ca_type : q_applications) {
		if (const ClassApplication *ca_repeat =
		            repeatedBase(&ca_type, s_complete, [] (const ClassApplication *application) { return application->base; })) {
			const Symbol &sym_repeat = *ca_repeat->type.declaration;
			classCycle(sym_repeat, bases, sym_repeat.name.getLexeme());
		}
	}
}
}

void Analyzer::checkInheritance () const {
	ClassBases m_bases;
	Candidates v_types;
	for (const Symbol *sym_type : v_order) {
		const Symbol           &sym      = *sym_type;
		const ClassDeclaration *cd_class = asClass(&sym);
		if (sym.kind == SymbolKind::Type) v_types.push_back(&sym);
		const ClassDeclaration                *base = nullptr;
		std::map<const Symbol *, ResolvedType> m_direct;
		const ResultTypes                     &v_parents = declarations.at(&sym).parents;
		for (std::size_t st_parent = 0; st_parent < v_parents.size(); ++st_parent) {
			const ResolvedType &parent = v_parents[st_parent];
			if (!parent.declaration || !parent.suffixes.empty()) {
				fail(sym.kind == SymbolKind::State ? Code::InvalidStateAttachment : Code::InvalidInterfaceHeader, sym.name.getSpan(),
				     "Expected a named parent type");
			}
			const std::pair<std::map<const Symbol *, ResolvedType>::iterator, bool> it_parent = m_direct.emplace(parent.declaration, parent);
			if (!it_parent.second) {
				if (it_parent.first->second != parent)
					fail(Code::ConflictingInterfaceApplication, sym.name.getSpan(),
					     "Conflicting applications of " + it_parent.first->second.spelling() + " and " + parent.spelling(),
					     &parent.declaration->name);
				fail(Code::InvalidInterfaceHeader, sym.name.getSpan(), "Duplicate parent type", &parent.declaration->name);
			}
			const ClassDeclaration *parentClass = asClass(parent.declaration);
			if (sym.kind == SymbolKind::State) {
				if (!parentClass && !asInterface(parent.declaration)) {
					fail(Code::InvalidStateAttachment, sym.name.getSpan(), "A state needs a class or interface host",
					     &parent.declaration->name);
				}
			} else if (parentClass && cd_class) {
				if (base || parentClass->finalKeyword) {
					fail(Code::InvalidClassMember, sym.name.getSpan(),
					     base ? "A class can have only one base class" : "Cannot inherit a final class", &parent.declaration->name);
				}
				base = parentClass;
				if (parent.declaration != &objectRoot())
					m_bases.emplace(&sym, ClassBase{ .pattern = parent, .clause = cd_class->parents[st_parent].type });
			} else if (!asInterface(parent.declaration)) {
				fail(Code::InvalidInterfaceHeader, sym.name.getSpan(), "Invalid parent type", &parent.declaration->name);
			}
		}
	}
	// The symbol table is keyed by name. Diagnostics follow written declaration and parent order.
	std::ranges::sort(v_types, {}, [&] (const Symbol *symbol) {
		const TokenSpan ts_name = symbol->name.getSpan();
		return std::tuple{ declarations.at(symbol).file, ts_name.startLine, ts_name.startCol };
	});
	checkInterfaceCycles(*this, v_types);
	std::set<const Symbol *> s_complete;
	for (const Symbol *sym_root : v_types) {
		const Symbol *sym_repeat = repeatedBase(sym_root, s_complete, [&] (const Symbol *symbol) -> const Symbol * {
			const auto it_base = m_bases.find(symbol);
			return it_base == m_bases.end() ? nullptr : it_base->second.pattern.declaration;
		});
		if (!sym_repeat) continue;
		if (!m_genericOwners.empty()) return checkClassApplications(*this, v_types, m_bases);
		std::string s_name = sym_repeat->name.getLexeme();
		if (!usesSpecializedDeclarations()) {
			for (const Symbol *sym_outer = declarations.at(sym_repeat).owner; sym_outer; sym_outer = declarations.at(sym_outer).owner)
				s_name = std::format("{}{}{}", sym_outer->name.getLexeme(), toString(ODot), s_name);
		}
		classCycle(*sym_repeat, m_bases, s_name);
	}
}
}
