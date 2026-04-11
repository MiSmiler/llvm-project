set(LLVM_LINK_COMPONENTS
  Support
  )

add_clang_library(clangTidyHahaModule
  HahaTidyModule.cpp
  PotentialNullPtrDerefCheck.cpp

  LINK_LIBS
  clangTidy
  clangTidyUtils

  DEPENDS
  )

clang_target_link_libraries(clangTidyHahaModule
  PRIVATE
  clangAnalysis
  clangAnalysisFlowSensitive
  clangAST
  clangASTMatchers
  clangBasic
  clangLex
  )