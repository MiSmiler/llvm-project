//===--- PotentialNullPtrDerefCheck.h - clang-tidy --------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_HAHA_POTENTIALNULLPTRDEREFCHECK_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_HAHA_POTENTIALNULLPTRDEREFCHECK_H

#include "../ClangTidyCheck.h"

namespace clang::tidy::haha {

/// Detects potential null pointer dereferences.
///
/// This check uses dataflow analysis to track pointer nullness across control
/// flow and warns when a pointer that may be null is dereferenced without a
/// prior null check.
///
/// For the user-facing documentation see:
/// http://clang.llvm.org/extra/clang-tidy/checks/haha/potential-null-ptr-deref.html
class PotentialNullPtrDerefCheck : public ClangTidyCheck {
public:
  PotentialNullPtrDerefCheck(StringRef Name, ClangTidyContext *Context)
      : ClangTidyCheck(Name, Context) {}
  void registerMatchers(ast_matchers::MatchFinder *Finder) override;
  void check(const ast_matchers::MatchFinder::MatchResult &Result) override;
};

} // namespace clang::tidy::haha

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_HAHA_POTENTIALNULLPTRDEREFCHECK_H