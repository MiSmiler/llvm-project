//===--- HahaTidyModule.cpp - clang-tidy ----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "../ClangTidy.h"
#include "../ClangTidyModule.h"
#include "../ClangTidyModuleRegistry.h"
#include "PotentialNullPtrDerefCheck.h"

namespace clang::tidy {
namespace haha {

class HahaModule : public ClangTidyModule {
public:
  void addCheckFactories(ClangTidyCheckFactories &CheckFactories) override {
    // Checks will be registered here
    CheckFactories.registerCheck<PotentialNullPtrDerefCheck>(
        "haha-potential-null-ptr-deref");
  }
};

} // namespace haha

// Register the HahaModule using this statically initialized variable.
static ClangTidyModuleRegistry::Add<haha::HahaModule>
    X("haha-module", "Adds haha lint checks.");

// This anchor is used to force the linker to link in the generated object file
// and thus register the HahaModule.
volatile int HahaModuleAnchorSource = 0;

} // namespace clang::tidy