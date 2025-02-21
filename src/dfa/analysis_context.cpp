//===- analysis_context.cpp -------------------------------------------===//
//
// Copyright (c) 2024 Junjie Shen
//
// see https://github.com/shenjunjiekoda/knight/blob/main/LICENSE for
// license information.
//
//===------------------------------------------------------------------===//
//
//  This file implements the analysis context class.
//
//===------------------------------------------------------------------===//

#include "dfa/analysis_context.hpp"
#include "dfa/analysis/analysis_base.hpp"
#include "dfa/program_state.hpp"
#include "dfa/region/region.hpp"
#include "dfa/stack_frame.hpp"
#include "tooling/context.hpp"

namespace knight::dfa {

AnalysisContext::AnalysisContext(KnightContext& ctx,
                                 RegionManager& region_manager)
    : m_ctx(ctx), m_region_manager(region_manager) {}

RegionManager& AnalysisContext::get_region_manager() const {
    return m_region_manager;
}

clang::ASTContext& AnalysisContext::get_ast_context() const {
    return *m_ctx.get_ast_context();
}

clang::SourceManager& AnalysisContext::get_source_manager() const {
    return m_ctx.get_source_manager();
}

ProgramStateRef AnalysisContext::get_state() const {
    return m_state;
}

void AnalysisContext::set_state(ProgramStateRef state) {
    m_state = std::move(state);
}

const clang::Decl* AnalysisContext::get_current_decl() const {
    return m_frame->get_decl();
}

const StackFrame* AnalysisContext::get_current_stack_frame() const {
    return m_frame;
}

void AnalysisContext::set_current_stack_frame(const StackFrame* frame) {
    m_frame = frame;
}

} // namespace knight::dfa