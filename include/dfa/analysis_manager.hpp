//===- analysis_manager.hpp -------------------------------------------===//
//
// Copyright (c) 2024 Junjie Shen
//
// see https://github.com/shenjunjiekoda/knight/blob/main/LICENSE for
// license information.
//
//===------------------------------------------------------------------===//
//
//  This header defines the analysis manager class.
//
//===------------------------------------------------------------------===//

#pragma once

#include "dfa/analysis/analyses.hpp"
#include "dfa/domain/dom_base.hpp"
#include "dfa/proc_cfg.hpp"
#include "dfa/region/region.hpp"
#include "tooling/context.hpp"
#include "util/assert.hpp"

#include <llvm/Support/raw_ostream.h>

#include <memory>
#include <unordered_set>

namespace knight::dfa {

class AnalysisBase;
class AnalysisContext;

using UniqueAnalysisRef = std::unique_ptr< AnalysisBase >;
using AnalysisRef = AnalysisBase*;
using AnalysisRefs = std::vector< AnalysisRef >;

using AnalysisIDSet = std::unordered_set< AnalysisID >;
using AnalysisNameRef = llvm::StringRef;

template < typename T >
class AnalysisCallBack;

template < typename RET, typename... Args >
class AnalysisCallBack< RET(Args...) > {
  public:
    using CallBack = RET (*)(void*, Args...);

  private:
    CallBack m_callback;
    AnalysisBase* m_analysis;
    AnalysisKind m_kind;

  public:
    AnalysisCallBack(AnalysisKind kind,
                     AnalysisBase* analysis,
                     CallBack callback)
        : m_kind(kind), m_analysis(analysis), m_callback(callback) {}

    [[nodiscard]] AnalysisID get_id() const { return get_analysis_id(m_kind); }

    [[nodiscard]] RET operator()(Args... args) const {
        return m_callback(m_analysis, args...);
    }
}; // class AnalysisCallBack

namespace internal {

using ExitNodeRef = ProcCFG::NodeRef;
using StmtRef = ProcCFG::StmtRef;

using AnalyzeBeginFunctionCallBack = AnalysisCallBack< void(AnalysisContext&) >;

using AnalyzeEndFunctionCallBack =
    AnalysisCallBack< void(ExitNodeRef, AnalysisContext&) >;

using AnalyzeStmtCallBack = AnalysisCallBack< void(StmtRef, AnalysisContext&) >;
using MatchStmtCallBack = bool (*)(StmtRef S);
enum class VisitStmtKind { Pre, Eval, Post };

constexpr unsigned AlignedSize = 64;

struct StmtAnalysisInfo {
    AnalyzeStmtCallBack anz_cb;
    MatchStmtCallBack match_cb;
    VisitStmtKind kind;
} __attribute__((aligned(AlignedSize)))
__attribute__((packed)); // struct StmtAnalysisInfo

} // namespace internal

class ProgramStateManager;

/// \brief The analysis manager which holds all the registered analyses.
///
class AnalysisManager {
    friend class CheckerManager;

  private:
    KnightContext& m_ctx;

    /// \brief analyses
    AnalysisIDSet m_analyses; // all analyses
    std::vector< AnalysisID >
        m_analysis_full_order; // subject to analysis dependencies
    std::unordered_map< AnalysisID, AnalysisIDSet >
        m_analysis_dependencies;          // all analysis dependencies
    AnalysisIDSet m_priviledged_analysis; // priviledged analysis

    std::unordered_map< AnalysisID, std::unique_ptr< AnalysisBase > >
        m_enabled_analyses;            // enabled analysis shall be created.
    AnalysisIDSet m_required_analyses; // all analyses should be created,
    // shall be equivalent with enabled analyses key set.

    std::unique_ptr< dfa::RegionManager > m_region_mgr;
    std::unique_ptr< dfa::ProgramStateManager > m_state_mgr;

    /// \brief registered domains
    std::unordered_map< DomID, AnalysisID > m_domains;
    using DomainDefaultValFn = std::function< SharedVal() >;
    using DomainBottomValFn = std::function< SharedVal() >;
    std::unordered_map< DomID, DomainDefaultValFn > m_domain_default_fn;
    std::unordered_map< DomID, DomainBottomValFn > m_domain_bottom_fn;
    std::unordered_map< AnalysisID, std::unordered_set< DomID > >
        m_analysis_domains;

    /// \brief visit begin function callbacks
    std::vector< internal::AnalyzeBeginFunctionCallBack >
        m_begin_function_analyses;
    /// \brief visit end function callbacks
    std::vector< internal::AnalyzeEndFunctionCallBack > m_end_function_analyses;
    /// \brief visit statement callbacks
    std::vector< internal::StmtAnalysisInfo > m_stmt_analyses;

  public:
    explicit AnalysisManager(KnightContext& ctx);
    ~AnalysisManager() = default;

