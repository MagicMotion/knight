//===- program_state.cpp -------------------------------------------===//
//
// Copyright (c) 2024 Junjie Shen
//
// see https://github.com/shenjunjiekoda/knight/blob/main/LICENSE for
// license information.
//
//===------------------------------------------------------------------===//
//
//  This file implements the program state class.
//
//===------------------------------------------------------------------===//

#include "dfa/program_state.hpp"
#include "dfa/analysis/analysis_base.hpp"
#include "dfa/checker/checker_base.hpp"
#include "dfa/domain/dom_base.hpp"
#include "dfa/domain/domains.hpp"
#include "dfa/region/region.hpp"
#include "util/assert.hpp"

#include <llvm/ADT/BitVector.h>
#include <llvm/ADT/STLExtras.h>

#include <memory>
#include <optional>

namespace knight::dfa {

void retain_state(const ProgramState* state) {
    ++const_cast< ProgramState* >(state)->m_ref_cnt;
}

void release_state(const ProgramState* state) {
    knight_assert(state->m_ref_cnt > 0);
    auto* s = const_cast< ProgramState* >(state);
    if (--s->m_ref_cnt == 0) {
        auto& mgr = s->get_state_manager();
        mgr.m_state_set.RemoveNode(s);
        s->~ProgramState();
        mgr.m_free_states.push_back(s);
    }
}

ProgramState::ProgramState(ProgramStateManager* state_mgr,
                           RegionManager* region_mgr,
                           DomValMap dom_val,
                           RegionSExprMap region_sexpr,
                           StmtSExprMap stmt_sexpr)
    : m_state_mgr(state_mgr),
      m_region_mgr(region_mgr),
      m_ref_cnt(0),
      m_dom_val(std::move(dom_val)),
      m_region_sexpr(std::move(region_sexpr)),
      m_stmt_sexpr(std::move(stmt_sexpr)) {}

ProgramState::ProgramState(ProgramState&& other) noexcept
    : m_state_mgr(other.m_state_mgr),
      m_region_mgr(other.m_region_mgr),
      m_ref_cnt(0),
      m_dom_val(std::move(other.m_dom_val)),
      m_region_sexpr(std::move(other.m_region_sexpr)),
      m_stmt_sexpr(std::move(other.m_stmt_sexpr)) {}

ProgramStateManager& ProgramState::get_state_manager() const {
    return *m_state_mgr;
}

std::optional< MemRegionRef > ProgramState::get_region(
    ProcCFG::DeclRef decl, const StackFrame* frame) const {
    if (llvm::isa< clang::VarDecl >(decl)) {
        return get_region_manager().get_region(llvm::cast< clang::VarDecl >(
                                                   decl),
                                               frame);
    }
    llvm::errs() << "unhandled decl type: " << decl->getDeclKindName() << "\n";
    return std::nullopt;
}

ProgramStateRef ProgramState::set_region_sexpr(MemRegionRef region,
                                               SExprRef sexpr) const {
    auto region_sexpr = m_region_sexpr;
    region_sexpr[region] = sexpr;
    return get_state_manager()
        .get_persistent_state_with_copy_and_region_sexpr_map(*this,
                                                             region_sexpr);
}

ProgramStateRef ProgramState::set_stmt_sexpr(ProcCFG::StmtRef stmt,
                                             SExprRef sexpr) const {
    auto stmt_sexpr = m_stmt_sexpr;
    stmt_sexpr[stmt] = sexpr;
    return get_state_manager()
        .get_persistent_state_with_copy_and_stmt_sexpr_map(*this, stmt_sexpr);
}

std::optional< SExprRef > ProgramState::get_region_sexpr(
    MemRegionRef region) const {
    auto it = m_region_sexpr.find(region);
    if (it != m_region_sexpr.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::optional< SExprRef > ProgramState::get_stmt_sexpr(
    ProcCFG::StmtRef stmt) const {
    auto it = m_stmt_sexpr.find(stmt);
    if (it != m_stmt_sexpr.end()) {
        return it->second;
    }
    return std::nullopt;
}

ProgramStateRef ProgramState::normalize() const {
    DomValMap dom_val = m_dom_val;
    for (auto& [id, val] : dom_val) {
        const_cast< AbsDomBase* >(val.get())->normalize();
    }
    return get_state_manager()
        .get_persistent_state_with_copy_and_dom_val_map(*this,
                                                        std::move(dom_val));
}

bool ProgramState::is_bottom() const {
    return llvm::any_of(m_dom_val, [](const auto& pair) {
        return pair.second->is_bottom();
    });
}

bool ProgramState::is_top() const {
    return llvm::all_of(m_dom_val,
                        [](const auto& pair) { return pair.second->is_top(); });
}

ProgramStateRef ProgramState::set_to_bottom() const {
    return get_state_manager().get_bottom_state();
}

ProgramStateRef ProgramState::set_to_top() const {
    return get_state_manager().get_default_state();
}

// NOLINTNEXTLINE
#define UNION_MAP(OP)                                            \
    DomValMap new_map;                                           \
    for (const auto& [other_id, other_val] : other->m_dom_val) { \
        auto it = m_dom_val.find(other_id);                      \
        if (it == m_dom_val.end()) {                             \
            new_map[other_id] = other_val->clone_shared();       \
        } else {                                                 \
            auto new_val = it->second->clone();                  \
            new_val->OP(*other_val);                             \
            new_map[other_id] = SharedVal(new_val);              \
        }                                                        \
    }                                                            \
    return get_state_manager()                                   \
        .get_persistent_state_with_copy_and_dom_val_map(*this,   \
                                                        std ::move(new_map));
// NOLINTNEXTLINE
#define INTERSECT_MAP(OP)                                      \
    DomValMap map;                                             \
    for (auto& [other_id, other_val] : other->m_dom_val) {     \
        auto it = m_dom_val.find(other_id);                    \
        if (it != m_dom_val.end()) {                           \
            auto new_val = it->second->clone();                \
            new_val->OP(*other_val);                           \
            map[other_id] = SharedVal(new_val);                \
        }                                                      \
    }                                                          \
    return get_state_manager()                                 \
        .get_persistent_state_with_copy_and_dom_val_map(*this, \
                                                        std ::move(map));

ProgramStateRef ProgramState::join(const ProgramStateRef& other) const {
    UNION_MAP(join_with);
}

ProgramStateRef ProgramState::join_at_loop_head(
    const ProgramStateRef& other) const {
    UNION_MAP(join_with_at_loop_head);
}

ProgramStateRef ProgramState::join_consecutive_iter(
    const ProgramStateRef& other) const {
    UNION_MAP(join_consecutive_iter_with);
}

ProgramStateRef ProgramState::widen(const ProgramStateRef& other) const {
    UNION_MAP(widen_with);
}

ProgramStateRef ProgramState::meet(const ProgramStateRef& other) const {
    INTERSECT_MAP(meet_with);
}

ProgramStateRef ProgramState::narrow(const ProgramStateRef& other) const {
    INTERSECT_MAP(narrow_with);
}

bool ProgramState::leq(const ProgramState& other) const {
    llvm::BitVector this_key_set;
    bool need_to_check_other = other.m_dom_val.size() != this->m_dom_val.size();
    for (const auto& [id, val] : this->m_dom_val) {
        this_key_set.set(id);
        auto dom_val = m_dom_val;
        auto it = other.m_dom_val.find(id);
        if (it == other.m_dom_val.end()) {
            if (!val->is_bottom()) {
                return false;
            }
            need_to_check_other = true;
            continue;
        }
        if (!val->leq(*(it->second))) {
            return false;
        }
    }

    if (!need_to_check_other) {
        return true;
    }

    for (const auto& [id, val] : other.m_dom_val) {
        if (!this_key_set[id]) {
            if (!val->is_top()) {
                return false;
            }
        }
    }

    return true;
}

bool ProgramState::equals(const ProgramState& other) const {
    return llvm::all_of(this->m_dom_val, [&other](const auto& this_pair) {
        auto it = other.m_dom_val.find(this_pair.first);
        return it != other.m_dom_val.end() &&
               this_pair.second->equals(*(it->second));
    });
}

void ProgramState::dump(llvm::raw_ostream& os) const {
    os << "ProgramState:{\n";
    for (const auto& [id, aval] : m_dom_val) {
        os << "[" << get_domain_name_by_id(id) << "]: ";
        aval->dump(os);
        os << "\n";
    }
    os << "}\n";
}

ProgramStateRef ProgramStateManager::get_default_state() {
    DomValMap dom_val;
    for (auto analysis_id : m_analysis_mgr.get_required_analyses()) {
        for (auto dom_id :
             m_analysis_mgr.get_registered_domains_in(analysis_id)) {
            if (auto default_fn =
                    m_analysis_mgr.get_domain_default_val_fn(dom_id)) {
                dom_val[dom_id] = std::move((*default_fn)());
            }
        }
    }
    ProgramState state(this,
                       &m_region_mgr,
                       std::move(dom_val),
                       RegionSExprMap{},
                       StmtSExprMap{});

    return get_persistent_state(state);
}

ProgramStateRef ProgramStateManager::get_bottom_state() {
    DomValMap dom_val;
    for (auto analysis_id : m_analysis_mgr.get_required_analyses()) {
        for (auto dom_id :
             m_analysis_mgr.get_registered_domains_in(analysis_id)) {
            if (auto bottom_fn =
                    m_analysis_mgr.get_domain_bottom_val_fn(dom_id)) {
                dom_val[dom_id] = std::move((*bottom_fn)());
            }
        }
    }
    ProgramState state(this,
                       &m_region_mgr,
                       std::move(dom_val),
                       RegionSExprMap{},
                       StmtSExprMap{});

    return get_persistent_state(state);
}

ProgramStateRef ProgramStateManager::get_persistent_state(ProgramState& state) {
    llvm::FoldingSetNodeID id;
    state.Profile(id);
    void* insert_pos; // NOLINT

    if (ProgramState* existed =
            m_state_set.FindNodeOrInsertPos(id, insert_pos)) {
        return existed;
    }

    ProgramState* new_state = nullptr;
    if (!m_free_states.empty()) {
        new_state = m_free_states.back();
        m_free_states.pop_back();
    } else {
        new_state = m_alloc.Allocate< ProgramState >();
    }
    new (new_state) ProgramState(std::move(state));
    m_state_set.InsertNode(new_state, insert_pos);
    return new_state;
}

ProgramStateRef ProgramStateManager::
    get_persistent_state_with_ref_and_dom_val_map(ProgramState& state,
                                                  DomValMap dom_val) {
    state.m_dom_val = std::move(dom_val);
    return get_persistent_state(state);
}

ProgramStateRef ProgramStateManager::
    get_persistent_state_with_copy_and_dom_val_map(const ProgramState& state,
                                                   DomValMap dom_val) {
    ProgramState new_state(state.m_state_mgr,
                           state.m_region_mgr,
                           std::move(dom_val),
                           state.m_region_sexpr,
                           state.m_stmt_sexpr);
    return get_persistent_state(new_state);
}

ProgramStateRef ProgramStateManager::
    get_persistent_state_with_copy_and_region_sexpr_map(
        const ProgramState& state, RegionSExprMap region_sexpr) {
    ProgramState new_state(state.m_state_mgr,
                           state.m_region_mgr,
                           state.m_dom_val,
                           std::move(region_sexpr),
                           state.m_stmt_sexpr);

    return get_persistent_state(new_state);
}
ProgramStateRef ProgramStateManager::
    get_persistent_state_with_copy_and_stmt_sexpr_map(const ProgramState& state,
                                                      StmtSExprMap stmt_sexpr) {
    ProgramState new_state(state.m_state_mgr,
                           state.m_region_mgr,
                           state.m_dom_val,
                           state.m_region_sexpr,
                           std::move(stmt_sexpr));

    return get_persistent_state(new_state);
}

ProgramStateRef internal::get_persistent_state_with_copy_and_dom_val_map(
    ProgramStateManager& manager,
    const ProgramState& state,
    DomValMap dom_val) {
    return manager.get_persistent_state_with_copy_and_dom_val_map(state,
                                                                  std::move(
                                                                      dom_val));
}

} // namespace knight::dfa