  public:
    /// \brief specialized analysis management
    ///
    /// Dependencies shall be handled before the registration.
    /// @{
    template < typename ANALYSIS, typename... AT >
    [[nodiscard]] UniqueAnalysisRef register_analysis(KnightContext& ctx,
                                                      AT&&... Args) {
        AnalysisID id = get_analysis_id(ANALYSIS::get_kind());
        if (m_analyses.contains(id)) {
            llvm::errs() << get_analysis_name_by_id(id)
                         << " analysis is already registered.\n";
        } else {
            m_analyses.insert(id);
        }

        auto analysis =
            std::make_unique< ANALYSIS >(ctx, std::forward< AT >(Args)...);
        ANALYSIS::register_callback(analysis.get(), *this);
        return std::move(analysis);
    }

    void add_required_analysis(AnalysisID id);
    void add_analysis_dependency(AnalysisID id, AnalysisID required_id);
    [[nodiscard]] AnalysisIDSet get_analysis_dependencies(AnalysisID id) const;

    template < typename Analysis >
    void set_analysis_priviledged() {
        auto analysis_id = get_analysis_id(Analysis::get_kind());
        m_priviledged_analysis.insert(analysis_id);
        m_required_analyses.insert(analysis_id);
    }

    void enable_analysis(std::unique_ptr< AnalysisBase > analysis);
    [[nodiscard]] bool is_analysis_required(AnalysisID id) const;

    std::optional< AnalysisBase* > get_analysis(AnalysisID id);
    /// @}

    /// \brief domain management
    ///
    /// Analysis shall be registered first.
    /// Domain dependencies shall be handled before the registration.
    /// @{
    template < typename Analysis, typename Dom >
    void add_domain_dependency() {
        auto analysis_id = get_analysis_id(Analysis::get_kind());
        auto dom_id = get_domain_id(Dom::get_kind());
        m_domains[dom_id] = analysis_id;
        m_domain_default_fn[dom_id] = Dom::default_val;
        m_domain_bottom_fn[dom_id] = Dom::bottom_val;
        m_analysis_domains[analysis_id].insert(dom_id);
    }
    [[nodiscard]] std::unordered_set< DomID > get_registered_domains_in(
        AnalysisID id) const;
    [[nodiscard]] std::optional< DomainDefaultValFn > get_domain_default_val_fn(
        DomID id) const;
    [[nodiscard]] std::optional< DomainBottomValFn > get_domain_bottom_val_fn(
        DomID id) const;
    /// @}

    /// \brief callback registrations
    /// @{
    void register_for_begin_function(internal::AnalyzeBeginFunctionCallBack cb);
    void register_for_end_function(internal::AnalyzeEndFunctionCallBack cb);
    void register_for_stmt(internal::AnalyzeStmtCallBack cb,
                           internal::MatchStmtCallBack match_cb,
                           internal::VisitStmtKind kind);
    /// @}

    [[nodiscard]] const std::vector< internal::AnalyzeBeginFunctionCallBack >&
    begin_function_analyses() const {
        return m_begin_function_analyses;
    }

    [[nodiscard]] const std::vector< internal::AnalyzeEndFunctionCallBack >&
    end_function_analyses() const {
        return m_end_function_analyses;
    }

    [[nodiscard]] const std::vector< internal::StmtAnalysisInfo >&
    stmt_analyses() const {
        return m_stmt_analyses;
    }

    [[nodiscard]] const AnalysisIDSet& get_required_analyses() const {
        return m_required_analyses;
    }

    [[nodiscard]] dfa::RegionManager& get_region_manager() const {
        return *m_region_mgr;
    }
    [[nodiscard]] dfa::ProgramStateManager& get_state_manager() const;
    [[nodiscard]] KnightContext& get_context() const { return m_ctx; }

    void compute_all_required_analyses_by_dependencies();
    void compute_full_order_analyses_after_registry();
    [[nodiscard]] std::vector< AnalysisID > get_ordered_analyses(
        AnalysisIDSet ids) const;

    void run_analyses_for_stmt(AnalysisContext& analysis_ctx,
                               internal::StmtRef stmt,
                               internal::VisitStmtKind visit_kind);
    void run_analyses_for_pre_stmt(AnalysisContext& analysis_ctx,
                                   internal::StmtRef stmt);
    void run_analyses_for_eval_stmt(AnalysisContext& analysis_ctx,
                                    internal::StmtRef stmt);
    void run_analyses_for_post_stmt(AnalysisContext& analysis_ctx,
                                    internal::StmtRef stmt);
    void run_analyses_for_begin_function(AnalysisContext& analysis_ctx);
    void run_analyses_for_end_function(AnalysisContext& analysis_ctx,
                                       ProcCFG::NodeRef node);

}; // class AnalysisManager

} // namespace knight::dfa
